/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Kendra Letchworth Weaver

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

#include <electronic/Everything.h>
#include <electronic/LinearPCM.h>
#include <electronic/PCM_internal.h>
#include <core/DataIO.h>
#include <core/DataMultiplet.h>
#include <core/Thread.h>


LinearPCMparams::LinearPCMparams(const FluidSolverParams& p) : FluidSolverParams(p)
{	//Initialize extra parameters:
	k2factor = (8*M_PI/T) * ionicConcentration * pow(ionicZelectrolyte,2);
}

LinearPCM::LinearPCM(const Everything& e, const FluidSolverParams& fsp)
: FluidSolver(e), params(fsp), Kkernel(e.gInfo)
{
	citePCM(fsp);
	logPrintf("   Cavity determined by nc: %lg and sigma: %lg\n", fsp.nc, fsp.sigma);
	Cavitation::print(fsp);
}

DataGptr LinearPCM::hessian(const DataGptr& phiTilde)
{	DataRptr epsilon = 1. + (params.epsBulk-1.) * shape;
	DataGptr rhoTilde = divergence(J(epsilon * I(gradient(phiTilde))));  //dielectric term
	if(params.k2factor) rhoTilde -= params.k2factor * J(shape*I(phiTilde)); // screening term
	return rhoTilde;
}

DataGptr LinearPCM::precondition(const DataGptr& rTilde)
{	return Kkernel*(J(epsInv*I(Kkernel*rTilde)));
}

//Initialize Kkernel to square-root of the inverse kinetic operator
inline void setPreconditionerKernel(int i, double G2, double* Kkernel, double kRMS)
{	if(i==0) Kkernel[i] = kRMS ? 1.0/kRMS : 0.0;
	else Kkernel[i] = 1.0/sqrt(G2 + pow(kRMS,2));
}

void LinearPCM::set(const DataGptr& rhoExplicitTilde, const DataGptr& nCavityTilde)
{
	this->rhoExplicitTilde = clone(rhoExplicitTilde); zeroNyquist(this->rhoExplicitTilde);
	this->nCavity = I(nCavityTilde);

	//Compute cavity shape function (0 to 1)
	shape = DataRptr(DataR::alloc(e.gInfo,isGpuEnabled()));
	pcmShapeFunc(nCavity, shape, params.nc, params.sigma);
	
	//Info:
	logPrintf("\tLinear fluid (dielectric constant: %g", params.epsBulk);
	if(params.ionicConcentration) logPrintf(", screening length: %g Bohr", sqrt(params.epsBulk/params.k2factor));
	logPrintf(") occupying %lf of unit cell:", integral(shape)/e.gInfo.detR); logFlush();

	//Update the preconditioner
	DataRptr epsilon = 1 + (params.epsBulk-1)*shape;
	DataRptr kappaSq = params.ionicConcentration ? params.k2factor*shape : 0; //set kappaSq to null pointer if no screening
	epsInv = inv(epsilon);
	double kRMS = (kappaSq ? sqrt(sum(kappaSq)/sum(epsilon)) : 0.0);
	applyFuncGsq(Kkernel.gInfo, setPreconditionerKernel, Kkernel.data, kRMS);
	Kkernel.set();
	
	//Initialize the state if it hasn't been loaded:
	if(!state) nullToZero(state, e.gInfo);
}


void LinearPCM::minimizeFluid()
{
	fprintf(e.fluidMinParams.fpLog, "\n\tWill stop at %d iterations, or sqrt(|r.z|)<%le\n", e.fluidMinParams.nIterations, e.fluidMinParams.knormThreshold);
	int nIter = solve((-4*M_PI)*rhoExplicitTilde, e.fluidMinParams);
	logPrintf("\tCompleted after %d iterations.\n", nIter);
}

double LinearPCM::get_Adiel_and_grad(DataGptr& grad_rhoExplicitTilde, DataGptr& grad_nCavityTilde, IonicGradient& extraForces)
{
	DataGptr& phi = state; // that's what we solved for in minimize

	//The "electrostatic" gradient is the potential due to the bound charge alone:
	grad_rhoExplicitTilde = phi - (-4*M_PI)*Linv(O(rhoExplicitTilde));
	Adiel["Electrostatic"] = 0.5*dot(grad_rhoExplicitTilde, O(rhoExplicitTilde));
	
	//Compute gradient w.r.t shape function:
	//--- Dielectric contributions:
	DataRptrVec gradPhi = I(gradient(phi));
	DataRptr gradPhiSq = gradPhi[0]*gradPhi[0] + gradPhi[1]*gradPhi[1] + gradPhi[2]*gradPhi[2];
	DataRptr Adiel_shape = (-(params.epsBulk-1)/(8*M_PI)) * gradPhiSq; //dielectric part
	//--- Screening contributions:
	if(params.ionicConcentration)
	{	DataRptr Iphi = I(phi); //potential in real space
		Adiel_shape += (params.k2factor/(8*M_PI)) * (Iphi*Iphi);
	}
	//--- Cavitation contributions:
	Cavitation::energyAndGrad(Adiel, shape, Adiel_shape, params);
	
	//The "cavity" gradient is computed by chain rule via the gradient w.r.t to the shape function:
	DataRptr grad_nCavity(DataR::alloc(e.gInfo));
	pcmShapeFunc_grad(nCavity, Adiel_shape, grad_nCavity, params.nc, params.sigma);
	grad_nCavityTilde = J(grad_nCavity);
	
	return Adiel;
}

void LinearPCM::loadState(const char* filename)
{	DataRptr Istate(DataR::alloc(e.gInfo));
	loadRawBinary(Istate, filename); //saved data is in real space
	state = J(Istate);
}

void LinearPCM::saveState(const char* filename) const
{	saveRawBinary(I(state), filename); //saved data is in real space
}

void LinearPCM::dumpDensities(const char* filenamePattern) const
{	string filename(filenamePattern);
	filename.replace(filename.find("%s"), 2, "Shape");
	logPrintf("Dumping '%s'... ", filename.c_str());  logFlush();
	saveRawBinary(shape, filename.c_str());
	logPrintf("done.\n"); logFlush();
}

void LinearPCM::dumpDebug(const char* filenamePattern) const
{
	string filename(filenamePattern);
	filename.replace(filename.find("%s"), 2, "Debug");
	logPrintf("Dumping '%s'... \t", filename.c_str());  logFlush();

	FILE* fp = fopen(filename.c_str(), "w");
	if(!fp) die("Error opening %s for writing.\n", filename.c_str());	
	
	// Dumps the polarization fraction
	DataRptrVec shape_x = gradient(shape);
	DataRptr surfaceDensity = sqrt(shape_x[0]*shape_x[0] + shape_x[1]*shape_x[1] + shape_x[2]*shape_x[2]);
	fprintf(fp, "\nCavity Information:\n");
	fprintf(fp, "Volume = %f\n", integral(1.-shape));
	fprintf(fp, "Surface Area = %f\n", integral(surfaceDensity));
	
	fprintf(fp, "\nComponents of Adiel:\n");
	Adiel.print(fp, true, "   %13s = %25.16lf\n");
	
	fclose(fp);
	logPrintf("done\n"); logFlush();
}
