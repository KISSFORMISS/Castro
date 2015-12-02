subroutine PROBINIT (init,name,namlen,problo,probhi)

  use bl_error_module
  use probdata_module
  use eos_module
  use network, only : nspec
  use meth_params_module, only : small_temp
  use prob_params_module, only : center

  implicit none 

  integer :: init,namlen,untin,i,k
  integer :: name(namlen)

  double precision :: center_x, center_y, center_z
  double precision :: problo(3), probhi(3)

  type (eos_t) :: eos_state

  namelist /fortin/ &
       rho_0, r_0, r_old, p_0, rho_ambient, smooth_delta, &
       center_x, center_y, center_z

  ! Build "probin" filename -- the name of file containing fortin namelist.
  integer, parameter :: maxlen = 127
  character :: probin*(maxlen)

  if (namlen .gt. maxlen) call bl_error("probin file name too long")

  do i = 1, namlen
     probin(i:i) = char(name(i))
  end do
         
  ! set namelist defaults

  is_3d_fullstar = .false.

  rho_0 = 1.d9
  r_0 = 6.5d8
  r_old = r_0
  p_0 = 1.d10
  rho_ambient = 1.d0
  smooth_delta = 1.d-5

  ! Read namelists in probin file
  untin = 9
  open(untin,file=probin(1:namlen),form='formatted',status='old')
  read(untin,fortin)
  close(unit=untin)

  r_old_s = r_old

  ! in 3-d, we center the sphere at (center_x, center_y, center_z)
  center(1) = center_x
  center(2) = center_y
  center(3) = center_z

  xmin = problo(1)
  xmax = probhi(1)

  ymin = problo(2)
  ymax = probhi(2)

  zmin = problo(3)
  zmax = probhi(3)

  ! set the composition to be uniform
  allocate(X_0(nspec))
  
  X_0(:) = 0.0
  X_0(1) = 1.0

  ! get the ambient temperature and sphere temperature, T_0

  eos_state % rho = rho_0
  eos_state % p   = p_0
  eos_state % xn  = x_0
  eos_state % T   = small_temp ! Initial guess for the EOS

  call eos(eos_input_rp, eos_state)

  T_0 = eos_state % T
  
  eos_state % rho = rho_ambient
  
  call eos(eos_input_rp, eos_state)

  T_ambient = eos_state % T

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
                       state,state_l1,state_l2,state_l3,state_h1,state_h2,state_h3, &
                       delta,xlo,xhi)

  use probdata_module
  use eos_module
  use network, only : nspec
  use interpolate_module
  use meth_params_module, only : NVAR, URHO, UMX, UMY, UMZ, UTEMP, UEDEN, UEINT, UFS, small_temp
  use prob_params_module, only : center

  implicit none

  integer          :: level, nscal
  integer          :: lo(3), hi(3)
  integer          :: state_l1,state_l2,state_l3,state_h1,state_h2,state_h3
  double precision :: state(state_l1:state_h1,state_l2:state_h2,state_l3:state_h3,NVAR)
  double precision :: time, delta(3)
  double precision :: xlo(3), xhi(3)
  
  double precision :: xl,yl,zl,xx,yy,zz,dist,pres,eint,temp,avg_rho,rho_n,volinv
  double precision :: dx_sub,dy_sub,dz_sub
  integer          :: i,j,k,ii,jj,kk,n

  type (eos_t) :: eos_state

  integer, parameter :: nsub = 5

  volinv = 1.d0/dble(nsub*nsub*nsub)

  dx_sub = delta(1)/dble(nsub)
  dy_sub = delta(2)/dble(nsub)
  dz_sub = delta(3)/dble(nsub)

  do k = lo(3), hi(3)
     zl = zmin + dble(k) * delta(3)

     do j = lo(2), hi(2)
        yl = ymin + dble(j) * delta(2)

        do i = lo(1), hi(1)
           xl = xmin + dble(i) * delta(1)

           avg_rho = 0.d0
           
           do kk = 0, nsub-1
              zz = zl + (dble(kk) + 0.5d0) * dz_sub

              do jj = 0, nsub-1
                 yy = yl + (dble(jj) + 0.5d0) * dy_sub

                 do ii = 0, nsub-1
                    xx = xl + (dble(ii) + 0.5d0) * dx_sub
                    
                    dist = sqrt((xx-center(1))**2 + (yy-center(2))**2 + (zz-center(3))**2)
                    
                    ! use a tanh profile to smooth the transition between rho_0 
                    ! and rho_ambient
                    rho_n = rho_0 - 0.5d0*(rho_0 - rho_ambient)* &
                         (1.d0 + tanh((dist - r_0)/smooth_delta))

                    avg_rho = avg_rho + rho_n
                    
                 enddo
              enddo
           enddo
        
           state(i,j,k,URHO) = avg_rho * volinv
           
           eos_state % rho = state(i,j,k,URHO)
           eos_state % p   = p_0
           eos_state % T   = small_temp ! Initial guess for the EOS
           eos_state % xn  = X_0

           call eos(eos_input_rp, eos_state)

           temp = eos_state % T
           eint = eos_state % e
           
           state(i,j,k,UTEMP) = temp
           state(i,j,k,UMX) = 0.d0
           state(i,j,k,UMY) = 0.d0
           state(i,j,k,UMZ) = 0.d0
           state(i,j,k,UEDEN) = state(i,j,k,URHO) * eint
           state(i,j,k,UEINT) = state(i,j,k,URHO) * eint
           state(i,j,k,UFS:UFS+nspec-1) = state(i,j,k,URHO) * X_0(1:nspec)
           
        enddo
     enddo
  enddo
  
end subroutine ca_initdata

