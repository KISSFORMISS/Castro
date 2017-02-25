#include "Castro.H"
#include "Castro_F.H"

#ifdef SELF_GRAVITY
#include "Gravity.H"

void
Castro::construct_old_gravity(int amr_iteration, int amr_ncycle, int sub_iteration, int sub_ncycle, Real time)
{
    MultiFab& grav_old = get_old_data(Gravity_Type);
    MultiFab& phi_old = get_old_data(PhiGrav_Type);

    // Always set phi to zero initially since some gravity modes
    // don't use it and we want to have valid data.

    if (gravity->get_gravity_type() != "PoissonGrav")
	phi_old.setVal(0.0);

    if (!do_grav) {

	grav_old.setVal(0.0);

	return;

    }

    // Do level solve at beginning of time step in order to compute the
    // difference between the multilevel and the single level solutions.

    if (gravity->get_gravity_type() == "PoissonGrav")
    {

	// Create a copy of the current (composite) data on this level.

	MultiFab comp_phi;
	PArray<MultiFab> comp_gphi(BL_SPACEDIM, PArrayManage);

        if (gravity->NoComposite() != 1 && gravity->DoCompositeCorrection() && level < parent->finestLevel()) {

	    comp_phi.define(phi_old.boxArray(), phi_old.nComp(), phi_old.nGrow(), Fab_allocate);
	    MultiFab::Copy(comp_phi, phi_old, 0, 0, phi_old.nComp(), phi_old.nGrow());

	    for (int n = 0; n < BL_SPACEDIM; ++n) {
		comp_gphi.set(n, new MultiFab(getEdgeBoxArray(n), 1, 0));
		comp_gphi[n].copy(gravity->get_grad_phi_prev(level)[n], 0, 0, 1);
	    }

	}

	if (verbose && ParallelDescriptor::IOProcessor()) {
	    std::cout << " " << '\n';
	    std::cout << "... old-time level solve at level " << level << '\n';
	}

	int is_new = 0;

	// If we are doing composite solves, then this is a placeholder solve
	// to get the difference between the composite and level solutions. If
	// we are only doing level solves, then this is the main result.

	gravity->solve_for_phi(level,
			       phi_old,
			       gravity->get_grad_phi_prev(level),
			       is_new);

        if (gravity->NoComposite() != 1 && gravity->DoCompositeCorrection() && level < parent->finestLevel()) {

	    // Subtract the level solve from the composite solution.

	    gravity->create_comp_minus_level_grad_phi(level,
						      comp_phi,
						      comp_gphi,
						      comp_minus_level_phi,
						      comp_minus_level_grad_phi);

	    // Copy the composite data back. This way the forcing
	    // uses the most accurate data we have.

	    MultiFab::Copy(phi_old, comp_phi, 0, 0, phi_old.nComp(), phi_old.nGrow());

	    for (int n = 0; n < BL_SPACEDIM; ++n)
		gravity->get_grad_phi_prev(level)[n].copy(comp_gphi[n], 0, 0, 1);

        }

	if (gravity->test_results_of_solves() == 1) {

	    if (verbose && ParallelDescriptor::IOProcessor()) {
		std::cout << " " << '\n';
		std::cout << "... testing grad_phi_curr after doing single level solve " << '\n';
	    }

	    gravity->test_level_grad_phi_prev(level);

	}
 
    }

    // Define the old gravity vector.

    gravity->get_old_grav_vector(level, grav_old, time);

}

void
Castro::construct_new_gravity(int amr_iteration, int amr_ncycle, int sub_iteration, int sub_ncycle, Real time)
{
    MultiFab& grav_new = get_new_data(Gravity_Type);
    MultiFab& phi_new = get_new_data(PhiGrav_Type);

    // Always set phi to zero initially since some gravity modes
    // don't use it and we want to have valid data.

    if (gravity->get_gravity_type() != "PoissonGrav")
	phi_new.setVal(0.0);

    if (!do_grav) {

	grav_new.setVal(0.0);

	return;

    }

    // If we're doing Poisson gravity, do the new-time level solve here.

    if (gravity->get_gravity_type() == "PoissonGrav")
    {

	// Use the "old" phi from the current time step as a guess for this solve.

	MultiFab& phi_old = get_old_data(PhiGrav_Type);

	MultiFab::Copy(phi_new, phi_old, 0, 0, 1, phi_new.nGrow());

	// Subtract off the (composite - level) contribution for the purposes
	// of the level solve. We'll add it back later.

	if (gravity->NoComposite() != 1 && gravity->DoCompositeCorrection() && level < parent->finestLevel())
	    phi_new.minus(comp_minus_level_phi, 0, 1, 0);

	if (verbose && ParallelDescriptor::IOProcessor()) {
	    std::cout << " " << '\n';
	    std::cout << "... new-time level solve at level " << level << '\n';
	}

	int is_new = 1;

	gravity->solve_for_phi(level,
			       phi_new,
			       gravity->get_grad_phi_curr(level),
			       is_new);

	if (gravity->NoComposite() != 1 && gravity->DoCompositeCorrection() == 1 && level < parent->finestLevel()) {

	    if (gravity->test_results_of_solves() == 1) {

		if (verbose && ParallelDescriptor::IOProcessor()) {
		    std::cout << " " << '\n';
		    std::cout << "... testing grad_phi_curr before adding comp_minus_level_grad_phi " << '\n';
		}

		gravity->test_level_grad_phi_curr(level);

	    }

	    // Add back the (composite - level) contribution. This ensures that
	    // if we are not doing a sync solve, then we still get the difference
	    // between the composite and level solves added to the force we
	    // calculate, so it is slightly more accurate than it would have been.

	    phi_new.plus(comp_minus_level_phi, 0, 1, 0);

	    for (int n = 0; n < BL_SPACEDIM; ++n)
		gravity->get_grad_phi_curr(level)[n].plus(comp_minus_level_grad_phi[n], 0, 1, 0);

	    if (gravity->test_results_of_solves() == 1) {

		if (verbose && ParallelDescriptor::IOProcessor()) {
		    std::cout << " " << '\n';
		    std::cout << "... testing grad_phi_curr after adding comp_minus_level_grad_phi " << '\n';
		}

		gravity->test_level_grad_phi_curr(level);

	    }

	}

    }

    // Define new gravity vector.

    gravity->get_new_grav_vector(level, grav_new, time);

    if (gravity->get_gravity_type() == "PoissonGrav") {

	if (gravity->NoComposite() != 1 && gravity->DoCompositeCorrection() == 1 && level < parent->finestLevel()) {

	    // Now that we have calculated the force, if we are going to do a sync
	    // solve then subtract off the (composite - level) contribution, as it
	    // interferes with the sync solve.

	    if (gravity->NoSync() == 0) {

		phi_new.minus(comp_minus_level_phi, 0, 1, 0);

		for (int n = 0; n < BL_SPACEDIM; ++n)
		    gravity->get_grad_phi_curr(level)[n].minus(comp_minus_level_grad_phi[n], 0, 1, 0);

	    }

	    // In any event we can now clear this memory, as we no longer need it.

	    comp_minus_level_phi.clear();
	    comp_minus_level_grad_phi.clear();

	}

    }

    // Calculate the flux of gravitational energy.

    fill_gfluxes();

}
#endif

void Castro::fill_gfluxes()
{

#ifdef SELF_GRAVITY
    MultiFab& grav_old = get_old_data(Gravity_Type);
    MultiFab& grav_new = get_new_data(Gravity_Type);
#endif

    // Set up gravitational energy fluxes.

    gfluxes.clear();

    for (int dir = 0; dir < BL_SPACEDIM; ++dir) {
	gfluxes.set(dir, new MultiFab(getEdgeBoxArray(dir), 1, 0));
	gfluxes[dir].setVal(0.0);
    }

    for (int dir = BL_SPACEDIM; dir < 3; ++dir) {
	gfluxes.set(dir, new MultiFab(grids, 1, 0));
	gfluxes[dir].setVal(0.0);
    }

    // We don't need to do this for the non-conservative gravity options.

    if (grav_source_type != 4) return;

    const Real* dx = geom.CellSize();
    const int* domlo = geom.Domain().loVect();
    const int* domhi = geom.Domain().hiVect();

#ifdef SELF_GRAVITY
    // Temporarily allocate edge-centered gravity arrays
    // that we will send to Fortran.

    PArray<MultiFab> grad_phi_prev(3);
    PArray<MultiFab> grad_phi_curr(3);

    for (int n = 0; n < BL_SPACEDIM; ++n) {
	grad_phi_prev.set(n, new MultiFab(getEdgeBoxArray(n), 1, 0));
	grad_phi_prev[n].setVal(0.0);

	grad_phi_curr.set(n, new MultiFab(getEdgeBoxArray(n), 1, 0));
	grad_phi_curr[n].setVal(0.0);
    }

    // For non-simulated dimensions, we'll do the same thing
    // we do for the hydrodynamic fluxes array, which is to
    // create a cell-centered array. This is done regardless
    // of gravity type.

    for (int n = BL_SPACEDIM; n < 3; ++n) {
	grad_phi_prev.set(n, new MultiFab(grids, 1, 0));
	grad_phi_prev[n].setVal(0.0);

	grad_phi_curr.set(n, new MultiFab(grids, 1, 0));
	grad_phi_curr[n].setVal(0.0);
    }

    // Now if we actually have Poisson gravity, copy the data over.

    if (gravity->get_gravity_type() == "PoissonGrav") {

	for (int n = 0; n < BL_SPACEDIM; ++n) {
	    MultiFab::Copy(grad_phi_prev[n], gravity->get_grad_phi_prev(level)[n], 0, 0, 1, 0);
	    MultiFab::Copy(grad_phi_curr[n], gravity->get_grad_phi_curr(level)[n], 0, 0, 1, 0);
	}

    }
#endif

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
	for (MFIter mfi(get_new_data(State_Type), true); mfi.isValid(); ++mfi)
	{

	    const Box& bx = mfi.tilebox();

	    ca_gflux(ARLIM_3D(bx.loVect()), ARLIM_3D(bx.hiVect()),
	             ARLIM_3D(domlo), ARLIM_3D(domhi),
#ifdef SELF_GRAVITY
		     BL_TO_FORTRAN_3D(grav_old[mfi]),
		     BL_TO_FORTRAN_3D(grav_new[mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_prev[0][mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_prev[1][mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_prev[2][mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_curr[0][mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_curr[1][mfi]),
		     BL_TO_FORTRAN_3D(grad_phi_curr[2][mfi]),
#endif
		     BL_TO_FORTRAN_3D(fluxes[0][mfi]),
		     BL_TO_FORTRAN_3D(fluxes[1][mfi]),
		     BL_TO_FORTRAN_3D(fluxes[2][mfi]),
		     BL_TO_FORTRAN_3D(gfluxes[0][mfi]),
		     BL_TO_FORTRAN_3D(gfluxes[1][mfi]),
		     BL_TO_FORTRAN_3D(gfluxes[2][mfi]),
		     ZFILL(dx));

	}

    }

#ifdef SELF_GRAVITY
    grad_phi_prev.clear();
    grad_phi_curr.clear();
#endif

}

void Castro::construct_old_gravity_source(Real time, Real dt)
{

#ifdef SELF_GRAVITY
    MultiFab& grav_old = get_old_data(Gravity_Type);
#endif

    old_sources[grav_src].setVal(0.0);

    if (!do_grav) return;

    // Gravitational source term for the time-level n data.

    const Real* dx = geom.CellSize();
    const int* domlo = geom.Domain().loVect();
    const int* domhi = geom.Domain().hiVect();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(Sborder,true); mfi.isValid(); ++mfi)
    {
	const Box& bx = mfi.growntilebox();

	ca_gsrc(ARLIM_3D(bx.loVect()), ARLIM_3D(bx.hiVect()),
		ARLIM_3D(domlo), ARLIM_3D(domhi),
		BL_TO_FORTRAN_3D(Sborder[mfi]),
#ifdef SELF_GRAVITY
		BL_TO_FORTRAN_3D(grav_old[mfi]),
#endif
		BL_TO_FORTRAN_3D(old_sources[grav_src][mfi]),
		ZFILL(dx),dt,&time);

    }

}

void Castro::construct_new_gravity_source(Real time, Real dt)
{
    MultiFab& S_old = get_old_data(State_Type);
    MultiFab& S_new = get_new_data(State_Type);

#ifdef SELF_GRAVITY
    MultiFab& grav_old = get_old_data(Gravity_Type);
    MultiFab& grav_new = get_new_data(Gravity_Type);
#endif

    new_sources[grav_src].setVal(0.0);

    if (!do_grav) return;

    const Real *dx = geom.CellSize();
    const int* domlo = geom.Domain().loVect();
    const int* domhi = geom.Domain().hiVect();

#ifndef SELF_GRAVITY
    fill_gfluxes();
#endif

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
	for (MFIter mfi(S_new,true); mfi.isValid(); ++mfi)
	{
	    const Box& bx = mfi.tilebox();

	    ca_corrgsrc(ARLIM_3D(bx.loVect()), ARLIM_3D(bx.hiVect()),
			ARLIM_3D(domlo), ARLIM_3D(domhi),
			BL_TO_FORTRAN_3D(S_old[mfi]),
			BL_TO_FORTRAN_3D(S_new[mfi]),
#ifdef SELF_GRAVITY
			BL_TO_FORTRAN_3D(grav_old[mfi]),
			BL_TO_FORTRAN_3D(grav_new[mfi]),
#endif
			BL_TO_FORTRAN_3D(volume[mfi]),
			BL_TO_FORTRAN_3D(gfluxes[0][mfi]),
			BL_TO_FORTRAN_3D(gfluxes[1][mfi]),
			BL_TO_FORTRAN_3D(gfluxes[2][mfi]),
			BL_TO_FORTRAN_3D(new_sources[grav_src][mfi]),
			ZFILL(dx),dt,&time);

	}
    }

    // We no longer need the flux data.

    gfluxes.clear();

}
