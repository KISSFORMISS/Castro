subroutine PROBINIT (init,name,namlen,problo,probhi)

  use bl_types
  use prob_params_module, only: center
  use probdata_module
  use bl_error_module
  use eos_type_module
  use eos_module
  use network, only : nspec

  implicit none

  integer init, namlen
  integer name(namlen)
  double precision problo(2), probhi(2)

  integer untin,i

  namelist /fortin/ diff_coeff, T1, T2, rho0, t_0

  ! Build "probin" filename -- the name of file containing fortin namelist.

  integer, parameter :: maxlen = 256
  character probin*(maxlen)
  real (kind=dp_t) :: X(nspec)

  type (eos_t) :: eos_state

  if (namlen .gt. maxlen) call bl_error("probin file name too long")

  do i = 1, namlen
     probin(i:i) = char(name(i))
  end do
         
  ! Set namelist defaults
  T1 = 1.0_dp_t
  T2 = 2.0_dp_t
  rho0 = 1.0_dp_t
  t_0 = 0.001_dp_t
  diff_coeff = 1.0_dp_t

  ! set center, domain extrema
  center(1) = (problo(1)+probhi(1))/2.d0
  center(2) = (problo(2)+probhi(2))/2.d0

  ! Read namelists
  untin = 9
  open(untin,file=probin(1:namlen),form='formatted',status='old')
  read(untin,fortin)
  close(unit=untin)

  ! compute the conductivity for this diffusion coefficient
  X(:) = 0.d0
  X(1) = 1.d0

  eos_state%T = T1
  eos_state%rho = rho0
  eos_state%xn(:) = X(:)

  call eos(eos_input_rt, eos_state)

  ! diffusion coefficient is D = k/(rho c_v). we are doing an ideal
  ! gas, so c_v is constant, and we are taking rho = constant too
  thermal_conductivity = diff_coeff*rho0*eos_state%cv

end subroutine PROBINIT


! ::: -----------------------------------------------------------
! ::: This routine is called at problem setup time and is used
! ::: to initialize data on each grid.  
! ::: 
! ::: NOTE:  all arrays have one cell of ghost zones surrounding
! :::        the grid interior.  Values in these cells need not
! :::        be set here.
! ::: 
! ::: INPUTS/OUTPUTS:
! ::: 
! ::: level     => amr level of grid
! ::: time      => time at which to init data             
! ::: lo,hi     => index limits of grid interior (cell centered)
! ::: nstate    => number of state components.  You should know
! :::		   this already!
! ::: state     <=  Scalar array
! ::: delta     => cell size
! ::: xlo,xhi   => physical locations of lower left and upper
! :::              right hand corner of grid.  (does not include
! :::		   ghost region).
! ::: -----------------------------------------------------------
subroutine ca_initdata(level,time,lo,hi,nscal, &
                       state,state_l1,state_l2,state_h1,state_h2, &
                       delta,xlo,xhi)

  use probdata_module, only : T1, T2, diff_coeff, t_0, rho0
  use eos_module
  use network, only: nspec
  use meth_params_module, only : NVAR, URHO, UMX, UMY, UEDEN, UEINT, UFS, UTEMP
  use prob_params_module, only : problo, center
  
  implicit none

  integer :: level, nscal
  integer :: lo(2), hi(2)
  integer :: state_l1,state_l2,state_h1,state_h2
  double precision :: xlo(2), xhi(2), time, delta(2)
  double precision :: state(state_l1:state_h1,state_l2:state_h2,NVAR)

  double precision :: xc, yc
  double precision :: X(nspec), temp
  double precision :: dist2
  integer :: i,j

  type (eos_t) :: eos_state

  ! set the composition
  X(:) = 0.d0
  X(1) = 1.d0

  do j = lo(2), hi(2)
     yc = problo(2) + delta(2)*(dble(j) + HALF)

     do i = lo(1), hi(1)
        xc = problo(1) + delta(1)*(dble(i) + HALF)

        state(i,j,URHO) = rho0

        dist2 = (xc - center(1))**2 + (yc - center(2))**2

        temp = (T2 - T1)*exp(-0.25_dp_t*dist2/(diff_coeff*t_0) ) + T1
        state(i,j,UTEMP) = temp

        ! compute the internal energy and temperature
        eos_state%T = temp
        eos_state%rho = state(i,j,URHO)
        eos_state%xn(:) = X

        call eos(eos_input_rt, eos_state)

        state(i,j,UMX) = ZERO
        state(i,j,UMY) = ZERO

        state(i,j,UEDEN) = rho0*eos_state%e +  &
             0.5d0*(state(i,j,UMX)**2/state(i,j,URHO) + &
                    state(i,j,UMY)**2/state(i,j,URHO))

        state(i,j,UEINT) = rho0*eos_state%e

        state(i,j,UFS:UFS-1+nspec) = rho0*X(:)

     enddo
  enddo

end subroutine ca_initdata
