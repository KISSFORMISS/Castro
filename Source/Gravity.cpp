#include <cmath>
#include <limits>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <ParmParse.H>
#include "Gravity.H"
#include "Castro.H"
#include <Gravity_F.H>
#include <Castro_F.H>

#include <FMultiGrid.H>

#define MAX_LEV 15

// Give this a bogus default value to force user to define in inputs file
std::string Gravity::gravity_type = "fillme";
#ifndef NDEBUG
#ifndef PARTICLES
int Gravity::test_solves  = 1;
#else
int Gravity::test_solves  = 0;
#endif
#else
int Gravity::test_solves  = 0;
#endif
int  Gravity::verbose        = 0;
int  Gravity::no_sync        = 0;
int  Gravity::no_composite   = 0;
int  Gravity::drdxfac        = 1;
int  Gravity::lnum           = 0;
int  Gravity::direct_sum_bcs = 0;
int  Gravity::get_g_from_phi = 0;
Real Gravity::sl_tol         = 1.e100;
Real Gravity::ml_tol         = 1.e100;
Real Gravity::delta_tol      = 1.e100;
Real Gravity::const_grav     =  0.0;
Real Gravity::max_radius_all_in_domain =  0.0;
Real Gravity::mass_offset    =  0.0;
int  Gravity::stencil_type   = CC_CROSS_STENCIL;

// ************************************************************************************** //

// Ggravity is defined as -4 * pi * G, where G is the gravitational constant.

// In CGS, this constant is currently 
//      Gconst   =  6.67428e-8           cm^3/g/s^2 , which results in 
//      Ggravity = -83.8503442814844e-8  cm^3/g/s^2

// ************************************************************************************** //

static Real Ggravity = 0.;

Array< Array<Real> > Gravity::radial_grav_old(MAX_LEV);
Array< Array<Real> > Gravity::radial_grav_new(MAX_LEV);
Array< Array<Real> > Gravity::radial_mass(MAX_LEV);
Array< Array<Real> > Gravity::radial_vol(MAX_LEV);
#ifdef GR_GRAV
Array< Array<Real> > Gravity::radial_pres(MAX_LEV);
#endif
 
Gravity::Gravity(Amr* Parent, int _finest_level, BCRec* _phys_bc, int _Density)
  : 
    parent(Parent),
    LevelData(MAX_LEV),
    grad_phi_curr(MAX_LEV),
    grad_phi_prev(MAX_LEV),
    phi_flux_reg(MAX_LEV,PArrayManage),
    grids(MAX_LEV),
    level_solver_resnorm(MAX_LEV),
    volume(MAX_LEV),
    area(MAX_LEV),
    phys_bc(_phys_bc)
{
     Density = _Density;
     read_params();
     finest_level_allocated = -1;
     if (gravity_type == "PoissonGrav") make_mg_bc();
}

Gravity::~Gravity() {}

void
Gravity::read_params ()
{
    static bool done = false;

    if (!done)
    {
        ParmParse pp("gravity");

        pp.get("gravity_type", gravity_type);

        if ( (gravity_type != "ConstantGrav") && 
	     (gravity_type != "PoissonGrav") && 
	     (gravity_type != "MonopoleGrav") &&
             (gravity_type != "PrescribedGrav") )
             {
                std::cout << "Sorry -- dont know this gravity type"  << std::endl;
        	BoxLib::Abort("Options are ConstantGrav, PoissonGrav, MonopoleGrav, or PrescribedGrav");
             }
	     
        if (  gravity_type == "ConstantGrav") 
        {
	  if ( Geometry::IsSPHERICAL() )
	      BoxLib::Abort("Cant use constant direction gravity with non-Cartesian coordinates");
           pp.get("const_grav", const_grav);
        }

#if (BL_SPACEDIM == 1)
        if (gravity_type == "PoissonGrav")
        {
	  BoxLib::Abort(" gravity_type = PoissonGrav doesn't work well in 1-d -- please set gravity_type = MonopoleGrav");
        } 
        else if (gravity_type == "MonopoleGrav" && !(Geometry::IsSPHERICAL()))
        {
	  BoxLib::Abort("Only use MonopoleGrav in 1D spherical coordinates");
        }
        else if (gravity_type == "ConstantGrav" && Geometry::IsSPHERICAL())
        {
	  BoxLib::Abort("Can't use constant gravity in 1D spherical coordinates");
        }
	  
#elif (BL_SPACEDIM == 2)
        if (gravity_type == "MonopoleGrav" && Geometry::IsCartesian() )
        {
	  BoxLib::Abort(" gravity_type = MonopoleGrav doesn't make sense in 2D Cartesian coordinates");
        } 
#endif

        pp.query("drdxfac", drdxfac);

        pp.query("v", verbose);
        pp.query("no_sync", no_sync);
        pp.query("no_composite", no_composite);
 
        pp.query("max_multipole_order", lnum);
    
        // Check if the user wants to compute the boundary conditions using the brute force method.
        // Default is false, since this method is slow.

        pp.query("direct_sum_bcs", direct_sum_bcs);

	// For non-Poisson gravity, do we want to construct the gravitational acceleration by taking
	// the gradient of the potential, rather than constructing it directly?

	int got_get_g_from_phi = pp.query("get_g_from_phi", get_g_from_phi);

	if (got_get_g_from_phi && !get_g_from_phi && gravity_type == "PoissonGrav")
	  if (ParallelDescriptor::IOProcessor())
	    std::cout << "Warning: gravity_type = PoissonGrav assumes get_g_from_phi is true" << std::endl;
	
        // Allow run-time input of solver tolerances
	if (Geometry::IsCartesian()) {
	  ml_tol = 1.e-11;
	  sl_tol = 1.e-11;
	  delta_tol = 1.e-11;
	}
	else {
	  ml_tol = 1.e-10;
	  sl_tol = 1.e-10;
	  delta_tol = 1.e-10;
	}
        pp.query("ml_tol",ml_tol);
        pp.query("sl_tol",sl_tol);
        pp.query("delta_tol",delta_tol);

        Real Gconst;
        BL_FORT_PROC_CALL(GET_GRAV_CONST, get_grav_const)(&Gconst);
        Ggravity = -4.0 * M_PI * Gconst;
        if (verbose > 0 && ParallelDescriptor::IOProcessor())
        {
           std::cout << "Getting Gconst from constants: " << Gconst << std::endl;
           std::cout << "Using " << Ggravity << " for 4 pi G in Gravity.cpp " << std::endl;
        }

        done = true;
    }
}

void
Gravity::set_numpts_in_gravity (int numpts)
{
  numpts_at_level = numpts;
}

void
Gravity::install_level (int                   level,
                        AmrLevel*             level_data,
                        MultiFab&             _volume,
                        MultiFab*             _area)
{
    if (verbose > 1 && ParallelDescriptor::IOProcessor())
        std::cout << "Installing Gravity level " << level << '\n';

    LevelData.clear(level);
    LevelData.set(level, level_data);

    volume.clear(level);
    volume.set(level, &_volume);

    area.set(level, _area);

    BoxArray ba(LevelData[level].boxArray());
    grids[level] = ba;

    level_solver_resnorm[level] = 0.0;

    if (gravity_type == "PoissonGrav") {

       grad_phi_prev[level].clear();
       grad_phi_prev[level].resize(BL_SPACEDIM,PArrayManage);
       for (int n=0; n<BL_SPACEDIM; ++n)
           grad_phi_prev[level].set(n,new MultiFab(getEdgeBoxArray(level,n),1,1));

       grad_phi_curr[level].clear();
       grad_phi_curr[level].resize(BL_SPACEDIM,PArrayManage);
       for (int n=0; n<BL_SPACEDIM; ++n)
           grad_phi_curr[level].set(n,new MultiFab(getEdgeBoxArray(level,n),1,1));

       if (level > 0) {
          phi_flux_reg.clear(level);
          IntVect crse_ratio = parent->refRatio(level-1);
          phi_flux_reg.set(level,new FluxRegister(grids[level],crse_ratio,level,1));
       }

#if (BL_SPACEDIM > 1)
    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {

        if (!Geometry::isAllPeriodic())
        {
           int n1d = drdxfac*numpts_at_level;

           radial_grav_old[level].resize(n1d);
           radial_grav_new[level].resize(n1d);
           radial_mass[level].resize(n1d);
           radial_vol[level].resize(n1d);
#ifdef GR_GRAV
           radial_pres[level].resize(n1d);
#endif
        }

#endif
    }

    // Compute the maximum radius at which all the mass at that radius is in the domain,
    //   assuming that the "hi" side of the domain is away from the center.
#if (BL_SPACEDIM > 1)
    if (level == 0)
    {
        Real center[BL_SPACEDIM];
        BL_FORT_PROC_CALL(GET_CENTER,get_center)(center);
        Real x = Geometry::ProbHi(0) - center[0];
        Real y = Geometry::ProbHi(1) - center[1];
        max_radius_all_in_domain = std::min(x,y);
#if (BL_SPACEDIM == 3)
        Real z = Geometry::ProbHi(2) - center[2];
        max_radius_all_in_domain = std::min(max_radius_all_in_domain,z);
#endif
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "Maximum radius for which the mass is contained in the domain: " 
                      << max_radius_all_in_domain << std::endl;
    }
#endif

    finest_level_allocated = level;
}

std::string Gravity::get_gravity_type()
{
  return gravity_type;
}

Real Gravity::get_const_grav()
{
  return const_grav;
}

int Gravity::NoSync()
{
  return no_sync;
}

int Gravity::NoComposite()
{
  return no_composite;
}

int Gravity::test_results_of_solves()
{
  return test_solves;
}

PArray<MultiFab>& 
Gravity::get_grad_phi_prev(int level)
{
  return grad_phi_prev[level];
}

MultiFab*
Gravity::get_grad_phi_prev_comp(int level, int comp)
{
  return &grad_phi_prev[level][comp];
}

PArray<MultiFab>& 
Gravity::get_grad_phi_curr(int level)
{
  return grad_phi_curr[level];
}

void 
Gravity::plus_grad_phi_curr(int level, PArray<MultiFab>& addend)
{
  for (int n = 0; n < BL_SPACEDIM; n++)
    grad_phi_curr[level][n].plus(addend[n],0,1,0);
}

void
Gravity::swapTimeLevels (int level)
{
    if (gravity_type == "PoissonGrav") {
	for (int n=0; n < BL_SPACEDIM; n++) {
	    MultiFab* dummy = grad_phi_curr[level].remove(n);
	    grad_phi_prev[level].clear(n);
	    grad_phi_prev[level].set(n,dummy);
	    
	    grad_phi_curr[level].set(n, new MultiFab(getEdgeBoxArray(level,n),1,1));
	    grad_phi_curr[level][n].setVal(1.e50);
	} 
    }
}

void
Gravity::zeroPhiFluxReg (int level)
{
  phi_flux_reg[level].setVal(0.);
}

void
Gravity::solve_for_phi (int               level,
			MultiFab&         phi,
                        PArray<MultiFab>& grad_phi,
			int               is_new)
{
    BL_PROFILE("Gravity::solve_for_phi()");

    if (verbose && ParallelDescriptor::IOProcessor())
	std::cout << " ... solve for phi at level " << level << std::endl;

    const Real strt = ParallelDescriptor::second();

    if (is_new == 0) sanity_check(level);

    PArray<MultiFab> phi_p(1);
    phi_p.set(0, &phi);

    PArray<MultiFab> rhs_p;
    get_rhs(level, 1, rhs_p, is_new);

    Array< PArray<MultiFab> > grad_phi_p(1);
    grad_phi_p[0].resize(BL_SPACEDIM);
    for (int i = 0; i < BL_SPACEDIM ; i++) {
	grad_phi_p[0].set(i, &grad_phi[i]);
    }

    PArray<MultiFab> res_null;

    Real time;
    if (is_new == 1) {
	time = LevelData[level].get_state_data(PhiGrav_Type).curTime();
    } else {
	time = LevelData[level].get_state_data(PhiGrav_Type).prevTime();
    }
    
    level_solver_resnorm[level] = solve_phi_with_fmg(level, level,
						     phi_p,
						     rhs_p,
						     grad_phi_p, 
						     res_null,
						     time);

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::solve_for_phi() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }
}

void
Gravity::solve_for_delta_phi (int                        crse_level,
                              int                        fine_level,
                              MultiFab&                  CrseRhs,
                              PArray<MultiFab>&          delta_phi,
                              PArray<PArray<MultiFab> >& grad_delta_phi)
{
    BL_PROFILE("Gravity::solve_delta_phi()");

    int nlevs = fine_level - crse_level + 1;
    BL_ASSERT(grad_delta_phi.size() == nlevs);
    BL_ASSERT(delta_phi.size() == nlevs);

    if (verbose && ParallelDescriptor::IOProcessor()) {
      std::cout << "... solving for delta_phi at crse_level = " << crse_level << std::endl;
      std::cout << "...                    up to fine_level = " << fine_level << std::endl;
    }

    PArray<MultiFab> rhs(nlevs);
    PArray<MultiFab> RAII(nlevs, PArrayManage);

    for (int ilev = 0; ilev < nlevs; ++ilev) {
	int amr_lev = ilev + crse_level;
	if (ilev == 0) {
	    rhs.set(ilev, &CrseRhs);
	} else {
	    RAII.set(ilev, new MultiFab(grids[amr_lev], 1, 0));
	    rhs.set(ilev, &RAII[ilev]);
	    rhs[ilev].setVal(0.0);
	}
    }

    PArray<Geometry> geom(nlevs);
    for (int ilev = 0; ilev < nlevs; ++ilev) {
	int amr_lev = ilev + crse_level;
	geom.set(ilev, &(parent->Geom(amr_lev)));
    }

    IntVect crse_ratio = crse_level > 0 ? parent->refRatio(crse_level-1)
                                        : IntVect::TheZeroVector();

    FMultiGrid fmg(geom, crse_level, crse_ratio);

    if (crse_level == 0 && !Geometry::isAllPeriodic()) {
	fmg.set_bc(mg_bc, delta_phi[0]);
    } else {
	fmg.set_bc(mg_bc);
    }

    Array<PArray<MultiFab> > coeffs(nlevs);
#if (BL_SPACEDIM < 3)
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
	for (int ilev = 0; ilev < nlevs; ++ilev) {
	    int amr_lev = ilev + crse_level;	    
	    coeffs[ilev].resize(BL_SPACEDIM, PArrayManage);
	    for (int i = 0; i < BL_SPACEDIM ; i++) {
		coeffs[ilev].set(i, new MultiFab(grids[amr_lev], 1, 0, Fab_allocate, 
						 IntVect::TheDimensionVector(i)));
		coeffs[ilev][i].setVal(1.0);
	    }

	    applyMetricTerms(amr_lev, rhs[ilev], coeffs[ilev]);
	}

	fmg.set_gravity_coeffs(coeffs);
    } 
    else 
#endif
    {
	fmg.set_const_gravity_coeffs();
    }
    
    // Subtract off RHS average (on coarse level) from all levels to ensure solvability

    const Box& crse_box = parent->Geom(crse_level).Domain();
    if (Geometry::isAllPeriodic() && (grids[crse_level].numPts() == crse_box.numPts())) {
       Real local_correction = 0.0;
#ifdef _OPENMP
#pragma omp parallel reduction(+:local_correction)
#endif
       for (MFIter mfi(CrseRhs,true); mfi.isValid(); ++mfi) {
           local_correction += CrseRhs[mfi].sum(mfi.tilebox(),0,1);
       }
       ParallelDescriptor::ReduceRealSum(local_correction);
       
       local_correction = local_correction / grids[crse_level].numPts();

       if (verbose && ParallelDescriptor::IOProcessor())
          std::cout << "WARNING: Adjusting RHS in solve_for_delta_phi by " << local_correction << std::endl;

       rhs[0].plus(-local_correction,0,1,0);
    }

    Real     tol = delta_tol;
    Real abs_tol = level_solver_resnorm[crse_level];
    for (int lev = crse_level+1; lev < fine_level; lev++)
        abs_tol = std::max(abs_tol,level_solver_resnorm[lev]);
      
    int need_grad_phi = 1;
    int always_use_bnorm = (Geometry::isAllPeriodic()) ? 0 : 1;
    fmg.solve(delta_phi, rhs, tol, abs_tol, always_use_bnorm, need_grad_phi);

    fmg.get_fluxes(grad_delta_phi);

#if (BL_SPACEDIM < 3)
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() ) {
	for (int ilev = 0; ilev < nlevs; ++ilev)
	{
	    int amr_lev = ilev + crse_level;
	    unweight_edges(amr_lev, grad_delta_phi[ilev]);
	}
    }
#endif
}

void
Gravity::gravity_sync (int crse_level, int fine_level, int iteration, int ncycle,
                       const MultiFab& drho_and_drhoU, const MultiFab& dphi,
                       PArray<MultiFab>& grad_delta_phi_cc)
{
    BL_PROFILE("Gravity::gravity_sync()");

    BL_ASSERT(parent->finestLevel()>crse_level);
    if (verbose && ParallelDescriptor::IOProcessor()) {
          std::cout << " ... gravity_sync at crse_level " << crse_level << '\n';
          std::cout << " ...     up to finest_level     " << fine_level << '\n';
    }

    const Geometry& crse_geom = parent->Geom(crse_level);
    const Box&    crse_domain = crse_geom.Domain();

    // This RHS will hold only the contribution from the average down.
    MultiFab CrseRhsAvgDown(grids[crse_level],1,0);

    // This RHS will contain everything but the average down contribution and is the source term
    // for the delta phi solve.
    MultiFab CrseRhsSync(grids[crse_level],1,0);
    MultiFab::Copy(CrseRhsSync,drho_and_drhoU,0,0,1,0);

    if (crse_level == 0 && crse_level < parent->finestLevel() && !Geometry::isAllPeriodic())
    {
        MultiFab::Copy(CrseRhsAvgDown,CrseRhsSync,0,0,1,0);

	Castro* fine_level = dynamic_cast<Castro*>(&(parent->getLevel(crse_level+1)));
	const MultiFab* mask = fine_level->build_fine_mask();
	MultiFab::Multiply(CrseRhsSync, *mask, 0, 0, 1, 0); 
    }

    CrseRhsSync.mult(Ggravity);
    CrseRhsSync.plus(dphi,0,1,0);

    // In the all-periodic case we enforce that CrseRhsSync sums to zero.
    if (crse_geom.isAllPeriodic() && (grids[crse_level].numPts() == crse_domain.numPts()))
    {
        Real local_correction = 0;
#ifdef _OPENMP
#pragma omp parallel reduction(+:local_correction)
#endif
        for (MFIter mfi(CrseRhsSync); mfi.isValid(); ++mfi)
            local_correction += CrseRhsSync[mfi].sum(mfi.validbox(), 0, 1);
        ParallelDescriptor::ReduceRealSum(local_correction);

        local_correction /= grids[crse_level].numPts();

        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "WARNING: Adjusting RHS in gravity_sync solve by " << local_correction << '\n';
        for (MFIter mfi(CrseRhsSync); mfi.isValid(); ++mfi)
            CrseRhsSync.plus(-local_correction,0,1,0);
    }

    // delta_phi needs a ghost cell for the solve
    PArray<MultiFab>  delta_phi(fine_level-crse_level+1, PArrayManage);
    for (int lev = crse_level; lev <= fine_level; lev++) {
       delta_phi.set(lev-crse_level,new MultiFab(grids[lev],1,1));
       delta_phi[lev-crse_level].setVal(0.);
    }

    PArray<PArray<MultiFab> > ec_gdPhi(fine_level-crse_level+1, PArrayManage);
    for (int lev = crse_level; lev <= fine_level; lev++) {
       ec_gdPhi.set(lev-crse_level,new PArray<MultiFab>(BL_SPACEDIM,PArrayManage));
       for (int n=0; n<BL_SPACEDIM; ++n)  
	   ec_gdPhi[lev-crse_level].set(n,new MultiFab(getEdgeBoxArray(lev,n),1,0));
    }

    // Using the average-down contribution, construct the boundary conditions for the Poisson solve.                                                                                                                                        
    if (crse_level == 0 && !Geometry::isAllPeriodic()) {
      if (verbose && ParallelDescriptor::IOProcessor()) 
         std::cout << " ... Making bc's for delta_phi at crse_level 0"  << std::endl;
#if (BL_SPACEDIM == 3)
      if ( direct_sum_bcs )
        fill_direct_sum_BCs(crse_level,CrseRhsAvgDown,delta_phi[crse_level]);
      else
        fill_multipole_BCs(crse_level,CrseRhsAvgDown,delta_phi[crse_level]);
#else
      int fill_interior = 0;
      make_radial_phi(crse_level,CrseRhsAvgDown,delta_phi[crse_level],fill_interior);
#endif
    }

    // Do multi-level solve for delta_phi
    solve_for_delta_phi(crse_level,fine_level,CrseRhsSync,delta_phi,ec_gdPhi);

    // In the all-periodic case we enforce that delta_phi averages to zero.
    if (crse_geom.isAllPeriodic() && (grids[crse_level].numPts() == crse_domain.numPts()) ) {
       Real local_correction = 0.0;
#ifdef _OPENMP
#pragma omp parallel reduction(+:local_correction)
#endif
       for (MFIter mfi(delta_phi[0],true); mfi.isValid(); ++mfi) {
           local_correction += delta_phi[0][mfi].sum(mfi.tilebox(),0,1);
       }
       ParallelDescriptor::ReduceRealSum(local_correction);

       local_correction = local_correction / grids[crse_level].numPts();

       for (int lev = crse_level; lev <= fine_level; lev++) {
	   delta_phi[lev-crse_level].plus(-local_correction,0,1,1);
       }
    }

    // Add delta_phi to phi_new, and grad(delta_phi) to grad(delta_phi_curr) on each level
    for (int lev = crse_level; lev <= fine_level; lev++) {
       LevelData[lev].get_new_data(PhiGrav_Type).plus(delta_phi[lev-crse_level],0,1,0);
       for (int n = 0; n < BL_SPACEDIM; n++) 
          grad_phi_curr[lev][n].plus(ec_gdPhi[lev-crse_level][n],0,1,0);
    }

    int is_new = 1;

    // Average phi_new from fine to coarse level
    for (int lev = fine_level-1; lev >= crse_level; lev--)
    {
       const IntVect& ratio = parent->refRatio(lev);
       BoxLib::average_down(LevelData[lev+1].get_new_data(PhiGrav_Type),
			    LevelData[lev  ].get_new_data(PhiGrav_Type),
			    0, 1, ratio);
    } 

    // Average the edge-based grad_phi from finer to coarser level
    for (int lev = fine_level-1; lev >= crse_level; lev--)
       average_fine_ec_onto_crse_ec(lev,is_new);

    // Add the contribution of grad(delta_phi) to the flux register below if necessary.
    if (crse_level > 0 && iteration == ncycle)
    {
        for (MFIter mfi(delta_phi[0]); mfi.isValid(); ++mfi) {
            for (int n=0; n<BL_SPACEDIM; ++n) {
		phi_flux_reg[crse_level].FineAdd(ec_gdPhi[0][n][mfi],area[crse_level][n][mfi],n,mfi.index(),0,0,1,1.);
	    }
	}
    }

    for (int lev = crse_level; lev <= fine_level; lev++) {
	grad_delta_phi_cc[lev-crse_level].setVal(0.0);
	const Geometry& geom = parent->Geom(lev);
	BoxLib::average_face_to_cellcenter(grad_delta_phi_cc[lev-crse_level],
					   ec_gdPhi[lev-crse_level],
					   geom);
    }
}

void
Gravity::GetCrsePhi(int level,
                    MultiFab& phi_crse,
                    Real      time      )
{
    BL_ASSERT(level!=0);

    const Real t_old = LevelData[level-1].get_state_data(PhiGrav_Type).prevTime();
    const Real t_new = LevelData[level-1].get_state_data(PhiGrav_Type).curTime();
    Real alpha = (time - t_old)/(t_new - t_old);
    Real omalpha = 1.0 - alpha;

    phi_crse.clear();
    phi_crse.define(grids[level-1], 1, 1, Fab_allocate); // BUT NOTE we don't trust phi's ghost cells.
#ifdef _OPENMP
#pragma omp parallel
#endif
    {
	FArrayBox PhiCrseTemp;
	for (MFIter mfi(phi_crse,true); mfi.isValid(); ++mfi)
        {
	    const Box& gtbx = mfi.growntilebox();
	    
	    PhiCrseTemp.resize(gtbx,1);
	    
	    PhiCrseTemp.copy(LevelData[level-1].get_old_data(PhiGrav_Type)[mfi]);
	    PhiCrseTemp.mult(omalpha);
	    
	    phi_crse[mfi].copy(LevelData[level-1].get_new_data(PhiGrav_Type)[mfi], gtbx);
	    phi_crse[mfi].mult(alpha, gtbx);
	    phi_crse[mfi].plus(PhiCrseTemp);
	}
    }

    const Geometry& geom = parent->Geom(level-1);
    BoxLib::fill_boundary(phi_crse, geom);
}

void
Gravity::GetCrseGradPhi(int level,
                        PArray<MultiFab>& grad_phi_crse,
                        Real              time          ) 
{
    BL_ASSERT(level!=0);

    const Real t_old = LevelData[level-1].get_state_data(State_Type).prevTime();
    const Real t_new = LevelData[level-1].get_state_data(State_Type).curTime();
    Real alpha = (time - t_old)/(t_new - t_old);
    Real omalpha = 1.0 - alpha;

    BL_ASSERT(grad_phi_crse.size() == BL_SPACEDIM);
    for (int i=0; i<BL_SPACEDIM; ++i)
    {
        BL_ASSERT(!grad_phi_crse.defined(i));
        grad_phi_crse.set(i,new MultiFab(getEdgeBoxArray(level-1,i), 1, 0));
#ifdef _OPENMP
#pragma omp parallel
#endif
	{
	    FArrayBox GradPhiCrseTemp;
	    for (MFIter mfi(grad_phi_crse[i],true); mfi.isValid(); ++mfi)
            {
		const Box& tbx =mfi.tilebox();
		    
		GradPhiCrseTemp.resize(tbx,1);
            
		GradPhiCrseTemp.copy(grad_phi_prev[level-1][i][mfi]);
		GradPhiCrseTemp.mult(omalpha);
            
		grad_phi_crse[i][mfi].copy(grad_phi_curr[level-1][i][mfi], tbx);
		grad_phi_crse[i][mfi].mult(alpha, tbx);
		grad_phi_crse[i][mfi].plus(GradPhiCrseTemp);
	    }
        }	
    }
}

void
Gravity::multilevel_solve_for_new_phi (int level, int finest_level, int use_previous_phi_as_guess)
{
    BL_PROFILE("Gravity::multilevel_solve_for_new_phi()");

    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << "... multilevel solve for new phi at base level " << level << " to finest level " << finest_level << std::endl;

    for (int lev = level; lev <= finest_level; lev++) {
       BL_ASSERT(grad_phi_curr[lev].size()==BL_SPACEDIM);
       for (int n=0; n<BL_SPACEDIM; ++n)
       {
           grad_phi_curr[lev].clear(n);
           grad_phi_curr[lev].set(n,new MultiFab(getEdgeBoxArray(lev,n),1,1));
       }
    }

    int is_new = 1;
    actual_multilevel_solve(level,finest_level,grad_phi_curr,is_new,use_previous_phi_as_guess);
}

void
Gravity::actual_multilevel_solve (int crse_level, int finest_level, 
                                  Array<PArray<MultiFab> >& grad_phi, 
				  int is_new, 
                                  int use_previous_phi_as_guess)
{
    BL_PROFILE("Gravity::actual_multilevel_solve()");

    int nlevels = finest_level - crse_level + 1;

    PArray<MultiFab> phi_p(nlevels);
    for (int ilev = 0; ilev < nlevels; ilev++)
    {
	int amr_lev = ilev + crse_level;
	if (is_new == 1) {
	    phi_p.set(ilev, &LevelData[amr_lev].get_new_data(PhiGrav_Type));
	} else {
	    phi_p.set(ilev, &LevelData[amr_lev].get_old_data(PhiGrav_Type));
	}

	if (!use_previous_phi_as_guess)
	    phi_p[ilev].setVal(0.);
    }

    PArray<MultiFab> rhs_p;
    get_rhs(crse_level, nlevels, rhs_p, is_new);

    if (!use_previous_phi_as_guess && crse_level == 0 && !Geometry::isAllPeriodic())
    {
	make_radial_phi(0, rhs_p[0], phi_p[0], 1);
    }

    Array<PArray<MultiFab> > grad_phi_p;
    grad_phi_p.resize(nlevels);
    for (int ilev = 0; ilev < nlevels; ilev++)
    {
	int amr_lev = ilev + crse_level;
	grad_phi_p[ilev].resize(BL_SPACEDIM);
	for (int i = 0; i < BL_SPACEDIM ; i++) {
	    grad_phi_p[ilev].set(i, &grad_phi[amr_lev][i]);
	}
    }

    PArray<MultiFab> res_null;

    Real time;
    if (is_new == 1) {
	time = LevelData[crse_level].get_state_data(PhiGrav_Type).curTime();
    } else {
	time = LevelData[crse_level].get_state_data(PhiGrav_Type).prevTime();
    }

    solve_phi_with_fmg(crse_level, finest_level,
		       phi_p, rhs_p, grad_phi_p, res_null,
		       time);

    // Average phi from fine to coarse level
    for (int amr_lev = finest_level; amr_lev > crse_level; amr_lev--)
    {
       const IntVect& ratio = parent->refRatio(amr_lev-1);
       if (is_new == 1)
       {
	   BoxLib::average_down(LevelData[amr_lev  ].get_new_data(PhiGrav_Type),
				LevelData[amr_lev-1].get_new_data(PhiGrav_Type),
				0, 1, ratio);
       }
       else if (is_new == 0)
       {
	   BoxLib::average_down(LevelData[amr_lev  ].get_old_data(PhiGrav_Type),
				LevelData[amr_lev-1].get_old_data(PhiGrav_Type),
				0, 1, ratio);
       }

    }

    // Average grad_phi from fine to coarse level
    for (int amr_lev = finest_level; amr_lev > crse_level; amr_lev--) 
       average_fine_ec_onto_crse_ec(amr_lev-1,is_new);

}

void
Gravity::get_old_grav_vector(int level, MultiFab& grav_vector, Real time)
{
    BL_PROFILE("Gravity::get_old_grav_vector()");

    int ng = grav_vector.nGrow();

    // Note that grav_vector coming into this routine always has three components.
    // So we'll define a temporary MultiFab with BL_SPACEDIM dimensions.
    // Then at the end we'll copy in all BL_SPACEDIM dimensions from this into
    // the outgoing grav_vector, leaving any higher dimensions unchanged.

    MultiFab grav(grids[level], BL_SPACEDIM, ng);
    grav.setVal(0.0,ng);
    
    if (gravity_type == "ConstantGrav") {

       // Set to constant value in the BL_SPACEDIM direction and zero in all others.
      
       grav.setVal(const_grav,BL_SPACEDIM-1,1,ng);

    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {

      // We also want to fill phi for MonopoleGrav, even though we don't return it.
      
      MultiFab& phi = LevelData[level].get_old_data(PhiGrav_Type);
      
#if (BL_SPACEDIM == 1)
       make_one_d_grav(level,time,grav,phi);
#else

       if (gravity_type == "MonopoleGrav") 
       {
          const Real prev_time = LevelData[level].get_state_data(State_Type).prevTime();
          make_radial_gravity(level,prev_time,radial_grav_old[level]);
          interpolate_monopole_grav(level,radial_grav_old[level],grav);

       }
       else if (gravity_type == "PrescribedGrav") 
       {
          make_prescribed_grav(level,time,grav);
       }  

#endif 
    } else if (gravity_type == "PoissonGrav") {

       // Fill grow cells in grad_phi, will need to compute grad_phi_cc in 1 grow cell
       const Geometry& geom = parent->Geom(level);
       if (level==0)
       {
             for (int i = 0; i < BL_SPACEDIM ; i++)
             {
                 grad_phi_prev[level][i].setBndry(0.0);
		 BoxLib::fill_boundary(grad_phi_prev[level][i], geom);
             }
 
       } else {
 
             PArray<MultiFab> crse_grad_phi(BL_SPACEDIM,PArrayManage);
             GetCrseGradPhi(level,crse_grad_phi,time);
             fill_ec_grow(level,grad_phi_prev[level],crse_grad_phi);
       }
 
       BoxLib::average_face_to_cellcenter(grav, grad_phi_prev[level], geom);

    } else {
       BoxLib::Abort("Unknown gravity_type in get_old_grav_vector");
    }

    // Do the copy to the output vector.

    for (int dir = 0; dir < 3; dir++)
      if (dir < BL_SPACEDIM)
	MultiFab::Copy(grav_vector, grav, dir, dir, 1, ng);
      else
	grav_vector.setVal(0.,dir,1,ng);
    
#if (BL_SPACEDIM > 1)
    if (gravity_type != "ConstantGrav") {
 
       // This is a hack-y way to fill the ghost cell values of grav_vector
       //   before returning it
       AmrLevel* amrlev = &parent->getLevel(level) ;

       AmrLevel::FillPatch(*amrlev,grav_vector,ng,time,Gravity_Type,0,3); 
    }
#endif

#ifdef POINTMASS
    Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(level));
    Real point_mass = cs->get_point_mass();
    add_pointmass_to_gravity(level,grav_vector,point_mass);
#endif
}

void
Gravity::get_new_grav_vector(int level, MultiFab& grav_vector, Real time)
{
    BL_PROFILE("Gravity::get_new_grav_vector()");

    int ng = grav_vector.nGrow();

    // Note that grav_vector coming into this routine always has three components.
    // So we'll define a temporary MultiFab with BL_SPACEDIM dimensions.
    // Then at the end we'll copy in all BL_SPACEDIM dimensions from this into
    // the outgoing grav_vector, leaving any higher dimensions unchanged.

    MultiFab grav(grids[level],BL_SPACEDIM,ng);
    grav.setVal(0.0,ng);
    
    if (gravity_type == "ConstantGrav") {

       // Set to constant value in the BL_SPACEDIM direction
       grav.setVal(const_grav,BL_SPACEDIM-1,1,ng);

    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {

      // We may want to fill phi for MonopoleGrav, even though we don't return it.      
      
      MultiFab& phi = LevelData[level].get_new_data(PhiGrav_Type);

#if (BL_SPACEDIM == 1)
	make_one_d_grav(level,time,grav,phi);
#else

	// We always fill radial_grav_new (at every level)
	if (gravity_type == "MonopoleGrav")
	{
          const Real cur_time = LevelData[level].get_state_data(State_Type).curTime();
          make_radial_gravity(level,cur_time,radial_grav_new[level]);
          interpolate_monopole_grav(level,radial_grav_new[level],grav);
	}
	else if (gravity_type == "PrescribedGrav") 
	{
	  make_prescribed_grav(level,time,grav);
	}

#endif
    } else if (gravity_type == "PoissonGrav") {

      // Fill grow cells in grad_phi, will need to compute grad_phi_cc in 1 grow cell
      const Geometry& geom = parent->Geom(level);
      if (level==0)
      {
            for (int i = 0; i < BL_SPACEDIM ; i++)
            {
                grad_phi_curr[level][i].setBndry(0.0);
		BoxLib::fill_boundary(grad_phi_curr[level][i], geom);
            }

      } else {

            PArray<MultiFab> crse_grad_phi(BL_SPACEDIM,PArrayManage);
            GetCrseGradPhi(level,crse_grad_phi,time);
            fill_ec_grow(level,grad_phi_curr[level],crse_grad_phi);
      }

      BoxLib::average_face_to_cellcenter(grav, grad_phi_curr[level], geom);

    } else {
       BoxLib::Abort("Unknown gravity_type in get_new_grav_vector");
    }

    // Do the copy to the output vector.

    for (int dir = 0; dir < 3; dir++)
      if (dir < BL_SPACEDIM)
	MultiFab::Copy(grav_vector, grav, dir, dir, 1, ng);
      else
	grav_vector.setVal(0.,dir,1,ng);

#if (BL_SPACEDIM > 1)
    if (gravity_type != "ConstantGrav" && ng>0) {
 
       // This is a hack-y way to fill the ghost cell values of grav_vector
       //   before returning it
       AmrLevel* amrlev = &parent->getLevel(level) ;

       AmrLevel::FillPatch(*amrlev,grav_vector,ng,time,Gravity_Type,0,3); 
    }
#endif

#ifdef POINTMASS
    Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(level));
    Real point_mass = cs->get_point_mass();
    add_pointmass_to_gravity(level,grav_vector,point_mass);
#endif
}

void
Gravity::test_level_grad_phi_prev(int level)
{
    BL_PROFILE("Gravity::test_level_grad_phi_prev()");

    // Fill the RHS for the solve
    MultiFab& S_old = LevelData[level].get_old_data(State_Type);
    MultiFab Rhs(grids[level],1,0);
    MultiFab::Copy(Rhs,S_old,Density,0,1,0);

    // This is a correction for fully periodic domains only
    if ( Geometry::isAllPeriodic() )
    {
       if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
          std::cout << " ... subtracting average density from RHS at level ... " 
                    << level << " " << mass_offset << std::endl;
       Rhs.plus(-mass_offset,0,1,0);
    }

    Rhs.mult(Ggravity);

    if (verbose) {
       Real rhsnorm = Rhs.norm0();
       if (ParallelDescriptor::IOProcessor()) {
          std::cout << "... test_level_grad_phi_prev at level " << level << std::endl;
          std::cout << "       norm of RHS             " << rhsnorm << std::endl;
       }
    }

    const Real* dx     = parent->Geom(level).CellSize();
    const Real* problo = parent->Geom(level).ProbLo();
    int coord_type     = Geometry::Coord();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        // Test whether using the edge-based gradients
        //   to compute Div(Grad(Phi)) satisfies Lap(phi) = RHS
        // Fill the RHS array with the residual
        BL_FORT_PROC_CALL(CA_TEST_RESIDUAL,ca_test_residual)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(grad_phi_prev[level][0][mfi]),
                    BL_TO_FORTRAN(grad_phi_prev[level][1][mfi]),
                    BL_TO_FORTRAN(grad_phi_prev[level][2][mfi])),
                    dx,problo,&coord_type);
    }
    if (verbose) {
       Real resnorm = Rhs.norm0();
//     Real gppxnorm = grad_phi_prev[level][0].norm0();
#if (BL_SPACEDIM > 1)
//     Real gppynorm = grad_phi_prev[level][1].norm0();
#endif
#if (BL_SPACEDIM > 2)
//     Real gppznorm = grad_phi_prev[level][2].norm0();
#endif
      if (ParallelDescriptor::IOProcessor())
        std::cout << "       norm of residual        " << resnorm << std::endl;
//      std::cout << "       norm of grad_phi_prev_x " << gppxnorm << std::endl;
#if (BL_SPACEDIM > 1)
//      std::cout << "       norm of grad_phi_prev_y " << gppynorm << std::endl;
#endif
#if (BL_SPACEDIM > 2)
//      std::cout << "       norm of grad_phi_prev_z " << gppznorm << std::endl;
#endif
    }
}

void
Gravity::test_level_grad_phi_curr(int level)
{
    BL_PROFILE("Gravity::test_level_grad_phi_curr()");

    // Fill the RHS for the solve
    MultiFab& S_new = LevelData[level].get_new_data(State_Type);
    MultiFab Rhs(grids[level],1,0);
    MultiFab::Copy(Rhs,S_new,Density,0,1,0);

    // This is a correction for fully periodic domains only
    if ( Geometry::isAllPeriodic() )
    {
       if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
          std::cout << " ... subtracting average density from RHS in solve ... " << mass_offset << std::endl;
       Rhs.plus(-mass_offset,0,1,0);
    }

    Rhs.mult(Ggravity);

    if (verbose) {
       Real rhsnorm = Rhs.norm0();
       if (ParallelDescriptor::IOProcessor()) {
          std::cout << "... test_level_grad_phi_curr at level " << level << std::endl;
          std::cout << "       norm of RHS             " << rhsnorm << std::endl;
        }
    }

    const Real*     dx = parent->Geom(level).CellSize();
    const Real* problo = parent->Geom(level).ProbLo();
    int coord_type     = Geometry::Coord();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        // Test whether using the edge-based gradients
        //   to compute Div(Grad(Phi)) satisfies Lap(phi) = RHS
        // Fill the RHS array with the residual
        BL_FORT_PROC_CALL(CA_TEST_RESIDUAL,ca_test_residual)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(grad_phi_curr[level][0][mfi]),
                    BL_TO_FORTRAN(grad_phi_curr[level][1][mfi]),
                    BL_TO_FORTRAN(grad_phi_curr[level][2][mfi])),
                    dx,problo,&coord_type);
    }
    if (verbose) {
       Real resnorm = Rhs.norm0();
//     Real gppxnorm = grad_phi_curr[level][0].norm0();
#if (BL_SPACEDIM > 1)
//     Real gppynorm = grad_phi_curr[level][1].norm0();
#endif
#if (BL_SPACEDIM > 2)
//     Real gppznorm = grad_phi_curr[level][2].norm0();
#endif
       if (ParallelDescriptor::IOProcessor())
          std::cout << "       norm of residual        " << resnorm << std::endl;
//        std::cout << "       norm of grad_phi_curr_x " << gppxnorm << std::endl;
#if (BL_SPACEDIM > 1)
//        std::cout << "       norm of grad_phi_curr_y " << gppynorm << std::endl;
#endif
#if (BL_SPACEDIM > 2)
//        std::cout << "       norm of grad_phi_curr_z " << gppznorm << std::endl;
#endif
    }
}

void 
Gravity::create_comp_minus_level_grad_phi(int level, MultiFab& comp_minus_level_phi,
                                          PArray<MultiFab>& comp_minus_level_grad_phi) 
{
    BL_PROFILE("Gravity::create_comp_minus_level_grad_phi()");

    const MultiFab& phi_old = LevelData[level].get_old_data(PhiGrav_Type);

    MultiFab SL_phi;
    PArray<MultiFab> SL_grad_phi(BL_SPACEDIM,PArrayManage);

    SL_phi.define(grids[level],1,1,Fab_allocate);
    MultiFab::Copy(SL_phi,phi_old,0,0,1,0);

    comp_minus_level_phi.setVal(0.);
    for (int n=0; n<BL_SPACEDIM; ++n)
      comp_minus_level_grad_phi[n].setVal(0.);

    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        SL_grad_phi.clear(n);
        SL_grad_phi.set(n,new MultiFab(getEdgeBoxArray(level,n),1,0));
        SL_grad_phi[n].setVal(0.);
    }

    // Do level solve at beginning of time step in order to compute the
    //   difference between the multilevel and the single level solutions.

#ifdef PARTICLES
    BoxLib::Error("Particles + Gravity + AMR: here be dragons... ( Gravity.cpp Gravity::create_comp_minus_level_grad_phi() )");
#endif
    int is_new = 0;
    solve_for_phi(level, SL_phi, SL_grad_phi, is_new);

    if (verbose && ParallelDescriptor::IOProcessor())  
       std::cout << "... compute difference between level and composite solves at level " << level << '\n';

    comp_minus_level_phi.copy(phi_old,0,0,1);
    comp_minus_level_phi.minus(SL_phi,0,1,0);

    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        comp_minus_level_grad_phi[n].copy(grad_phi_prev[level][n],0,0,1);
        comp_minus_level_grad_phi[n].minus(SL_grad_phi[n],0,1,0);
    }

    // Just do this to release the memory
    for (int n=0; n<BL_SPACEDIM; ++n) SL_grad_phi.clear(n);
}

void
Gravity::add_to_fluxes(int level, int iteration, int ncycle)
{
    BL_PROFILE("Gravity::add_to_fluxes()");

    int finest_level = parent->finestLevel();
    FluxRegister* phi_fine = (level<finest_level ? &phi_flux_reg[level+1] : 0);
    FluxRegister* phi_current = (level>0 ? &phi_flux_reg[level] : 0);

    if (phi_fine) {

        for (int n=0; n<BL_SPACEDIM; ++n) {

            MultiFab fluxes(getEdgeBoxArray(level,n), 1, 0);

#ifdef _OPENMP
#pragma omp parallel
#endif
            for (MFIter mfi(fluxes,true); mfi.isValid(); ++mfi)
            {
		const Box& tbx = mfi.tilebox();
                FArrayBox& gphi_flux = fluxes[mfi];
                gphi_flux.copy(grad_phi_curr[level][n][mfi], tbx);
                gphi_flux.mult(area[level][n][mfi], tbx, 0, 0, 1);
            }

            phi_fine->CrseInit(fluxes,n,0,0,1,-1);
        }
    }

    if (phi_current && (iteration == ncycle))
    {
      MultiFab& phi_curr = LevelData[level].get_new_data(PhiGrav_Type);
      for (MFIter mfi(phi_curr); mfi.isValid(); ++mfi) 
      {
         for (int n=0; n<BL_SPACEDIM; ++n)
            phi_current->FineAdd(grad_phi_curr[level][n][mfi],area[level][n][mfi],n,mfi.index(),0,0,1,1.);
      }
    }

}

void
Gravity::average_fine_ec_onto_crse_ec(int level, int is_new)
{
    BL_PROFILE("Gravity::average_fine_ec_onto_crse_ec()");

    // NOTE: this is called with level == the coarser of the two levels involved
    if (level == parent->finestLevel()) return;

    //
    // Coarsen() the fine stuff on processors owning the fine data.
    //
    BoxArray crse_gphi_fine_BA(grids[level+1].size());

    IntVect fine_ratio = parent->refRatio(level);

    for (int i = 0; i < crse_gphi_fine_BA.size(); ++i)
        crse_gphi_fine_BA.set(i,BoxLib::coarsen(grids[level+1][i],fine_ratio));

    PArray<MultiFab> crse_gphi_fine(BL_SPACEDIM,PArrayManage);
    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        const BoxArray eba = BoxArray(crse_gphi_fine_BA).surroundingNodes(n);
        crse_gphi_fine.set(n,new MultiFab(eba,1,0));
    }

    if (is_new == 1)
    {
        BoxLib::average_down_faces(grad_phi_curr[level+1],crse_gphi_fine,fine_ratio);
	for (int n=0; n<BL_SPACEDIM; ++n)
	    grad_phi_curr[level][n].copy(crse_gphi_fine[n]);
    }
    else if (is_new == 0)
    {
        BoxLib::average_down_faces(grad_phi_prev[level+1],crse_gphi_fine,fine_ratio);
	for (int n=0; n<BL_SPACEDIM; ++n)
	    grad_phi_prev[level][n].copy(crse_gphi_fine[n]);
    }
}

void
Gravity::test_composite_phi (int crse_level)
{
    BL_PROFILE("Gravity::test_composite_phi()");

    if (verbose && ParallelDescriptor::IOProcessor()) {
        std::cout << "   " << '\n';
        std::cout << "... test_composite_phi at base level " << crse_level << '\n';
    }

    int finest_level = parent->finestLevel();
    int nlevels = finest_level - crse_level + 1;

    PArray<MultiFab> phi(nlevels, PArrayManage);
    PArray<MultiFab> rhs(nlevels, PArrayManage);
    PArray<MultiFab> res(nlevels, PArrayManage);
    for (int ilev = 0; ilev < nlevels; ++ilev) 
    {
	int amr_lev = crse_level + ilev;

	phi.set(ilev, new MultiFab(grids[amr_lev],1,1));
	MultiFab::Copy(phi[ilev], 
		       LevelData[amr_lev].get_new_data(PhiGrav_Type),
		       0,0,1,1);

	rhs.set(ilev, new MultiFab(grids[amr_lev],1,1));
	MultiFab::Copy(rhs[ilev], 
		       LevelData[amr_lev].get_new_data(State_Type),
		       Density,0,1,0);

	res.set(ilev, new MultiFab(grids[amr_lev],1,0));
	res[amr_lev].setVal(0.);
    }

    Array< PArray<MultiFab> > grad_phi_null;

    Real time = LevelData[crse_level].get_state_data(PhiGrav_Type).curTime();
    
    solve_phi_with_fmg(crse_level, finest_level,
		       phi, rhs, grad_phi_null, res, time);

    // Average residual from fine to coarse level before printing the norm
    for (int amr_lev = finest_level-1; amr_lev >= 0; --amr_lev)
    {
	const IntVect& ratio = parent->refRatio(amr_lev);
	BoxLib::average_down(res[amr_lev+1], res[amr_lev],
			     0, 1, ratio);
    } 

    for (int amr_lev = crse_level; amr_lev <= finest_level; ++amr_lev) {
	Real resnorm = res[amr_lev].norm0();
	if (ParallelDescriptor::IOProcessor()) {
	    std::cout << "      ... norm of composite residual at level " 
		      << amr_lev << "  " << resnorm << '\n';
	}
    }
    if (ParallelDescriptor::IOProcessor()) std::cout << std::endl;
}

void
Gravity::reflux_phi (int level, MultiFab& dphi)
{
    const Geometry& geom = parent->Geom(level);
    dphi.setVal(0.);
    phi_flux_reg[level+1].Reflux(dphi,volume[level],1.0,0,0,1,geom);
}

void 
Gravity::fill_ec_grow (int level,
                       PArray<MultiFab>&       ecF,
                       const PArray<MultiFab>& ecC) const
{
    BL_PROFILE("Gravity::fill_ec_grow()");

    //
    // Fill grow cells of the edge-centered mfs.  Assume
    // ecF built on edges of grids at this amr level, and ecC 
    // is build on edges of the grids at amr level-1
    //
    BL_ASSERT(ecF.size() == BL_SPACEDIM);

    const int nGrow = ecF[0].nGrow();

    if (nGrow == 0 || level == 0) return;

#if BL_SPACEDIM >= 2
    BL_ASSERT(nGrow == ecF[1].nGrow());
#endif
#if BL_SPACEDIM == 3
    BL_ASSERT(nGrow == ecF[2].nGrow());
#endif

    const BoxArray& fgrids = grids[level];
    const Geometry& fgeom  = parent->Geom(level);

    BoxList bl = BoxLib::GetBndryCells(fgrids,1);

    BoxArray f_bnd_ba(bl);

    bl.clear();

    BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

    IntVect crse_ratio = parent->refRatio(level-1);

    for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
        c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(crse_ratio));
        f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(crse_ratio));
    }
    
    for (int n = 0; n < BL_SPACEDIM; ++n)
    {
        //
        // crse_src & fine_src must have same parallel distribution.
        // We'll use the KnapSack distribution for the fine_src_ba.
        // Since fine_src_ba should contain more points, this'll lead
        // to a better distribution.
        //
        BoxArray crse_src_ba(c_bnd_ba);
        BoxArray fine_src_ba(f_bnd_ba);
        
        crse_src_ba.surroundingNodes(n);
        fine_src_ba.surroundingNodes(n);
        
        std::vector<long> wgts(fine_src_ba.size());
        
        for (unsigned int i = 0; i < wgts.size(); i++)
        {
            wgts[i] = fine_src_ba[i].numPts();
        }
        DistributionMapping dm;
        //
        // This call doesn't invoke the MinimizeCommCosts() stuff.
        // There's very little to gain with these types of coverings
        // of trying to use SFC or anything else.
        // This also guarantees that these DMs won't be put into the
        // cache, as it's not representative of that used for more
        // usual MultiFabs.
        //
        dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());
        
        MultiFab crse_src; crse_src.define(crse_src_ba, 1, 0, dm, Fab_allocate);
        MultiFab fine_src; fine_src.define(fine_src_ba, 1, 0, dm, Fab_allocate);
        
        crse_src.setVal(1.e200);
        fine_src.setVal(1.e200);
        //
        // We want to fill crse_src from ecC[n].
        //
	crse_src.copy(ecC[n]);  // parallel copy

#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
        {
            const int  nComp = 1;
            const Box& box   = crse_src[mfi].box();
            const int* rat   = crse_ratio.getVect();
            BL_FORT_PROC_CALL(CA_PC_EDGE_INTERP,ca_pc_edge_interp)
                (box.loVect(), box.hiVect(), &nComp, rat, &n,
                 BL_TO_FORTRAN(crse_src[mfi]),
                 BL_TO_FORTRAN(fine_src[mfi]));
        }

        crse_src.clear();
        //
        // Replace pc-interpd fine data with preferred u_mac data at
        // this level u_mac valid only on surrounding faces of valid
        // region - this op will not fill grow region.
        //
        fine_src.copy(ecF[n]); // parallel copy
        
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter mfi(fine_src); mfi.isValid(); ++mfi)
        {
            //
            // Interpolate unfilled grow cells using best data from
            // surrounding faces of valid region, and pc-interpd data
            // on fine edges overlaying coarse edges.
            //
            const int  nComp = 1;
            const Box& fbox  = fine_src[mfi].box();
            const int* rat   = crse_ratio.getVect();
            BL_FORT_PROC_CALL(CA_EDGE_INTERP,ca_edge_interp)
                (fbox.loVect(), fbox.hiVect(), &nComp, rat, &n,
                 BL_TO_FORTRAN(fine_src[mfi]));
        }

	ecF[n].copy(fine_src, 0, 0, 1, 0, ecF[n].nGrow()); // parallel copy
    }

    for (int n = 0; n < BL_SPACEDIM; ++n)
    {
	BoxLib::fill_boundary(ecF[n], fgeom);
    }
}

#if (BL_SPACEDIM == 1)
void
Gravity::make_one_d_grav(int level,Real time, MultiFab& grav_vector, MultiFab& phi)
{
    BL_PROFILE("Gravity::make_one_d_grav()");

   int ng = grav_vector.nGrow();

   AmrLevel* amrlev =                     &parent->getLevel(level) ;
   const Real* dx   = parent->Geom(level).CellSize();
   Box domain(parent->Geom(level).Domain());

   Box bx(grids[level].minimalBox());
   int lo = bx.loVect()[0];
   int hi = bx.hiVect()[0];
   bx.setSmall(0,lo-ng);
   bx.setBig(0,hi+ng);
   BoxArray ba(bx);

   FArrayBox grav_fab(bx,1);

   FArrayBox phi_fab(bx,1);

   grav_fab.setVal(0.0);
   phi_fab.setVal(0.0);
   
   // We only use mf for its BoxArray in the FillPatchIterator --
   //    it doesn't need to have enough components
   MultiFab mf(ba,1,0,Fab_allocate);

   const Real* problo = parent->Geom(level).ProbLo();

#ifdef GR_GRAV
   // Fill the state, interpolated from coarser levels where needed,
   //   and compute gravity by integrating outward from the center.
   //   Include post-Newtonian corrections.

   // We only use S_new to get the number of components for the GR routine
   MultiFab& S_new = LevelData[level].get_new_data(State_Type);
   int nvar = S_new.nComp(); 

   for (FillPatchIterator fpi(*amrlev,mf,0,time,State_Type,Density,nvar); 
        fpi.isValid(); ++fpi) 
   {
      BL_FORT_PROC_CALL(CA_COMPUTE_1D_GR_GRAV,ca_compute_1d_gr_grav)
          (BL_TO_FORTRAN(fpi()),grav_fab.dataPtr(),dx,problo);
   }
#else
   // Fill density, interpolated from coarser levels where needed,
   //   and compute gravity by integrating outward from the center
   for (FillPatchIterator fpi(*amrlev,mf,0,time,State_Type,Density,1); 
        fpi.isValid(); ++fpi) 
   {
      BL_FORT_PROC_CALL(CA_COMPUTE_1D_GRAV,ca_compute_1d_grav)
	(BL_TO_FORTRAN(fpi()),lo,hi,grav_fab.dataPtr(),phi_fab.dataPtr(),dx,problo);
   }
#endif

   // Only whichProc is used to fill grav_fab.
   int whichProc(mf.DistributionMap()[0]);

   ParallelDescriptor::Bcast(grav_fab.dataPtr(),grav_fab.box().numPts(),whichProc);
   ParallelDescriptor::Bcast(phi_fab.dataPtr(),phi_fab.box().numPts(),whichProc);

#ifdef _OPENMP
#pragma omp parallel
#endif   
   for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi) 
   {
        grav_vector[mfi].copy(grav_fab);
   }

#ifdef _OPENMP
#pragma omp parallel
#endif
   for (MFIter mfi(phi); mfi.isValid(); ++mfi)
   {
       phi[mfi].copy(phi_fab);
   }
      
}
#endif

#if (BL_SPACEDIM > 1)
void
Gravity::make_prescribed_grav(int level, Real time, MultiFab& grav_vector)
{
    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();

#ifdef _OPENMP
#pragma omp parallel
#endif   
    for (MFIter mfi(grav_vector,true); mfi.isValid(); ++mfi)
    {
       const Box& bx = mfi.growntilebox();
       BL_FORT_PROC_CALL(CA_PRESCRIBE_GRAV,ca_prescribe_grav)
	   (ARLIM_3D(bx.loVect()), ARLIM_3D(bx.hiVect()),
            BL_TO_FORTRAN_3D(grav_vector[mfi]),
	    dx);
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_prescribed_grav() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }
}

void
Gravity::interpolate_monopole_grav(int level, Array<Real>& radial_grav, MultiFab& grav_vector)
{
    int n1d = radial_grav.size();

    const Geometry& geom = parent->Geom(level);
    const Real* dx = geom.CellSize();
    Real dr        = dx[0] / double(drdxfac);

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(grav_vector,true); mfi.isValid(); ++mfi)
    {
       const Box& bx = mfi.growntilebox();
       BL_FORT_PROC_CALL(CA_PUT_RADIAL_GRAV,ca_put_radial_grav)
	   (bx.loVect(),bx.hiVect(),dx,&dr,
            BL_TO_FORTRAN(grav_vector[mfi]),
            radial_grav.dataPtr(),geom.ProbLo(),
            &n1d,&level);
    }
}
#endif

void
Gravity::make_radial_phi(int level, MultiFab& Rhs, MultiFab& phi, int fill_interior)
{
    BL_PROFILE("Gravity::make_radial_phi()");

    BL_ASSERT(level==0);

#if (BL_SPACEDIM > 1)
    const Real strt = ParallelDescriptor::second();

    int n1d = drdxfac*numpts_at_level;

    Array<Real> radial_mass(n1d,0.0);
    Array<Real> radial_vol(n1d,0.0);
    Array<Real> radial_phi(n1d,0.0);
    Array<Real> radial_grav(n1d+1,0.0);

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();
    Real dr = dx[0] / double(drdxfac);

    // Define total mass in each shell
    // Note that RHS = density (we have not yet multiplied by G)

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    PArray< Array<Real> > priv_radial_mass(nthreads, PArrayManage);
    PArray< Array<Real> > priv_radial_vol (nthreads, PArrayManage);
    for (int i=0; i<nthreads; i++) {
	priv_radial_mass.set(i, new Array<Real>(n1d,0.0));
	priv_radial_vol.set (i, new Array<Real>(n1d,0.0));
    }
#pragma omp parallel
#endif
    {
#ifdef _OPENMP
	int tid = omp_get_thread_num();
#endif
	for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
	{
	    const Box& bx =mfi.tilebox();
	    BL_FORT_PROC_CALL(CA_COMPUTE_RADIAL_MASS,ca_compute_radial_mass)
		(bx.loVect(), bx.hiVect(),dx,&dr,
		 BL_TO_FORTRAN(Rhs[mfi]), 
#ifdef _OPENMP
		 priv_radial_mass[tid].dataPtr(), priv_radial_vol[tid].dataPtr(),
#else
		 radial_mass.dataPtr(), radial_vol.dataPtr(),
#endif
		 geom.ProbLo(),&n1d,&drdxfac,&level);
	}

#ifdef _OPENMP
#pragma omp barrier
#pragma omp for
	for (int i=0; i<n1d; i++) {
	    for (int it=0; it<nthreads; it++) {
		radial_mass[i] += priv_radial_mass[it][i];
		radial_vol [i] += priv_radial_vol [it][i];		
	    }
	}
#endif
    }
   
    ParallelDescriptor::ReduceRealSum(radial_mass.dataPtr(),n1d);

    // Integrate radially outward to define the gravity
    BL_FORT_PROC_CALL(CA_INTEGRATE_PHI,ca_integrate_phi)
        (radial_mass.dataPtr(),radial_grav.dataPtr(),radial_phi.dataPtr(),&dr,&n1d);

    Box domain(parent->Geom(level).Domain());
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(phi,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.growntilebox();
        BL_FORT_PROC_CALL(CA_PUT_RADIAL_PHI,ca_put_radial_phi)
            (bx.loVect(), bx.hiVect(),
             domain.loVect(), domain.hiVect(),
             dx,&dr, BL_TO_FORTRAN(phi[mfi]),
             radial_phi.dataPtr(),geom.ProbLo(),
             &n1d,&fill_interior);
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_radial_phi() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }

#else
    BoxLib::Abort("Can't use make_radial_phi with dim == 1");
#endif
}


#if (BL_SPACEDIM == 3)
void
Gravity::fill_multipole_BCs(int level, MultiFab& Rhs, MultiFab& phi)
{
    BL_ASSERT(level==0);

    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();

    // Storage arrays for the multipole moments.
    // We will initialize them to zero, and then
    // sum up the results over grids.

    Box boxq0( IntVect(0, 0, 0), IntVect(lnum, 0,    numpts_at_level-1) );
    Box boxqC( IntVect(0, 0, 0), IntVect(lnum, lnum, numpts_at_level-1) );
    Box boxqS( IntVect(0, 0, 0), IntVect(lnum, lnum, numpts_at_level-1) );

    FArrayBox qL0(boxq0);
    FArrayBox qLC(boxqC);
    FArrayBox qLS(boxqS);

    FArrayBox qU0(boxq0);
    FArrayBox qUC(boxqC);
    FArrayBox qUS(boxqS);

    qL0.setVal(0.0);
    qLC.setVal(0.0);
    qLS.setVal(0.0);

    qU0.setVal(0.0);
    qUC.setVal(0.0);
    qUS.setVal(0.0);

    int boundary_only = 1;
    
    // Loop through the grids and compute the individual contributions
    // to the various moments. The multipole moment constructor
    // is coded to only add to the moment arrays, so it is safe
    // to directly hand the arrays to them.

    int lo_bc[3];
    int hi_bc[3];

    for (int dir = 0; dir < 3; dir++)
    {
      lo_bc[dir] = phys_bc->lo(dir);
      hi_bc[dir] = phys_bc->hi(dir);
    }

    int symmetry_type = Symmetry;
    const Box& domain = parent->Geom(level).Domain();

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    PArray<FArrayBox> priv_qL0(nthreads, PArrayManage);
    PArray<FArrayBox> priv_qLC(nthreads, PArrayManage);
    PArray<FArrayBox> priv_qLS(nthreads, PArrayManage);
    PArray<FArrayBox> priv_qU0(nthreads, PArrayManage);
    PArray<FArrayBox> priv_qUC(nthreads, PArrayManage);
    PArray<FArrayBox> priv_qUS(nthreads, PArrayManage);
    for (int i=0; i<nthreads; i++) {
	priv_qL0.set(i, new FArrayBox(boxq0));
	priv_qLC.set(i, new FArrayBox(boxqC));
	priv_qLS.set(i, new FArrayBox(boxqS));
        priv_qU0.set(i, new FArrayBox(boxq0));
	priv_qUC.set(i, new FArrayBox(boxqC));
	priv_qUS.set(i, new FArrayBox(boxqS));
    }
#pragma omp parallel
#endif
    {
#ifdef _OPENMP
	int tid = omp_get_thread_num();
	priv_qL0[tid].setVal(0.0);
	priv_qLC[tid].setVal(0.0);
	priv_qLS[tid].setVal(0.0);
	priv_qU0[tid].setVal(0.0);
	priv_qUC[tid].setVal(0.0);
	priv_qUS[tid].setVal(0.0);	
#endif
	for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
	{
	    const Box& bx = mfi.tilebox();
	    BL_FORT_PROC_CALL(CA_COMPUTE_MULTIPOLE_MOMENTS,ca_compute_multipole_moments)
		(bx.loVect(), bx.hiVect(), domain.loVect(), domain.hiVect(), 
		 &symmetry_type,lo_bc,hi_bc,
		 dx,BL_TO_FORTRAN(Rhs[mfi]),
		 &lnum,
#ifdef _OPENMP
		 priv_qL0[tid].dataPtr(),priv_qLC[tid].dataPtr(),priv_qLS[tid].dataPtr(),
		 priv_qU0[tid].dataPtr(),priv_qUC[tid].dataPtr(),priv_qUS[tid].dataPtr(),
#else
		 qL0.dataPtr(),qLC.dataPtr(),qLS.dataPtr(),
		 qU0.dataPtr(),qUC.dataPtr(),qUS.dataPtr(),
#endif
		 &numpts_at_level,&boundary_only);

	}

#ifdef _OPENMP
	int np0 = boxq0.numPts();
	int npC = boxqC.numPts();
	int npS = boxqS.numPts();
	Real* pL0 = qL0.dataPtr();
	Real* pLC = qLC.dataPtr();
	Real* pLS = qLS.dataPtr();
	Real* pU0 = qU0.dataPtr();
	Real* pUC = qUC.dataPtr();
	Real* pUS = qUS.dataPtr();
#pragma omp barrier
#pragma omp for nowait
	for (int i=0; i<np0; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qL0[it].dataPtr();
		pL0[i] += pp[i];
	    }
	}
#pragma omp for nowait
	for (int i=0; i<npC; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qLC[it].dataPtr();
		pLC[i] += pp[i];
	    }
	}
#pragma omp for nowait
	for (int i=0; i<npS; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qLS[it].dataPtr();
		pLS[i] += pp[i];
	    }
	}
#pragma omp for nowait
	for (int i=0; i<np0; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qU0[it].dataPtr();
		pU0[i] += pp[i];
	    }
	}
#pragma omp for nowait
	for (int i=0; i<npC; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qUC[it].dataPtr();
		pUC[i] += pp[i];
	    }
	}
#pragma omp for nowait
	for (int i=0; i<npS; ++i)
	{
	    for (int it=0; it<nthreads; it++) {
		const Real* pp = priv_qUS[it].dataPtr();
		pUS[i] += pp[i];
	    }
	}
#endif
    }

    // Now, do a global reduce over all processes.

    ParallelDescriptor::ReduceRealSum(qL0.dataPtr(),boxq0.numPts());
    ParallelDescriptor::ReduceRealSum(qLC.dataPtr(),boxqC.numPts());
    ParallelDescriptor::ReduceRealSum(qLS.dataPtr(),boxqS.numPts());

    if (boundary_only != 1) {
      ParallelDescriptor::ReduceRealSum(qU0.dataPtr(),boxq0.numPts());
      ParallelDescriptor::ReduceRealSum(qUC.dataPtr(),boxqC.numPts());
      ParallelDescriptor::ReduceRealSum(qUS.dataPtr(),boxqS.numPts());
    }
    
    // Finally, construct the boundary conditions using the
    // complete multipole moments, for all points on the
    // boundary that are held on this process.

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(phi,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.growntilebox();
        BL_FORT_PROC_CALL(CA_PUT_MULTIPOLE_PHI,ca_put_multipole_phi)
            (bx.loVect(), bx.hiVect(),
             domain.loVect(), domain.hiVect(),
             dx, BL_TO_FORTRAN(phi[mfi]),
             &lnum,
	     qL0.dataPtr(),qLC.dataPtr(),qLS.dataPtr(),
	     qU0.dataPtr(),qUC.dataPtr(),qUS.dataPtr(),
	     &numpts_at_level,&boundary_only);
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::fill_multipole_BCs() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }

}

void
Gravity::fill_direct_sum_BCs(int level, MultiFab& Rhs, MultiFab& phi)
{
    BL_ASSERT(level==0);

    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();

    // Storage arrays for the BCs.

    const int* domlo = geom.Domain().loVect();
    const int* domhi = geom.Domain().hiVect();

    const int loVectXY[3] = {domlo[0]-1, domlo[1]-1, 0         };
    const int hiVectXY[3] = {domhi[0]+1, domhi[1]+1, 0         };

    const int loVectXZ[3] = {domlo[0]-1, 0         , domlo[2]-1};
    const int hiVectXZ[3] = {domhi[0]+1, 0         , domhi[2]+1};

    const int loVectYZ[3] = {0         , domlo[1]-1, domlo[2]-1};
    const int hiVectYZ[3] = {0         , domhi[1]+1, domhi[1]+1};

    IntVect smallEndXY( loVectXY );
    IntVect bigEndXY  ( hiVectXY );
    IntVect smallEndXZ( loVectXZ );
    IntVect bigEndXZ  ( hiVectXZ );
    IntVect smallEndYZ( loVectYZ );
    IntVect bigEndYZ  ( hiVectYZ );

    Box boxXY(smallEndXY, bigEndXY);
    Box boxXZ(smallEndXZ, bigEndXZ);
    Box boxYZ(smallEndYZ, bigEndYZ);

    const long nPtsXY = boxXY.numPts();
    const long nPtsXZ = boxXZ.numPts();
    const long nPtsYZ = boxYZ.numPts(); 

    FArrayBox bcXYLo(boxXY);
    FArrayBox bcXYHi(boxXY);
    FArrayBox bcXZLo(boxXZ);
    FArrayBox bcXZHi(boxXZ);
    FArrayBox bcYZLo(boxYZ);
    FArrayBox bcYZHi(boxYZ);

    bcXYLo.setVal(0.0);
    bcXYHi.setVal(0.0);
    bcXZLo.setVal(0.0);
    bcXZHi.setVal(0.0);
    bcYZLo.setVal(0.0);
    bcYZHi.setVal(0.0);
    
    // Loop through the grids and compute the individual contributions
    // to the BCs. The BC constructor is coded to only add to the 
    // BCs, so it is safe to directly hand the arrays to them.

    int lo_bc[3];
    int hi_bc[3];

    for (int dir = 0; dir < 3; dir++)
    {
      lo_bc[dir] = phys_bc->lo(dir);
      hi_bc[dir] = phys_bc->hi(dir);
    }

    int symmetry_type = Symmetry;
    
#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    PArray<FArrayBox> priv_bcXYLo(nthreads, PArrayManage);
    PArray<FArrayBox> priv_bcXYHi(nthreads, PArrayManage);
    PArray<FArrayBox> priv_bcXZLo(nthreads, PArrayManage);
    PArray<FArrayBox> priv_bcXZHi(nthreads, PArrayManage);
    PArray<FArrayBox> priv_bcYZLo(nthreads, PArrayManage);
    PArray<FArrayBox> priv_bcYZHi(nthreads, PArrayManage);
    for (int i=0; i<nthreads; i++) {
	priv_bcXYLo.set(i, new FArrayBox(boxXY));
	priv_bcXYHi.set(i, new FArrayBox(boxXY));
	priv_bcXZLo.set(i, new FArrayBox(boxXZ));
	priv_bcXZHi.set(i, new FArrayBox(boxXZ));
	priv_bcYZLo.set(i, new FArrayBox(boxYZ));
	priv_bcYZHi.set(i, new FArrayBox(boxYZ));
    }
#pragma omp parallel
#endif
    {
#ifdef _OPENMP
	int tid = omp_get_thread_num();
	priv_bcXYLo[tid].setVal(0.0);
	priv_bcXYHi[tid].setVal(0.0);
	priv_bcXZLo[tid].setVal(0.0);
	priv_bcXZHi[tid].setVal(0.0);
	priv_bcYZLo[tid].setVal(0.0);
	priv_bcYZHi[tid].setVal(0.0);
#endif
	for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
	{
	    const Box bx = mfi.tilebox();
	    BL_FORT_PROC_CALL(CA_COMPUTE_DIRECT_SUM_BC,ca_compute_direct_sum_bc)
		(bx.loVect(), bx.hiVect(), domlo, domhi, 
		 &symmetry_type,lo_bc,hi_bc,
		 dx,BL_TO_FORTRAN(Rhs[mfi]),
		 geom.ProbLo(),geom.ProbHi(),
#ifdef _OPENMP
		 priv_bcXYLo[tid].dataPtr(), priv_bcXYHi[tid].dataPtr(),
		 priv_bcXZLo[tid].dataPtr(), priv_bcXZHi[tid].dataPtr(),
		 priv_bcYZLo[tid].dataPtr(), priv_bcYZHi[tid].dataPtr()
#else
		 bcXYLo.dataPtr(), bcXYHi.dataPtr(),
		 bcXZLo.dataPtr(), bcXZHi.dataPtr(),
		 bcYZLo.dataPtr(), bcYZHi.dataPtr()
#endif
		    );
	}

#ifdef _OPENMP
	Real* pXYLo = bcXYLo.dataPtr();
	Real* pXYHi = bcXYHi.dataPtr();
	Real* pXZLo = bcXZLo.dataPtr();
	Real* pXZHi = bcXZHi.dataPtr();
	Real* pYZLo = bcYZLo.dataPtr();
	Real* pYZHi = bcYZHi.dataPtr();
#pragma omp barrier
#pragma omp for nowait
        for (int i=0; i<nPtsXY; i++) {
	    for (int it=0; it<nthreads; it++) {
		const Real* pl = priv_bcXYLo[it].dataPtr();
		const Real* ph = priv_bcXYHi[it].dataPtr();
		pXYLo[i] += pl[i];
		pXYHi[i] += ph[i];
	    }
	}
#pragma omp for nowait
        for (int i=0; i<nPtsXZ; i++) {
	    for (int it=0; it<nthreads; it++) {
		const Real* pl = priv_bcXZLo[it].dataPtr();
		const Real* ph = priv_bcXZHi[it].dataPtr();
		pXZLo[i] += pl[i];
		pXZHi[i] += ph[i];
	    }
	}
#pragma omp for nowait
        for (int i=0; i<nPtsYZ; i++) {
	    for (int it=0; it<nthreads; it++) {
		const Real* pl = priv_bcYZLo[it].dataPtr();
		const Real* ph = priv_bcYZHi[it].dataPtr();
		pYZLo[i] += pl[i];
		pYZHi[i] += ph[i];
	    }
	}
#endif
    }

    // because the number of elments in mpi_reduce is int
    BL_ASSERT(nPtsXY <= std::numeric_limits<int>::max());
    BL_ASSERT(nPtsXZ <= std::numeric_limits<int>::max());
    BL_ASSERT(nPtsYZ <= std::numeric_limits<int>::max());

    ParallelDescriptor::ReduceRealSum(bcXYLo.dataPtr(), nPtsXY);
    ParallelDescriptor::ReduceRealSum(bcXYHi.dataPtr(), nPtsXY);
    ParallelDescriptor::ReduceRealSum(bcXZLo.dataPtr(), nPtsXZ);
    ParallelDescriptor::ReduceRealSum(bcXZHi.dataPtr(), nPtsXZ);
    ParallelDescriptor::ReduceRealSum(bcYZLo.dataPtr(), nPtsYZ);
    ParallelDescriptor::ReduceRealSum(bcYZHi.dataPtr(), nPtsYZ);
    
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(phi,true); mfi.isValid(); ++mfi)
    {
        const Box& bx= mfi.growntilebox();
        BL_FORT_PROC_CALL(CA_PUT_DIRECT_SUM_BC,ca_put_direct_sum_bc)
            (bx.loVect(), bx.hiVect(), domlo, domhi,
             BL_TO_FORTRAN(phi[mfi]),
             bcXYLo.dataPtr(), bcXYHi.dataPtr(),
             bcXZLo.dataPtr(), bcXZHi.dataPtr(),
             bcYZLo.dataPtr(), bcYZHi.dataPtr());
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::fill_direct_sum_BCs() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }
    
}
#endif

#if (BL_SPACEDIM < 3)
void
Gravity::applyMetricTerms(int level, MultiFab& Rhs, PArray<MultiFab>& coeffs)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(Rhs,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
	D_TERM(const Box& xbx = mfi.nodaltilebox(0);,
	       const Box& ybx = mfi.nodaltilebox(1);,
	       const Box& zbx = mfi.nodaltilebox(2););
        // Modify Rhs and coeffs with the appropriate metric terms.
        BL_FORT_PROC_CALL(CA_APPLY_METRIC,ca_apply_metric)
            (bx.loVect(), bx.hiVect(),
	     D_DECL(xbx.loVect(),
		    ybx.loVect(),
		    zbx.loVect()),
	     D_DECL(xbx.hiVect(),
		    ybx.hiVect(),
		    zbx.hiVect()),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(coeffs[0][mfi]),
                    BL_TO_FORTRAN(coeffs[1][mfi]),
                    BL_TO_FORTRAN(coeffs[2][mfi])),
                    dx,&coord_type);
    }
}

void
Gravity::unweight_cc(int level, MultiFab& cc)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(cc,true); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        BL_FORT_PROC_CALL(CA_UNWEIGHT_CC,ca_unweight_cc)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(cc[mfi]),dx,&coord_type);
    }
}

void
Gravity::unweight_edges(int level, PArray<MultiFab>& edges)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
#ifdef _OPENMP
#pragma omp parallel
#endif    
    for (int idir=0; idir<BL_SPACEDIM; ++idir) {
	for (MFIter mfi(edges[idir],true); mfi.isValid(); ++mfi)
	{
	    const Box& bx = mfi.tilebox();
	    BL_FORT_PROC_CALL(CA_UNWEIGHT_EDGES,ca_unweight_edges)
		(bx.loVect(), bx.hiVect(),
		 BL_TO_FORTRAN(edges[idir][mfi]),
		 dx,&coord_type,&idir);
	}
    }
}
#endif

void
Gravity::make_mg_bc ()
{
    const Geometry& geom = parent->Geom(0);
    for ( int dir = 0; dir < BL_SPACEDIM; ++dir )
    {
        if ( geom.isPeriodic(dir) )
        {
            mg_bc[2*dir + 0] = 0;
            mg_bc[2*dir + 1] = 0;
        }
        else
        {
            if (phys_bc->lo(dir) == Symmetry) {
              mg_bc[2*dir + 0] = MGT_BC_NEU;
            } else if (phys_bc->lo(dir) == Outflow) {
              mg_bc[2*dir + 0] = MGT_BC_DIR;
            } else {
              BoxLib::Abort("Unknown lo bc in make_mg_bc");
            }
            if (phys_bc->hi(dir) == Symmetry) {
              mg_bc[2*dir + 1] = MGT_BC_NEU;
            } else if (phys_bc->hi(dir) == Outflow) {
              mg_bc[2*dir + 1] = MGT_BC_DIR;
            } else {
              BoxLib::Abort("Unknown hi bc in make_mg_bc");
            }
        }
    }

    // Set Neumann bc at r=0.
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
        mg_bc[0] = MGT_BC_NEU;
}

void
Gravity::set_mass_offset (Real time, bool multi_level)
{
    const Geometry& geom = parent->Geom(0);

    if (!geom.isAllPeriodic()) {
	mass_offset = 0.0;
    } 
    else {
	Real old_mass_offset = mass_offset;
	mass_offset = 0.0;

	if (multi_level)
	{
	    for (int lev = 0; lev <= parent->finestLevel(); lev++) {
		Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(lev));
		mass_offset += cs->volWgtSum("density", time);    
	    }
	} 
	else 
	{
	    Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(0));
	    mass_offset = cs->volWgtSum("density", time, false, false);  // do not mask off fine grids
	}

#ifdef PARTICLES
	if ( Castro::theDMPC() ) {
	    for (int lev = 0; lev <= parent->finestLevel(); lev++) {
                mass_offset += Castro::theDMPC()->sumParticleMass(lev);
            }
        }
#endif
 
	mass_offset = mass_offset / geom.ProbSize();
	if (verbose && ParallelDescriptor::IOProcessor()) 
	    std::cout << "Defining average density to be " << mass_offset << std::endl;

	Real diff = std::abs(mass_offset - old_mass_offset);
	Real eps = 1.e-10 * std::abs(old_mass_offset);
	if (diff > eps && old_mass_offset > 0)
	{
	    if (ParallelDescriptor::IOProcessor())
	    {
	        std::cout << " ... new vs old mass_offset " << mass_offset << " " << old_mass_offset
			  << " ... diff is " << diff <<  std::endl;
		std::cout << " ... Gravity::set_mass_offset -- total mass has changed!" << std::endl;;
            }
        }
    }
}

#ifdef POINTMASS
void
Gravity::add_pointmass_to_gravity (int level, MultiFab& grav_vector, Real point_mass)
{
   const Real* dx     = parent->Geom(level).CellSize();
   const Real* problo = parent->Geom(level).ProbLo();
#ifdef _OPENMP
#pragma omp parallel
#endif    
   for (MFIter mfi(grav_vector,true); mfi.isValid(); ++mfi)
   {
       const Box& bx = mfi.growntilebox();
       BL_FORT_PROC_CALL(PM_ADD_TO_GRAV,pm_add_to_grav)
	   (&point_mass,BL_TO_FORTRAN(grav_vector[mfi]),
	    problo,dx,bx.loVect(),bx.hiVect());
   }
}
#endif

#if (BL_SPACEDIM == 3)
Real
Gravity::computeAvg (int level, MultiFab* mf, bool mask)
{
    BL_PROFILE("Gravity::computeAvg()");

    Real        sum     = 0.0;

    const Geometry& geom = parent->Geom(level);
    const Real* dx       = geom.CellSize();

    BL_ASSERT(mf != 0);

    if (level < parent->finestLevel() && mask)
    {
	Castro* fine_level = dynamic_cast<Castro*>(&(parent->getLevel(level+1)));
	const MultiFab* mask = fine_level->build_fine_mask();
	MultiFab::Multiply(*mf, *mask, 0, 0, 1, 0);	
    }

#ifdef _OPENMP
#pragma omp parallel reduction(+:sum)
#endif    
    for (MFIter mfi(*mf,true); mfi.isValid(); ++mfi)
    {
        FArrayBox& fab = (*mf)[mfi];

        Real s;
        const Box& box  = mfi.tilebox();
        const int* lo   = box.loVect();
        const int* hi   = box.hiVect();

        //
        // Note that this routine will do a volume weighted sum of
        // whatever quantity is passed in, not strictly the "mass".
        //
	BL_FORT_PROC_CALL(CA_SUMMASS,ca_summass)
	    (ARLIM_3D(lo),ARLIM_3D(hi),BL_TO_FORTRAN_3D(fab),dx,BL_TO_FORTRAN_3D(volume[level][mfi]),&s);
        sum += s;
    }

    ParallelDescriptor::ReduceRealSum(sum);

    return sum;
}
#endif

#if (BL_SPACEDIM > 1)
void
Gravity::make_radial_gravity(int level, Real time, Array<Real>& radial_grav)
{
    BL_PROFILE("Gravity::make_radial_gravity()");

    const Real strt = ParallelDescriptor::second();

    // This is just here in case we need to debug ...
    int do_diag = 0;

    Real sum_over_levels = 0.;

    for (int lev = 0; lev <= level; lev++)
    {
        const Real t_old = LevelData[lev].get_state_data(State_Type).prevTime();
        const Real t_new = LevelData[lev].get_state_data(State_Type).curTime();
        const Real eps   = (t_new - t_old) * 1.e-6;

        // Create MultiFab with one component and no grow cells
        MultiFab S(grids[lev],1,0);

        if ( std::abs(time-t_old) < eps)
        {
            S.copy(LevelData[lev].get_old_data(State_Type),Density,0,1);
        } 
        else if ( std::abs(time-t_new) < eps)
        {
            S.copy(LevelData[lev].get_new_data(State_Type),Density,0,1);
            if (lev < level)
            {
                Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(lev+1));
                cs->getFluxReg().Reflux(S,volume[lev],1.0,0,0,1,parent->Geom(lev));
            }
        } 
        else if (time > t_old && time < t_new)
        {
            Real alpha   = (time - t_old)/(t_new - t_old);
            Real omalpha = 1.0 - alpha;

            S.copy(LevelData[lev].get_old_data(State_Type),Density,0,1);
            S.mult(omalpha);

            MultiFab S_new(grids[lev],1,0);
            S_new.copy(LevelData[lev].get_new_data(State_Type),Density,0,1);
            S_new.mult(alpha);

            S.plus(S_new,Density,1,0);
        }  
        else
        {  
     	    std::cout << " Level / Time in make_radial_gravity is: " << lev << " " << time  << std::endl;
      	    std::cout << " but old / new time      are: " << t_old << " " << t_new << std::endl;
      	    BoxLib::Abort("Problem in Gravity::make_radial_gravity");
        }  

        if (lev < level)
        {
	    Castro* fine_level = dynamic_cast<Castro*>(&(parent->getLevel(lev+1)));
	    const MultiFab* mask = fine_level->build_fine_mask();
	    MultiFab::Multiply(S, *mask, 0, 0, 1, 0);	
        }

        int n1d = radial_mass[lev].size();

#ifdef GR_GRAV
        for (int i = 0; i < n1d; i++) radial_pres[lev][i] = 0.;
#endif
        for (int i = 0; i < n1d; i++) radial_vol[lev][i] = 0.;
        for (int i = 0; i < n1d; i++) radial_mass[lev][i] = 0.;

        const Geometry& geom = parent->Geom(lev);
        const Real* dx   = geom.CellSize();
        Real dr = dx[0] / double(drdxfac);

#ifdef _OPENMP
	int nthreads = omp_get_max_threads();
#ifdef GR_GRAV
	PArray< Array<Real> > priv_radial_pres(nthreads, PArrayManage);
#endif
	PArray< Array<Real> > priv_radial_mass(nthreads, PArrayManage);
	PArray< Array<Real> > priv_radial_vol (nthreads, PArrayManage);
	for (int i=0; i<nthreads; i++) {
#ifdef GR_GRAV
	    priv_radial_pres.set(i, new Array<Real>(n1d,0.0));
#endif
	    priv_radial_mass.set(i, new Array<Real>(n1d,0.0));
	    priv_radial_vol.set (i, new Array<Real>(n1d,0.0));
	}
#pragma omp parallel
#endif
	{
#ifdef _OPENMP
	    int tid = omp_get_thread_num();
#endif	
	    for (MFIter mfi(S,true); mfi.isValid(); ++mfi)
	    {
	        const Box& bx = mfi.tilebox();
		FArrayBox& fab = S[mfi];
		
		BL_FORT_PROC_CALL(CA_COMPUTE_RADIAL_MASS,ca_compute_radial_mass)
		    (bx.loVect(), bx.hiVect(), dx, &dr,
                     BL_TO_FORTRAN(fab), 
#ifdef _OPENMP
                     priv_radial_mass[tid].dataPtr(), 
                     priv_radial_vol[tid].dataPtr(), 
#else
                     radial_mass[lev].dataPtr(), 
                     radial_vol[lev].dataPtr(), 
#endif
                     geom.ProbLo(),&n1d,&drdxfac,&lev);
		
#ifdef GR_GRAV
		BL_FORT_PROC_CALL(CA_COMPUTE_AVGPRES,ca_compute_avgpres)
		    (bx.loVect(), bx.hiVect(),dx,&dr,
                     BL_TO_FORTRAN(fab),
#ifdef _OPENMP
                     priv_radial_pres.dataPtr(),
#else
                     radial_pres[lev].dataPtr(),
#endif
                     geom.ProbLo(),&n1d,&drdxfac,&lev);
#endif
	    }

#ifdef _OPENMP
#pragma omp barrier
#pragma omp for
	    for (int i=0; i<n1d; i++) {
		for (int it=0; it<nthreads; it++) {
#ifdef GR_GRAV
	            radial_pres[lev][i] += priv_radial_pres[it][i];
#endif
	            radial_mass[lev][i] += priv_radial_mass[it][i];
		    radial_vol [lev][i] += priv_radial_vol [it][i];		
		}
	    }
#endif
	}

        ParallelDescriptor::ReduceRealSum(radial_mass[lev].dataPtr() ,n1d);
        ParallelDescriptor::ReduceRealSum(radial_vol[lev].dataPtr()  ,n1d);
#ifdef GR_GRAV
        ParallelDescriptor::ReduceRealSum(radial_pres[lev].dataPtr()  ,n1d);
#endif

        if (do_diag > 0)
        {
            Real sum = 0.;
            for (int i = 0; i < n1d; i++) sum += radial_mass[lev][i];
            sum_over_levels += sum;
        }
    }

    if (do_diag > 0 && ParallelDescriptor::IOProcessor())
        std::cout << "Gravity::make_radial_gravity: Sum of mass over all levels " << sum_over_levels << std::endl;

    int n1d = radial_mass[level].size();
    Array<Real> radial_mass_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
    {
        radial_mass_summed[i] = radial_mass[level][i];
    }

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        int ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
            {
                for (int n = 0; n < ratio; n++)
                {
                   radial_mass_summed[ratio*i+n] += 1./double(ratio) * radial_mass[lev][i];
                }
            }
        }
    }

    if (do_diag > 0 && ParallelDescriptor::IOProcessor())
    {
        Real sum_added = 0.;
        for (int i = 0; i < n1d; i++) sum_added += radial_mass_summed[i];
        std::cout << "Gravity::make_radial_gravity: Sum of combined mass " << sum_added << std::endl;
    }

    const Geometry& geom = parent->Geom(level);
    const Real* dx = geom.CellSize();
    Real dr        = dx[0] / double(drdxfac);

    // ***************************************************************** //
    // Compute the average density to use at the radius above
    //   max_radius_all_in_domain so we effectively count mass outside
    //   the domain.
    // ***************************************************************** //

    Array<Real> radial_vol_summed(n1d,0);
    Array<Real> radial_den_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
         radial_vol_summed[i] =  radial_vol[level][i];

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        int ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
            {
                for (int n = 0; n < ratio; n++)
                {
                   radial_vol_summed[ratio*i+n]  += 1./double(ratio) * radial_vol[lev][i];
                }
            }
        }
    }

    for (int i = 0; i < n1d; i++)  
    {
        radial_den_summed[i] = radial_mass_summed[i];
        if (radial_vol_summed[i] > 0.) radial_den_summed[i]  /= radial_vol_summed[i];
    }

#ifdef GR_GRAV
    Array<Real> radial_pres_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
        radial_pres_summed[i] = radial_pres[level][i];

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
                for (int n = 0; n < ratio; n++)
                   radial_pres_summed[ratio*i+n] += 1./double(ratio) * radial_pres[lev][i];
        }
    }

    for (int i = 0; i < n1d; i++)  
        if (radial_vol_summed[i] > 0.) radial_pres_summed[i] /= radial_vol_summed[i];

    // Integrate radially outward to define the gravity -- here we add the post-Newtonian correction
    BL_FORT_PROC_CALL(CA_INTEGRATE_GR_GRAV,ca_integrate_gr_grav)
        (radial_den_summed.dataPtr(),radial_mass_summed.dataPtr(),
         radial_pres_summed.dataPtr(),radial_grav.dataPtr(),&dr,&n1d);

#else
    // Integrate radially outward to define the gravity
    BL_FORT_PROC_CALL(CA_INTEGRATE_GRAV,ca_integrate_grav)
        (radial_mass_summed.dataPtr(),radial_den_summed.dataPtr(),
         radial_grav.dataPtr(),&max_radius_all_in_domain,&dr,&n1d); 
#endif
   
    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

#ifdef BL_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(end,IOProc);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_radial_gravity() time = " << end << std::endl;
#ifdef BL_LAZY
	});
#endif
    }
}

#ifdef PARTICLES
void
Gravity::AddParticlesToRhs (int               level,
                            MultiFab&         Rhs,
                            int               ngrow)
{
    if( Castro::theDMPC() )
    {
        MultiFab particle_mf(grids[level], 1, ngrow);
        particle_mf.setVal(0.);
        Castro::theDMPC()->AssignDensitySingleLevel(particle_mf, level);
        MultiFab::Add(Rhs, particle_mf, 0, 0, 1, 0);
    }
}

void
Gravity::AddParticlesToRhs(int base_level, int finest_level, PArray<MultiFab>& Rhs_particles)
{
    const int num_levels = finest_level - base_level + 1;

    if( Castro::theDMPC() )
    {
        PArray<MultiFab> PartMF;
        Castro::theDMPC()->AssignDensity(PartMF, base_level, 1, finest_level);
        for (int lev = 0; lev < num_levels; lev++)
        {
            if (PartMF[lev].contains_nan(true))
            {
                std::cout << "Testing particle density at level " << base_level+lev << std::endl;
                BoxLib::Abort("...PartMF has NaNs in Gravity::actual_multilevel_solve()");
            }
        }

        for (int lev = finest_level - 1 - base_level; lev >= 0; lev--)
        {
            const IntVect& ratio = parent->refRatio(lev+base_level);
	    BoxLib::average_down(PartMF[lev+1], PartMF[lev],
				 0, 1, ratio);
        }

        for (int lev = 0; lev < num_levels; lev++)
        {
            MultiFab::Add(Rhs_particles[lev], PartMF[lev], 0, 0, 1, 0);
        }
    }
}
#endif
#endif

Real
Gravity::solve_phi_with_fmg (int crse_level, int fine_level,
			     PArray<MultiFab> & phi,
			     PArray<MultiFab> & rhs,
			     Array< PArray<MultiFab> >& grad_phi,
			     PArray<MultiFab>& res,
			     Real time)
{
    BL_PROFILE("Gravity::solve_phi_with_fmg()");

    int nlevs = fine_level-crse_level+1;

    if (crse_level == 0 && !Geometry::isAllPeriodic())
    {
        if (verbose && ParallelDescriptor::IOProcessor()) 
	    std::cout << " ... Making bc's for phi at level 0 " << std::endl;

#if (BL_SPACEDIM == 3)
	if ( direct_sum_bcs ) {
	    fill_direct_sum_BCs(crse_level, rhs[0], phi[0]);
        } else {
	    fill_multipole_BCs(crse_level, rhs[0], phi[0]);
	}
#else
	int fill_interior = 0;
	make_radial_phi(crse_level, rhs[0], phi[0], fill_interior);
#endif
    }

#if (BL_SPACEDIM == 3)
    if ( Geometry::isAllPeriodic() )
    {
	if (verbose && ParallelDescriptor::IOProcessor()) {
	    std::cout << " ... subtracting average density " << mass_offset 
		      << " from RHS in solve at levels " 
		      << crse_level << " to " << fine_level << std::endl;
	}
	for (int ilev = 0; ilev < nlevs; ++ilev) {
	    rhs[ilev].plus(-mass_offset,0,1,0);
	}
    }
#endif

    for (int ilev = 0; ilev < nlevs; ++ilev)
    {
        rhs[ilev].mult(Ggravity);
    }

    PArray<Geometry> geom(nlevs);
    for (int ilev = 0; ilev < nlevs; ++ilev) {
	int amr_lev = ilev + crse_level;
	geom.set(ilev, &(parent->Geom(amr_lev)));
    }
    
    IntVect crse_ratio = crse_level > 0 ? parent->refRatio(crse_level-1)
                                        : IntVect::TheZeroVector();

    FMultiGrid fmg(geom, crse_level, crse_ratio);

    MultiFab CPhi;  // need to be here so that it is still alive when solve is called.
    if (crse_level == 0) {
	fmg.set_bc(mg_bc, phi[0]);
    } else {
        GetCrsePhi(crse_level, CPhi, time);
	fmg.set_bc(mg_bc, CPhi, phi[0]);
    }

    Array<PArray<MultiFab> > coeffs(nlevs);
#if (BL_SPACEDIM < 3)
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
	for (int ilev = 0; ilev < nlevs; ++ilev) {
	    int amr_lev = ilev + crse_level;	    
	    coeffs[ilev].resize(BL_SPACEDIM, PArrayManage);
	    for (int i = 0; i < BL_SPACEDIM ; i++) {
		coeffs[ilev].set(i, new MultiFab(grids[amr_lev], 1, 0, Fab_allocate, 
						 IntVect::TheDimensionVector(i)));
		coeffs[ilev][i].setVal(1.0);
	    }

	    applyMetricTerms(amr_lev, rhs[ilev], coeffs[ilev]);
	}

	fmg.set_gravity_coeffs(coeffs);
    } 
    else 
#endif
    {
	fmg.set_const_gravity_coeffs();
    }

    Real final_resnorm = -1.0;

    if (grad_phi.size() > 0)
    {
	Real     tol = (nlevs == 1) ? sl_tol : ml_tol;
	Real abs_tol = 0.0;
	int need_grad_phi = 1;
	int always_use_bnorm = (Geometry::isAllPeriodic()) ? 0 : 1;
	final_resnorm = fmg.solve(phi, rhs, tol, abs_tol, 
				  always_use_bnorm, need_grad_phi);

	fmg.get_fluxes(grad_phi);
	
#if (BL_SPACEDIM < 3)
	if (Geometry::IsSPHERICAL() || Geometry::IsRZ()) {
	    for (int ilev = 0; ilev < nlevs; ++ilev) 
	    {
		int amr_lev = ilev + crse_level;
		//    Need to un-weight the fluxes
		unweight_edges(amr_lev, grad_phi[ilev]);
	    }
	}
#endif
    }

    if (res.size() > 0)
    {
	fmg.compute_residual(phi, rhs, res);

#if (BL_SPACEDIM < 3)
	// unweight the residual
	if (Geometry::IsSPHERICAL() || Geometry::IsRZ() ) {
	    for (int ilev = 0; ilev < nlevs; ++ilev) 
	    {
		int amr_level = ilev + crse_level;
		unweight_cc(amr_level, res[ilev]);
	    }
	}
#endif
    }

    return final_resnorm;
}

void
Gravity::get_rhs (int crse_level, int nlevs, PArray<MultiFab> & rhs, int is_new)
{
    rhs.resize(nlevs, PArrayManage);

    for (int ilev = 0; ilev < nlevs; ++ilev) 
    {
	int amr_lev = ilev + crse_level;	
	rhs.set(ilev, new MultiFab(grids[amr_lev],1,0));
	MultiFab& state = (is_new == 1) ?
	    LevelData[amr_lev].get_new_data(State_Type) :
	    LevelData[amr_lev].get_old_data(State_Type);
	MultiFab::Copy(rhs[ilev], state, Density,0,1,0);
    }

#ifdef PARTICLES
    if ( Castro::theDMPC() )
	AddParticlesToRhs(crse_level,crse_level+nlevs-1,rhs);
#endif

}

void
Gravity::sanity_check (int level)
{
    // This is a sanity check on whether we are trying to fill multipole boundary conditiosn
    //  for grids at this level > 0 -- this case is not currently supported. 
    //  Here we shrink the domain at this level by 1 in any direction which is not symmetry or periodic, 
    //  then ask if the grids at this level are contained in the shrunken domain.  If not, then grids
    //  at this level touch the domain boundary and we must abort.
    if (level > 0  && !Geometry::isAllPeriodic()) 
    {
	const Geometry& geom = parent->Geom(level);
	Box shrunk_domain(geom.Domain());
	for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	    if (!Geometry::isPeriodic(dir)) 
	    {
		if (phys_bc->lo(dir) != Symmetry) 
		    shrunk_domain.growLo(dir,-1);
		if (phys_bc->hi(dir) != Symmetry) 
		    shrunk_domain.growHi(dir,-1);
	    }
	}
	BoxArray shrunk_domain_ba(shrunk_domain);
	if (!shrunk_domain_ba.contains(grids[level]))
	    BoxLib::Error("Oops -- don't know how to set boundary conditions for grids at this level that touch the domain boundary!");
    }
}
