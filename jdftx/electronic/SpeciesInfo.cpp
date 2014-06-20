/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Deniz Gunceler
Copyright 1996-2003 Sohrab Ismail-Beigi

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/SpeciesInfo.h>
#include <electronic/SpeciesInfo_internal.h>
#include <electronic/Everything.h>
#include <electronic/matrix.h>
#include <electronic/operators.h>
#include <electronic/symbols.h>
#include <electronic/ColumnBundle.h>
#include <core/LatticeUtils.h>
#include <core/DataMultiplet.h>
#include <fstream>
#include <sstream>


void SpeciesInfo::sync_atpos()
{	if(!atpos.size()) return; //unused species
	#ifdef GPU_ENABLED
	//Transfer atomic positions to the GPU:
	cudaMemcpy(atposGpu, &atpos[0], sizeof(vector3<>)*atpos.size(), cudaMemcpyHostToDevice);
	gpuErrorCheck();
	#endif
	//Invalidate cached projectors:
	cachedV.clear();
}

inline bool isParallel(vector3<> x, vector3<> y)
{	return fabs(1.-fabs(dot(x, y)/(x.length() * y.length()))) < symmThreshold;
}

bool SpeciesInfo::Constraint::isEquivalent(const Constraint& otherConstraint, const matrix3<>& transform) const
{ 	if(moveScale != otherConstraint.moveScale) return false; //Ensure same moveSCale
	if(type != otherConstraint.type) return false; //Ensure same constraint type
	return (type==None) or isParallel(transform*d, otherConstraint.d);
}

int SpeciesInfo::Constraint::getDimension() const
{	if(not moveScale) return 0;
	switch(type)
	{	case Linear: return 1;
		case Planar: return 2;
		default: return 3;
	}
}

void SpeciesInfo::Constraint::print(FILE* fp, const Everything& e) const
{	vector3<> d = this->d; //in cartesian coordinates
	if(e.iInfo.coordsType == CoordsLattice)
		d = ~(e.gInfo.R) * d; //print in lattice coordinates
	fprintf(fp, "  %s %.14lg %.14lg %.14lg", constraintTypeMap.getString(type), d[0], d[1], d[2]);
}

vector3<> SpeciesInfo::Constraint::operator()(const vector3<>& grad)
{	vector3<> scaledGrad = moveScale * grad;
	switch(type)
	{	case Linear: return dot(scaledGrad, d)*d/ d.length_squared();
		case Planar: return  scaledGrad - dot(scaledGrad, d)*d/d.length_squared();	
		default: return scaledGrad;
	}
}


SpeciesInfo::SpeciesInfo()
{
	Z = 0.0;
	atomicNumber = 0;
	Z_chargeball = 0.0; width_chargeball = 0.0;
	tauCore_rCut = 0.; tauCorePlot = false;
	dE_dnG = 0.0;
	mass = 0.0;
	coreRadius = 0.;
	initialOxidationState = 0.;
	
	pulayfilename ="none";
	OpsiRadial = &psiRadial;

	nAug = 0;
	E_nAug = 0;
	nagIndex = 0;
	nagIndexPtr = 0;
}

SpeciesInfo::~SpeciesInfo()
{	if(atpos.size())
	{
		#ifdef GPU_ENABLED
		cudaFree(atposGpu);
		#endif
		VlocRadial.free();
		nCoreRadial.free();
		tauCoreRadial.free();
		for(auto& Vnl_l: VnlRadial) for(auto& Vnl_lp : Vnl_l) Vnl_lp.free();
		for(auto& Qijl: Qradial) Qijl.second.free();
		for(auto& psi_l: psiRadial) for(auto& psi_lp: psi_l) psi_lp.free();
		if(OpsiRadial != &psiRadial)
		{	for(auto& Opsi_l: *OpsiRadial) for(auto& Opsi_lp: Opsi_l) Opsi_lp.free();
			delete OpsiRadial;
		}
	}
}


void SpeciesInfo::setup(const Everything &everything)
{	e = &everything;
	if(!atpos.size()) return; //unused species
	
	//Read pseudopotential
	ifstream ifs(potfilename.c_str());
	if(!ifs.is_open()) die("Can't open pseudopotential file '%s' for reading.\n", potfilename.c_str());
	logPrintf("\nReading pseudopotential file '%s':\n",potfilename.c_str());
	switch(pspFormat)
	{	case Fhi: readFhi(ifs); break;
		case Uspp: readUspp(ifs); break;
		case UPF: readUPF(ifs); break;
	}
	estimateAtomEigs();
	if(coreRadius) logPrintf("  Core radius for overlap checks: %.2lf bohrs.\n", coreRadius);
	else if(!VnlRadial.size()) logPrintf("  Disabling overlap check for local pseudopotential.\n");
	else logPrintf("  Warning: could not determine core radius; disabling overlap check for this species.\n");
	setupPulay(); //Pulay info

	//Check for augmentation:
	if(Qint.size())
	{	bool needKE = e->exCorr.needsKEdensity();
		bool needEXX = (e->exCorr.exxFactor()!=0.);
		for(auto exCorr: e->exCorrDiff)
		{	needKE |= e->exCorr.needsKEdensity();
			needEXX |= (e->exCorr.exxFactor()!=0.);
		}
		if(needKE || needEXX)
			die("\nUltrasoft pseudopotentials do not currently support meta-GGA or hybrid functionals.\n");
	}
	
	//Generate atomic number from symbol, if not stored in pseudopotential:
	if(!atomicNumber)
	{	AtomicSymbol atSym = AtomicSymbol::H;
		if(!atomicSymbolMap.getEnum(name.c_str(), atSym))
			die("\nCould not determine atomic number for species '%s'.\n"
				"Either use a pseudopotential which contains this information,\n"
				"or set the species name to be the chemical symbol for that atom type.\n", name.c_str());
		atomicNumber = int(atSym);
	}
	
	//Get the atomic mass if not set:
	if(!mass) mass = atomicMass(AtomicSymbol(atomicNumber));
	
	//Initialize the MnlAll matrix, and if necessary, QintAll:
	{	//Count projectors:
		int nSpinors = e->eInfo.spinorLength();
		int nProj = 0;
		for(unsigned l=0; l<VnlRadial.size(); l++)
			nProj += (2*l+1) * nSpinors * VnlRadial[l].size();
		if(nProj)
		{	MnlAll = zeroes(nProj,nProj);
			if(Qint.size())
				QintAll = zeroes(nProj,nProj);
			//Set submatrices:
			int iProj = 0;
			for(unsigned l=0; l<VnlRadial.size(); l++)
				if(VnlRadial[l].size())
				{	unsigned nMS = (2*l+1) * nSpinors; //number of m and spins at each l
					int iStop = iProj + nMS*VnlRadial[l].size();
					for(unsigned iMS=0; iMS<nMS; iMS++) //repeat nMS times
					{	MnlAll.set(iProj+iMS,nMS,iStop, iProj+iMS,nMS,iStop, Mnl[l]);
						if(Qint.size() && Qint[l])
							QintAll.set(iProj+iMS,nMS,iStop, iProj+iMS,nMS,iStop, Qint[l]);
					}
					iProj = iStop;
				}
		}
	}
	
	#ifdef GPU_ENABLED
	//Alloc and init GPU atomic positions:
	cudaMalloc(&atposGpu, sizeof(vector3<>)*atpos.size());
	atposPref = atposGpu;
	#else
	atposPref = &atpos[0];
	#endif
	sync_atpos();
	
	Rprev = e->gInfo.R; //remember initial lattice vectors, so that updateLatticeDependent can check if an update is necessary
}


void SpeciesInfo::print(FILE* fp) const
{	if(!atpos.size()) return; //unused species
	for(unsigned at=0; at<atpos.size(); at++)
	{	vector3<> pos = atpos[at]; //always in gInfo coordinates
		if(e->iInfo.coordsType == CoordsCartesian)
			pos = e->gInfo.R * pos; //convert to Cartesian coordinates
		fprintf(fp, "ion %s %19.15lf %19.15lf %19.15lf %lg",
			name.c_str(), pos[0], pos[1], pos[2], constraints[at].moveScale);
		if(constraints[at].type != Constraint::None)
			constraints[at].print(fp, *e);
		fprintf(fp, "\n");
	}
	
	//Output magnetic moments for spin-polarized calculations:
	//--- Only supported for pseudopotentials with atomic orbitals
	if(e->eInfo.spinType == SpinZ && OpsiRadial->size())
	{	diagMatrix M(atpos.size(), 0.); //magnetic moments
		for(int q=e->eInfo.qStart; q<e->eInfo.qStop; q++) //states
		{	const ColumnBundle& Cq = e->eVars.C[q];
			const diagMatrix& Fq = e->eVars.F[q];
			ColumnBundle Opsi = Cq.similar(atpos.size()); //space for atomic orbitals
			const QuantumNumber& qnum = *(Cq.qnum);
			const Basis& basis = *(Cq.basis);
			for(int l=0; l<int(OpsiRadial->size()); l++) //angular momenta
				for(const RadialFunctionG& curOpsiRadial: OpsiRadial->at(l)) //principal quantum number (shells of pseudo-atom)
					for(int m=-l; m<=+l; m++) //angular momentum directions
					{	//Compute the atomic orbitals:
						callPref(Vnl)(basis.nbasis, basis.nbasis, atpos.size(), l, m, qnum.k, basis.iGarrPref, e->gInfo.G, atposPref, curOpsiRadial, Opsi.dataPref());
						//Accumulate electron counts:
						matrix CdagOpsi = Cq ^ Opsi;
						M += (qnum.spin * qnum.weight) * diag(dagger(CdagOpsi) * Fq * CdagOpsi);
					}
		}
		mpiUtil->allReduce(M.data(), M.size(), MPIUtil::ReduceSum);
		fprintf(fp, "# magnetic-moments %s", name.c_str());
		for(double m: M) fprintf(fp, " %+lg", m);
		fprintf(fp, "\n");
	}
}

void SpeciesInfo::updateLatticeDependent()
{	const GridInfo& gInfo = e->gInfo;
	bool Rchanged = (Rprev != gInfo.R);
	Rprev = gInfo.R;

	//Change radial function extents if R has changed:
	if(Rchanged)
	{	int nGridLoc = int(ceil(gInfo.GmaxGrid/gInfo.dGradial))+5;
		VlocRadial.updateGmax(0, nGridLoc);
		nCoreRadial.updateGmax(0, nGridLoc);
		tauCoreRadial.updateGmax(0, nGridLoc);
		for(auto& Qijl: Qradial) Qijl.second.updateGmax(Qijl.first.l, nGridLoc);
		cachedV.clear(); //clear any cached projectors
	}
	
	//Update Qradial indices, matrix and nagIndex if not previously init'd, or if R has changed:
	if(Qint.size() && (Rchanged || !QradialMat))
	{	int nCoeffHlf = (Qradial.cbegin()->second.nCoeff+1)/2; //pack real radial functions into complex numbers
		int nCoeff = 2*nCoeffHlf;
		//Qradial:
		QradialMat = zeroes(nCoeffHlf, Qradial.size());
		double* QradialMatData = (double*)QradialMat.dataPref();
		int index=0;
		for(auto& Qijl: Qradial)
		{	((QijIndex&)Qijl.first).index = index;
			callPref(eblas_copy)(QradialMatData+index*nCoeff, Qijl.second.coeffPref(), Qijl.second.nCoeff);
			index++;
		}
		//nagIndex:
		#ifdef GPU_ENABLED
		if(nagIndex) cudaFree(nagIndex); nagIndex=0;
		if(nagIndexPtr) cudaFree(nagIndexPtr); nagIndexPtr=0;
		#else
		if(nagIndex) delete[] nagIndex; nagIndex=0;
		if(nagIndexPtr) delete[] nagIndexPtr; nagIndexPtr=0;
		#endif
		callPref(setNagIndex)(gInfo.S, gInfo.G, gInfo.iGstart, gInfo.iGstop, nCoeff, 1./gInfo.dGradial, nagIndex, nagIndexPtr);
	}
}

//! Return the first non-blank, non-comment line:
string getLineIgnoringComments(istream& in)
{	string line;
	while(line.find_first_not_of(" \t\n\r")==string::npos || line.at(0)=='#')
		getline(in, line);
	return line;
}

// Read pulay stuff from file which has a line with number of Ecuts
// followed by arbitrary number of Ecut dE_dnG pairs
void SpeciesInfo::setupPulay()
{	
	if(pulayfilename == "none") return;
	
	ifstream ifs(pulayfilename.c_str());
	if(!ifs.is_open()) die("  Can't open pulay file %s for reading.\n", pulayfilename.c_str());
	logPrintf("  Reading pulay file %s ... ", pulayfilename.c_str());
	istringstream iss;
	int nEcuts; istringstream(getLineIgnoringComments(ifs)) >> nEcuts;
	std::map<double,double> pulayMap;
	for(int i=0; i<nEcuts; i++)
	{	double Ecut, dE_dnG;
		istringstream(getLineIgnoringComments(ifs)) >> Ecut >> dE_dnG;
		pulayMap[Ecut] = dE_dnG;
	}
	
	double minEcut = pulayMap.begin()->first;
	double maxEcut = pulayMap.rbegin()->first;
	double Ecut = e->cntrl.Ecut;
	if(pulayMap.find(Ecut) != pulayMap.end())
	{	dE_dnG = pulayMap[Ecut];
		logPrintf("using dE_dnG = %le computed for Ecut = %lg.\n", dE_dnG, Ecut);
	}
	else if(Ecut < minEcut)
	{	die("\n  Ecut=%lg < smallest Ecut=%lg in pulay file %s.\n",
			Ecut, minEcut, pulayfilename.c_str());
	}
	else if(Ecut > maxEcut)
	{	dE_dnG = 0;
		logPrintf("using dE_dnG = 0 for Ecut=%lg > largest Ecut=%lg in file.\n",
			Ecut, maxEcut);
	}
	else
	{	auto iRight = pulayMap.upper_bound(Ecut); //iterator just > Ecut
		auto iLeft = iRight; iLeft--; //iterator just < Ecut
		double t = (Ecut - iLeft->first) / (iRight->first - iLeft->first);
		dE_dnG = (1.-t) * iLeft->second + t * iRight->second;
		logPrintf("using dE_dnG = %le interpolated from Ecut = %lg and %lg.\n",
			dE_dnG, iLeft->first, iRight->first);
	}
}
