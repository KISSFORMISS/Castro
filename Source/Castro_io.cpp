#include <winstd.H>

#ifndef WIN32
#include <unistd.h>
#endif

#include <iomanip>
#include <iostream>
#include <string>
#include <ctime>

#include <Utility.H>
#include "Castro.H"
#include "Castro_F.H"
#include "Castro_io.H"
#include <ParmParse.H>

#ifdef RADIATION
#include "Radiation.H"
#endif

#ifdef PARTICLES
#include <Particles_F.H>
#endif

#ifdef GRAVITY
#include "Gravity.H"
#endif

#ifdef DIFFUSION
#include "Diffusion.H"
#endif

#ifdef LEVELSET
#include "LevelSet_F.H"
#endif

#ifdef _OPENMP
#include <omp.h>
#endif


#include "buildInfo.H"

using std::string;

namespace 
{
    int version = -1;
}

// I/O routines for Castro

void
Castro::restart (Amr&     papa,
                 istream& is,
                 bool     bReadSpecial)
{
    // Let's check Castro checkpoint version first; 
    // trying to read from checkpoint; if nonexisting, set it to 0.
    if (version == -1) {
   	if (ParallelDescriptor::IOProcessor()) {
   	    std::ifstream CastroHeaderFile;
   	    std::string FullPathCastroHeaderFile = papa.theRestartFile();
   	    FullPathCastroHeaderFile += "/CastroHeader";
   	    CastroHeaderFile.open(FullPathCastroHeaderFile.c_str(), std::ios::in);
   	    if (CastroHeaderFile.good()) {
		char foo[256];
		// first line: Checkpoint version: ?
		CastroHeaderFile.getline(foo, 256, ':');  
		CastroHeaderFile >> version;
   		CastroHeaderFile.close();
  	    } else {
   		version = 0;
   	    }
   	}
  	ParallelDescriptor::Bcast(&version, 1, ParallelDescriptor::IOProcessorNumber());
    }
 
    BL_ASSERT(version >= 0);
 
    // also need to mod checkPoint function to store the new version in a text file

    AmrLevel::restart(papa,is,bReadSpecial);

    if (version == 0) { // old checkpoint without PhiGrav_Type
#ifdef GRAVITY
      state[PhiGrav_Type].restart(desc_lst[PhiGrav_Type], state[Gravity_Type]);
#endif      
    }

    if (version < 3) { // old checkpoint without Source_Type
      state[Source_Type].restart(desc_lst[Source_Type], state[State_Type]);
    }

    // For versions < 2, we didn't store all three components
    // of the momenta in the checkpoint when doing 1D or 2D simulations.
    // So the state data that was read in will be a MultiFab with a
    // number of components that doesn't include the extra momenta,
    // which is incompatible with what we want. Our strategy is therefore
    // to create a new MultiFab with the right number of components, and
    // copy the data from the old MultiFab into the new one in the correct
    // slots. Then we'll swap pointers so that the new MultiFab is where
    // the new state data lives, and delete the old data as we no longer need it.
    
#if (BL_SPACEDIM < 3)    

    if (version < 2) {

      int ns = desc_lst[State_Type].nComp();
      int ng = desc_lst[State_Type].nExtra();
      MultiFab* new_data = new MultiFab(grids,ns,ng,Fab_allocate);
      MultiFab& chk_data = get_state_data(State_Type).newData();

#if (BL_SPACEDIM == 1)      
      
      // In 1D, we can copy everything below the y-momentum as normal,
      // and everything above the z-momentum as normal but shifted by
      // two components. The y- and z-momentum are zeroed out.

      for (int n = 0; n < ns; n++) {
	if (n < Ymom)
	  MultiFab::Copy(*new_data, chk_data, n,   n, 1, ng);
	else if (n == Ymom || n == Zmom)
	  new_data->setVal(0.0, n, 1, ng);
	else
	  MultiFab::Copy(*new_data, chk_data, n-2, n, 1, ng);
      }

#elif (BL_SPACEDIM == 2)
      
      // Strategy is the same in 2D but we only need to worry about
      // shifting by one component.

      for (int n = 0; n < ns; n++) {
	if (n < Zmom)
	  MultiFab::Copy(*new_data, chk_data, n,   n, 1, ng);
	else if (n == Zmom)
	  new_data->setVal(0.0, n, 1, ng);
	else
	  MultiFab::Copy(*new_data, chk_data, n-1, n, 1, ng);
      }

#endif

      // Now swap the pointers.

      get_state_data(State_Type).replaceNewData(new_data);

    }
 
#endif

#ifdef REACTIONS
    
    // Get data from the reactions header file.

    max_delta_e = 0.0;

    // Note that we want all grids on the domain to have this value,
    // so we have all processors read this in. We could do the same
    // with a broadcast from the IOProcessor but this avoids communication.
    
    std::ifstream ReactFile;
    std::string FullPathReactFile = parent->theRestartFile();
    FullPathReactFile += "/ReactHeader";
    ReactFile.open(FullPathReactFile.c_str(), std::ios::in);

    // Maximum change in internal energy in last timestep.
      
    ReactFile >> max_delta_e;

    ReactFile.close();

    // Set the energy change to the components of the
    // reactions MultiFab; it will get overwritten later
    // but will achieve our desired effect of being
    // utilized in the first timestep calculation.
    
    get_new_data(Reactions_Type).setVal(max_delta_e);

#endif
	
    buildMetrics();

    // get the elapsed CPU time to now;
    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      // get ellapsed CPU time
      std::ifstream CPUFile;
      std::string FullPathCPUFile = parent->theRestartFile();
      FullPathCPUFile += "/CPUtime";
      CPUFile.open(FullPathCPUFile.c_str(), std::ios::in);	
  
      CPUFile >> previousCPUTimeUsed;
      CPUFile.close();

      std::cout << "read CPU time: " << previousCPUTimeUsed << "\n";

    }


    if (level == 0)
      {
	// get problem-specific stuff -- note all processors do this,
	// eliminating the need for a broadcast
	std::string dir = parent->theRestartFile();

	char * dir_for_pass = new char[dir.size() + 1];
	std::copy(dir.begin(), dir.end(), dir_for_pass);
	dir_for_pass[dir.size()] = '\0';

	int len = dir.size();
      
	Array<int> int_dir_name(len);
	for (int j = 0; j < len; j++)
	  int_dir_name[j] = (int) dir_for_pass[j];

	BL_FORT_PROC_CALL(PROBLEM_RESTART, problem_restart)(int_dir_name.dataPtr(), &len);      

	delete [] dir_for_pass;

      }

    BL_ASSERT(flux_reg == 0);
    if (level > 0 && do_reflux)
        flux_reg = new FluxRegister(grids,crse_ratio,level,NUM_STATE);

#ifdef SGS
    BL_ASSERT(sgs_flux_reg == 0);
    if (level > 0 && do_reflux)
        sgs_flux_reg = new FluxRegister(grids,crse_ratio,level,NUM_STATE);
#endif

#ifdef RADIATION
    BL_ASSERT(rad_flux_reg == 0);
    if (Radiation::rad_hydro_combined && level > 0 && do_reflux)
      rad_flux_reg = new FluxRegister(grids,crse_ratio,level,Radiation::nGroups);
#endif

    const Real* dx  = geom.CellSize();

    if ( (grown_factor > 1) && (parent->maxLevel() < 1) )
    {
       std::cout << "grown_factor is " << grown_factor << std::endl;
       std::cout << "max_level is " << parent->maxLevel() << std::endl;
       BoxLib::Error("Must have max_level > 0 if doing special restart with grown_factor");
    }

    if (grown_factor > 1 && level == 0)
    {
       if (verbose && ParallelDescriptor::IOProcessor())
          std::cout << "Doing special restart with grown_factor " << grown_factor << std::endl;

       MultiFab& S_new = get_new_data(State_Type);
       Real cur_time   = state[State_Type].curTime();

       Box orig_domain;
       if (star_at_center == 0) {
          orig_domain = BoxLib::coarsen(geom.Domain(),grown_factor);
       } else if (star_at_center == 1) {

          Box domain(geom.Domain());
          int d,lo=0,hi=0;
          if (Geometry::IsRZ()) {
             if (grown_factor != 2) 
                BoxLib::Abort("Must have grown_factor = 2");

             d = 0;
             int dlen =  domain.size()[d];
             lo = 0;
             hi = dlen/2;
             orig_domain.setSmall(d,lo);
             orig_domain.setBig(d,hi);

             d = 1;
             dlen =  domain.size()[d];
             lo =   dlen/4    ;
             hi = 3*dlen/4 - 1;
             orig_domain.setSmall(d,lo);
             orig_domain.setBig(d,hi);

          } else {
             for (int d = 0; d < BL_SPACEDIM; d++) 
             {
                int dlen =  domain.size()[d];
                if (grown_factor == 2) {
                   lo =   dlen/4    ;
                   hi = 3*dlen/4 - 1;
                } else if (grown_factor == 3) {
                   lo =   (dlen)/3    ;
                   hi = 2*(dlen)/3 - 1;
                } else { 
                   BoxLib::Abort("Must have grown_factor = 2 or 3");
                }
                orig_domain.setSmall(d,lo);
                orig_domain.setBig(d,hi);
             }
          }
       } else {
          if (ParallelDescriptor::IOProcessor())
             std::cout << "... invalid value of star_at_center: " << star_at_center << std::endl;
          BoxLib::Abort();
       }

       int ns = NUM_STATE;

       for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
       {
           const Box& bx      = mfi.validbox();
           const int* lo      = bx.loVect();
           const int* hi      = bx.hiVect();

           if (! orig_domain.contains(bx)) {

#ifdef DIMENSION_AGNOSTIC
              BL_FORT_PROC_CALL(CA_INITDATA,ca_initdata)
                (level, cur_time, ARLIM_3D(lo), ARLIM_3D(hi), ns,
		 BL_TO_FORTRAN_3D(S_new[mfi]), ZFILL(dx),
		 ZFILL(geom.ProbLo()), ZFILL(geom.ProbHi()));
#else
	      BL_FORT_PROC_CALL(CA_INITDATA,ca_initdata)
		(level, cur_time, lo, hi, ns,
		 BL_TO_FORTRAN(S_new[mfi]), dx,
		 geom.ProbLo(), geom.ProbHi());
#endif

           }
       }
    }

    if (grown_factor > 1 && level == 1)
        getLevel(0).avgDown();

#if (BL_SPACEDIM > 1)
    if ( (level == 0) && (spherical_star == 1) ) {
       MultiFab& S_new = get_new_data(State_Type);
       int nc = S_new.nComp();
       int n1d = get_numpts();
       BL_FORT_PROC_CALL(ALLOCATE_OUTFLOW_DATA,allocate_outflow_data)(&n1d,&nc);
       int is_new = 1;
       make_radial_data(is_new);
    }
#endif

#ifdef GRAVITY
    if (do_grav && level == 0) {
       BL_ASSERT(gravity == 0);
       gravity = new Gravity(parent,parent->finestLevel(),&phys_bc,Density);
    }
#endif

#ifdef DIFFUSION
    if (level == 0) {
       BL_ASSERT(diffusion == 0);
       diffusion = new Diffusion(parent,&phys_bc);
    }
#endif

#ifdef RADIATION
    if (do_radiation) {
      if (radiation == 0) {
        // radiation is a static object, only alloc if not already there
        int rad_restart = 1; // disables quasi-steady initialization
        radiation = new Radiation(parent, this, rad_restart);
      }
      radiation->regrid(level, grids);
      radiation->restart(level, parent->theRestartFile(), is);
    }
#endif
}

void
Castro::set_state_in_checkpoint (Array<int>& state_in_checkpoint)
{ 
  for (int i=0; i<NUM_STATE_TYPE; ++i)
    state_in_checkpoint[i] = 1;

  for (int i=0; i<NUM_STATE_TYPE; ++i) {
#ifdef GRAVITY    
    if (version == 0 && i == PhiGrav_Type) {
      // We are reading an old checkpoint with no PhiGrav_Type
      state_in_checkpoint[i] = 0;
    }
#endif
    if (version < 3 && i == Source_Type) {
      // We are reading an old checkpoint with no Source_Type
      state_in_checkpoint[i] = 0;
    }
  }
}

void
Castro::checkPoint(const std::string& dir,
                   std::ostream&  os,
                   VisMF::How     how,
                   bool dump_old_default)
{
  AmrLevel::checkPoint(dir, os, how, dump_old);

#ifdef RADIATION
  if (do_radiation) {
    radiation->checkPoint(level, dir, os, how);
  }
#endif

#ifdef PARTICLES
  ParticleCheckPoint(dir);
#endif

  if (level == 0 && ParallelDescriptor::IOProcessor())
    {
	{
	    std::ofstream CastroHeaderFile;
	    std::string FullPathCastroHeaderFile = dir;
	    FullPathCastroHeaderFile += "/CastroHeader";
	    CastroHeaderFile.open(FullPathCastroHeaderFile.c_str(), std::ios::out);

	    CastroHeaderFile << "Checkpoint version: 3" << std::endl;
	    CastroHeaderFile.close();
	}

	{
	    // store ellapsed CPU time
	    std::ofstream CPUFile;
	    std::string FullPathCPUFile = dir;
	    FullPathCPUFile += "/CPUtime";
	    CPUFile.open(FullPathCPUFile.c_str(), std::ios::out);	
	    
	    CPUFile << std::setprecision(15) << getCPUTime();
	    CPUFile.close();
	}

	{
	    // store any problem-specific stuff
	    char * dir_for_pass = new char[dir.size() + 1];
	    std::copy(dir.begin(), dir.end(), dir_for_pass);
	    dir_for_pass[dir.size()] = '\0';
	    
	    int len = dir.size();
	    
	    Array<int> int_dir_name(len);
	    for (int j = 0; j < len; j++)
		int_dir_name[j] = (int) dir_for_pass[j];
	    
	    BL_FORT_PROC_CALL(PROBLEM_CHECKPOINT, problem_checkpoint)(int_dir_name.dataPtr(), &len);      
	    
	    delete [] dir_for_pass;
	}
    }

#ifdef REACTIONS		

    // Write out maximum value of delta_e from reactions data.
    // First, determine the maximum value of delta_e on all levels.

    if (level == 0)
      max_delta_e = 0.0;

    // Determine the maximum absolute value of the delta_e component of the reactions MF.
    // Note that there are NumSpec components starting from 0 corresponding to the species changes.
	  
    max_delta_e = std::max(max_delta_e, get_new_data(Reactions_Type).norm0(NumSpec));

    ParallelDescriptor::ReduceRealMax(max_delta_e);

    // Now, write out to the header if we're on the finest level and therefore have checked all entries for delta_e.
    
    if (level == parent->finestLevel() && ParallelDescriptor::IOProcessor()) {
	  
      std::ofstream ReactHeaderFile;
      std::string FullPathReactHeaderFile = dir;
      FullPathReactHeaderFile += "/ReactHeader";
      ReactHeaderFile.open(FullPathReactHeaderFile.c_str(), std::ios::out);

      ReactHeaderFile << std::scientific << std::setprecision(16) << max_delta_e;
      ReactHeaderFile.close();

    }

#endif	      
  
}

std::string
Castro::thePlotFileType () const
{
    //
    // Increment this whenever the writePlotFile() format changes.
    //
    static const std::string the_plot_file_type("HyperCLaw-V1.1");

    return the_plot_file_type;
}

void
Castro::setPlotVariables ()
{
  AmrLevel::setPlotVariables();

#ifdef RADIATION
  if (Radiation::nNeutrinoSpecies > 0 &&
      Radiation::plot_neutrino_group_energies_total == 0) {
    char rad_name[10];
    for (int j = 0; j < Radiation::nNeutrinoSpecies; j++) {
      for (int i = 0; i < Radiation::nNeutrinoGroups[j]; i++) {
        sprintf(rad_name, "rads%dg%d", j, i);
        parent->deleteStatePlotVar(rad_name);
      }
    }
  }
#endif

  // Don't add the Source_Type data to the plotfile, we only
  // want to store it in the checkpoints.

  for (int i = 0; i < desc_lst[Source_Type].nComp(); i++)
    parent->deleteStatePlotVar(desc_lst[Source_Type].name(i));
			       
  ParmParse pp("castro");

  bool plot_X;

  if (pp.query("plot_X",plot_X))
  {
      if (plot_X)
      {
          //
	  // Get the number of species from the network model.
          //
	  BL_FORT_PROC_CALL(GET_NUM_SPEC, get_num_spec)(&NumSpec);
          //
	  // Get the species names from the network model.
          //
	  for (int i = 0; i < NumSpec; i++)
          {
              int len = 20;
              Array<int> int_spec_names(len);
              //
              // This call return the actual length of each string in "len"
              //
              BL_FORT_PROC_CALL(GET_SPEC_NAMES, get_spec_names)
                  (int_spec_names.dataPtr(),&i,&len);
              char* spec_name = new char[len+1];
              for (int j = 0; j < len; j++) 
                  spec_name[j] = int_spec_names[j];
              spec_name[len] = '\0';
	      string spec_string = "X(";
              spec_string += spec_name;
              spec_string += ')';
	      parent->addDerivePlotVar(spec_string);
              delete [] spec_name;
	  }
      }
  }
}

void
Castro::writePlotFile (const std::string& dir,
                       ostream&       os,
                       VisMF::How     how)
{
    int i, n;
    //
    // The list of indices of State to write to plotfile.
    // first component of pair is state_type,
    // second component of pair is component # within the state_type
    //
    std::vector<std::pair<int,int> > plot_var_map;
    for (int typ = 0; typ < desc_lst.size(); typ++)
        for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
            if (parent->isStatePlotVar(desc_lst[typ].name(comp)) &&
                desc_lst[typ].getType() == IndexType::TheCellType())
                plot_var_map.push_back(std::pair<int,int>(typ,comp));

    int num_derive = 0;
    std::list<std::string> derive_names;
    const std::list<DeriveRec>& dlist = derive_lst.dlist();

    for (std::list<DeriveRec>::const_iterator it = dlist.begin();
	 it != dlist.end();
	 ++it)
    {
        if (parent->isDerivePlotVar(it->name()))
        {
#ifdef PARTICLES
            if (it->name() == "particle_count" ||
                it->name() == "total_particle_count" ||
                it->name() == "particle_mass_density" ||
                it->name() == "total_density")
            {
                if (Castro::theDMPC())
                {
                    derive_names.push_back(it->name());
                    num_derive++;
                }
            } else 
#endif
            derive_names.push_back(it->name());
            num_derive++;
	} 
    }

    int n_data_items = plot_var_map.size() + num_derive;

#ifdef RADIATION
    if (Radiation::nplotvar > 0) n_data_items += Radiation::nplotvar;
#endif

    Real cur_time = state[State_Type].curTime();

    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
        //
        // The first thing we write out is the plotfile type.
        //
        os << thePlotFileType() << '\n';

        if (n_data_items == 0)
            BoxLib::Error("Must specify at least one valid data item to plot");

        os << n_data_items << '\n';

	//
	// Names of variables -- first state, then derived
	//
	for (i =0; i < plot_var_map.size(); i++)
        {
	    int typ = plot_var_map[i].first;
	    int comp = plot_var_map[i].second;
	    os << desc_lst[typ].name(comp) << '\n';
        }

	for ( std::list<std::string>::iterator it = derive_names.begin();
	      it != derive_names.end(); ++it)
        {
	    const DeriveRec* rec = derive_lst.get(*it);
            os << rec->variableName(0) << '\n';
        }

#ifdef RADIATION
	for (i=0; i<Radiation::nplotvar; ++i)
	    os << Radiation::plotvar_names[i] << '\n';
#endif

        os << BL_SPACEDIM << '\n';
        os << parent->cumTime() << '\n';
        int f_lev = parent->finestLevel();
        os << f_lev << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << Geometry::ProbLo(i) << ' ';
        os << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << Geometry::ProbHi(i) << ' ';
        os << '\n';
        for (i = 0; i < f_lev; i++)
            os << parent->refRatio(i)[0] << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->Geom(i).Domain() << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->levelSteps(i) << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
        {
            for (int k = 0; k < BL_SPACEDIM; k++)
                os << parent->Geom(i).CellSize()[k] << ' ';
            os << '\n';
        }
        os << (int) Geometry::Coord() << '\n';
        os << "0\n"; // Write bndry data.

#ifdef RADIATION
	if (do_radiation && Radiation::do_multigroup) {
	  std::ofstream groupfile;
	  std::string FullPathGroupFile = dir;
	  FullPathGroupFile += "/RadiationGroups";
	  groupfile.open(FullPathGroupFile.c_str(), std::ios::out);

	  radiation->write_groups(groupfile);

	  groupfile.close();
	}
#endif

        // job_info file with details about the run
	std::ofstream jobInfoFile;
	std::string FullPathJobInfoFile = dir;
	FullPathJobInfoFile += "/job_info";
	jobInfoFile.open(FullPathJobInfoFile.c_str(), std::ios::out);	

	std::string PrettyLine = "===============================================================================\n";
	std::string OtherLine = "--------------------------------------------------------------------------------\n";
	std::string SkipSpace = "        ";


	// job information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Job Information\n";
	jobInfoFile << PrettyLine;
	
	jobInfoFile << "job name: " << job_name << "\n\n";
	jobInfoFile << "inputs file: " << inputs_name << "\n\n";

	jobInfoFile << "number of MPI processes: " << ParallelDescriptor::NProcs() << "\n";
#ifdef _OPENMP
	jobInfoFile << "number of threads:       " << omp_get_max_threads() << "\n";
#endif

	jobInfoFile << "\n";
	jobInfoFile << "CPU time used since start of simulation (CPU-hours): " <<
	  getCPUTime()/3600.0;

	jobInfoFile << "\n\n";

        // plotfile information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Plotfile Information\n";
	jobInfoFile << PrettyLine;

	time_t now = time(0);

	// Convert now to tm struct for local timezone
	tm* localtm = localtime(&now);
	jobInfoFile   << "output data / time: " << asctime(localtm);

	char currentDir[FILENAME_MAX];
	if (getcwd(currentDir, FILENAME_MAX)) {
	  jobInfoFile << "output dir:         " << currentDir << "\n";
	}

	jobInfoFile << "\n\n";


        // build information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Build Information\n";
	jobInfoFile << PrettyLine;

	jobInfoFile << "build date:    " << buildInfoGetBuildDate() << "\n";
	jobInfoFile << "build machine: " << buildInfoGetBuildMachine() << "\n";
	jobInfoFile << "build dir:     " << buildInfoGetBuildDir() << "\n";
	jobInfoFile << "BoxLib dir:    " << buildInfoGetBoxlibDir() << "\n";

	jobInfoFile << "\n";
	
	jobInfoFile << "COMP:  " << buildInfoGetComp() << "\n";
	jobInfoFile << "FCOMP: " << buildInfoGetFcomp() << "\n";

	jobInfoFile << "\n";

	jobInfoFile << "EOS:     " << buildInfoGetAux(1) << "\n";
	jobInfoFile << "network: " << buildInfoGetAux(2) << "\n";

	jobInfoFile << "\n";

	const char* githash1 = buildInfoGetGitHash(1);
	const char* githash2 = buildInfoGetGitHash(2);
	const char* githash3 = buildInfoGetGitHash(3);
	if (strlen(githash1) > 0) {
	  jobInfoFile << "Castro   git hash: " << githash1 << "\n";
	}
	if (strlen(githash2) > 0) {
	  jobInfoFile << "BoxLib   git hash: " << githash2 << "\n";
	}
	if (strlen(githash3) > 0) {	
	  jobInfoFile << "AstroDev git hash: " << githash3 << "\n";
	}

	const char* buildgithash = buildInfoGetBuildGitHash();
	const char* buildgitname = buildInfoGetBuildGitName();
	if (strlen(buildgithash) > 0){
	  jobInfoFile << buildgitname << " git hash: " << buildgithash << "\n";
	}
	
	jobInfoFile << "\n\n";


	// grid information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Grid Information\n";
	jobInfoFile << PrettyLine;

	for (i = 0; i <= f_lev; i++)
	  {
	    jobInfoFile << " level: " << i << "\n";
	    jobInfoFile << "   number of boxes = " << parent->numGrids(i) << "\n";
	    jobInfoFile << "   maximum zones   = ";
	    for (n = 0; n < BL_SPACEDIM; n++)
	      {
		jobInfoFile << parent->Geom(i).Domain().length(n) << " ";
		//jobInfoFile << parent->Geom(i).ProbHi(n) << " ";
	      }
	    jobInfoFile << "\n\n";
	  }

	jobInfoFile << " Boundary conditions\n";
	Array<int> lo_bc_out(BL_SPACEDIM), hi_bc_out(BL_SPACEDIM);
	ParmParse pp("castro");
	pp.getarr("lo_bc",lo_bc_out,0,BL_SPACEDIM);
	pp.getarr("hi_bc",hi_bc_out,0,BL_SPACEDIM);


	// these names correspond to the integer flags setup in the 
	// Castro_setup.cpp
	const char* names_bc[] =
	  { "interior", "inflow", "outflow", 
	    "symmetry", "slipwall", "noslipwall" };


	jobInfoFile << "   -x: " << names_bc[lo_bc_out[0]] << "\n";
	jobInfoFile << "   +x: " << names_bc[hi_bc_out[0]] << "\n";
	if (BL_SPACEDIM >= 2) {
	  jobInfoFile << "   -y: " << names_bc[lo_bc_out[1]] << "\n";
	  jobInfoFile << "   +y: " << names_bc[hi_bc_out[1]] << "\n";
	}
	if (BL_SPACEDIM == 3) {
	  jobInfoFile << "   -z: " << names_bc[lo_bc_out[2]] << "\n";
	  jobInfoFile << "   +z: " << names_bc[hi_bc_out[2]] << "\n";
	}

	jobInfoFile << "\n\n";


	// species info
	Real Aion = 0.0;
	Real Zion = 0.0;

	int mlen = 20;

	jobInfoFile << PrettyLine;
	jobInfoFile << " Species Information\n";
	jobInfoFile << PrettyLine;
	
	jobInfoFile << 
	      std::setw(6) << "index" << SkipSpace << 
	      std::setw(mlen+1) << "name" << SkipSpace <<
	      std::setw(7) << "A" << SkipSpace <<
	      std::setw(7) << "Z" << "\n";
	jobInfoFile << OtherLine;

	for (int i = 0; i < NumSpec; i++)
          {

	    int len = mlen;
	    Array<int> int_spec_names(len);
	    //
	    // This call return the actual length of each string in "len"
	    //
	    BL_FORT_PROC_CALL(GET_SPEC_NAMES, get_spec_names)
	      (int_spec_names.dataPtr(),&i,&len);
	    char* spec_name = new char[len+1];
	    for (int j = 0; j < len; j++) 
	      spec_name[j] = int_spec_names[j];
	    spec_name[len] = '\0';

	    // get A and Z
	    BL_FORT_PROC_CALL(GET_SPEC_AZ, get_spec_az)
	      (&i, &Aion, &Zion);

	    jobInfoFile << 
	      std::setw(6) << i << SkipSpace << 
	      std::setw(mlen+1) << std::setfill(' ') << spec_name << SkipSpace <<
	      std::setw(7) << Aion << SkipSpace <<
	      std::setw(7) << Zion << "\n";
	    delete [] spec_name;
	  }
	jobInfoFile << "\n\n";


	// runtime parameters
	jobInfoFile << PrettyLine;
	jobInfoFile << " Inputs File Parameters\n";
	jobInfoFile << PrettyLine;
	
	ParmParse::dumpTable(jobInfoFile, true);

	jobInfoFile.close();
	

    }
    // Build the directory to hold the MultiFab at this level.
    // The name is relative to the directory containing the Header file.
    //
    static const std::string BaseName = "/Cell";
    char buf[64];
    sprintf(buf, "Level_%d", level);
    std::string Level = buf;
    //
    // Now for the full pathname of that directory.
    //
    std::string FullPath = dir;
    if (!FullPath.empty() && FullPath[FullPath.size()-1] != '/')
        FullPath += '/';
    FullPath += Level;
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!BoxLib::UtilCreateDirectory(FullPath, 0755))
            BoxLib::CreateDirectoryFailed(FullPath);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    if (ParallelDescriptor::IOProcessor())
    {
        os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
        os << parent->levelSteps(level) << '\n';

        for (i = 0; i < grids.size(); ++i)
        {
            RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
            for (n = 0; n < BL_SPACEDIM; n++)
                os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
        //
        // The full relative pathname of the MultiFabs at this level.
        // The name is relative to the Header file containing this name.
        // It's the name that gets written into the Header.
        //
        if (n_data_items > 0)
        {
            std::string PathNameInHeader = Level;
            PathNameInHeader += BaseName;
            os << PathNameInHeader << '\n';
        }
    }
    //
    // We combine all of the multifabs -- state, derived, etc -- into one
    // multifab -- plotMF.
    // NOTE: we are assuming that each state variable has one component,
    // but a derived variable is allowed to have multiple components.
    int       cnt   = 0;
    const int nGrow = 0;
    MultiFab  plotMF(grids,n_data_items,nGrow);
    MultiFab* this_dat = 0;
    //
    // Cull data from state variables -- use no ghost cells.
    //
    for (i = 0; i < plot_var_map.size(); i++)
    {
	int typ  = plot_var_map[i].first;
	int comp = plot_var_map[i].second;
	this_dat = &state[typ].newData();
	MultiFab::Copy(plotMF,*this_dat,comp,cnt,1,nGrow);
	cnt++;
    }
    //
    // Cull data from derived variables.
    // 
    if (derive_names.size() > 0)
    {
	for (std::list<std::string>::iterator it = derive_names.begin();
	     it != derive_names.end(); ++it) 
	{
	    MultiFab* derive_dat = derive(*it,cur_time,nGrow);
	    MultiFab::Copy(plotMF,*derive_dat,0,cnt,1,nGrow);
	    delete derive_dat;
	    cnt++;
	}
    }

#ifdef RADIATION
    if (Radiation::nplotvar > 0) {
	MultiFab::Copy(plotMF,radiation->plotvar[level],0,cnt,Radiation::nplotvar,0);
	cnt += Radiation::nplotvar;
    }
#endif

    //
    // Use the Full pathname when naming the MultiFab.
    //
    std::string TheFullPath = FullPath;
    TheFullPath += BaseName;
    VisMF::Write(plotMF,TheFullPath,how,true);

#ifdef PARTICLES
    //
    // Write the particles in a plotfile directory 
    // Particles are only written if particles.write_in_plotfile = 1 in inputs file.
    //
    ParticlePlotFile(dir);
#endif
}
