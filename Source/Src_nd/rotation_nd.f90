module rot_sources_module

  implicit none

  private

  public add_rot_source, cross_product, fill_rotation_field, get_omega, get_domegadt

contains

  function get_omega(time) result(omega)

    use prob_params_module, only: coord_type
    use meth_params_module, only: rot_period, rot_period_dot, rot_axis
    use bl_constants_module, only: ZERO, TWO, M_PI

    implicit none

    double precision :: time
    double precision :: omega(3)

    double precision :: curr_period

    if (coord_type == 0) then
       ! If rot_period is less than zero, that means rotation is disabled, and so we should effectively
       ! shut off the source term by setting omega = 0.

       omega = (/ ZERO, ZERO, ZERO /)

       if (rot_period > ZERO) then

          ! If we have a time rate of change of the rotational period,
          ! adjust it accordingly in the calculation of omega. We assume 
          ! that the change has been linear and started at t == 0.

          curr_period = rot_period + rot_period_dot * time

          omega(rot_axis) = TWO * M_PI / curr_period

       endif
    else
       call bl_error("Error:: Castro_rot_sources_3d.f90 :: unknown coord_type")
    endif

  end function



  function get_domegadt(time) result(domegadt)

    use prob_params_module, only: coord_type
    use meth_params_module, only: rot_period, rot_period_dot, rot_axis
    use bl_constants_module, only: ZERO, TWO, M_PI

    implicit none

    double precision :: time
    double precision :: domegadt(3)

    double precision :: curr_period, curr_omega(3)

    if (coord_type == 0) then

       domegadt = (/ ZERO, ZERO, ZERO /)

       if (rot_period > ZERO) then

          ! Rate of change of the rotational frequency is given by
          ! d( ln(period) ) / dt = - d( ln(omega) ) / dt

          curr_period = rot_period + rot_period_dot * time
          curr_omega  = get_omega(time)

          domegadt = curr_omega * (-rot_period_dot / curr_period)

       endif
    else
       call bl_error("Error:: Castro_rot_sources_3d.f90 :: unknown coord_type")
    endif

  end function get_domegadt
    


  subroutine fill_rotation_field(rot,rot_l1,rot_l2,rot_l3,rot_h1,rot_h2,rot_h3, &
                                 q,q_l1,q_l2,q_l3,q_h1,q_h2,q_h3,lo,hi,dx,time)

    ! fill_rotation_field returns the sources to the velocity
    ! equations (not the conserved momentum equations) that are used
    ! in predicting the interface states
    use meth_params_module, only: QVAR, QU, QV, QW, NHYP
    use prob_params_module, only: problo, center
    use bl_constants_module

    implicit none

    integer         , intent(in   ) :: lo(3), hi(3)
    integer         , intent(in   ) :: rot_l1,rot_l2,rot_l3,rot_h1,rot_h2,rot_h3
    integer         , intent(in   ) :: q_l1,q_l2,q_l3,q_h1,q_h2,q_h3

    double precision, intent(inout) :: rot(rot_l1:rot_h1,rot_l2:rot_h2,rot_l3:rot_h3,3)
    double precision, intent(in   ) :: q(q_l1:q_h1,q_l2:q_h2,q_l3:q_h3,QVAR)
    double precision, intent(in   ) :: dx(3),time

    integer          :: i,j,k
    double precision :: x,y,z,r(3)
    double precision :: v(3),omega(3),domegadt(3)
    double precision :: omegacrossr(3),omegacrossomegacrossr(3),omegacrossv(3)

    integer, parameter :: ngq = NHYP
    
    omega = get_omega(time)
    domegadt = get_domegadt(time)

    ! fill all the rotation ghost cells, because we do tracing in the
    ! PPM routines
    do k = lo(3)-ngq, hi(3)+ngq
       z = problo(3) + dx(3)*(dble(k)+HALF) - center(3)

       do j = lo(2)-ngq, hi(2)+ngq
          y = problo(2) + dx(2)*(dble(j)+HALF) - center(2)

          do i = lo(1)-ngq, hi(1)+ngq
             x = problo(1) + dx(1)*(dble(i)+HALF) - center(1)

             r = (/ x, y, z /)

             omegacrossr = cross_product(omega,r)
             omegacrossomegacrossr = cross_product(omega,omegacrossr)

             v = (/ q(i,j,k,QU), q(i,j,k,QV), q(i,j,k,QW) /)

             omegacrossv = cross_product(omega,v)

             rot(i,j,k,:) = -TWO * omegacrossv - omegacrossomegacrossr - cross_product(domegadt, r)
             
          enddo
       enddo
    enddo

  end subroutine fill_rotation_field


  subroutine add_rot_source(uin,uin_l1,uin_l2,uin_l3,uin_h1,uin_h2,uin_h3, &
                            uout,uout_l1,uout_l2,uout_l3,uout_h1,uout_h2,uout_h3, &
                            lo,hi,dx,dt,time,E_added,xmom_added,ymom_added,zmom_added)

    ! Here we add dt * S_rot^n -- the time-level n rotation source to
    ! the momentum equation.  Note that uin here is the state at time
    ! n -- no sources should have been added to it (including
    ! gravity).
    
    use meth_params_module, only: NVAR, URHO, UMX, UMY, UMZ, UEDEN, rot_period, rot_source_type
    use prob_params_module, only: coord_type, problo, center
    use bl_constants_module

    implicit none

    integer         , intent(in   ) :: lo(3), hi(3)
    integer         , intent(in   ) :: uin_l1,uin_l2,uin_l3,uin_h1,uin_h2,uin_h3
    integer         , intent(in   ) :: uout_l1,uout_l2,uout_l3,uout_h1,uout_h2,uout_h3

    double precision, intent(in   ) ::  uin( uin_l1: uin_h1, uin_l2: uin_h2, uin_l3: uin_h3,NVAR)
    double precision, intent(inout) :: uout(uout_l1:uout_h1,uout_l2:uout_h2,uout_l3:uout_h3,NVAR)
    double precision, intent(in   ) :: dx(3), dt, time

    integer          :: i,j,k
    double precision :: x,y,z,r(3)
    double precision :: v(3),omega(3),domegadt(3)
    double precision :: Sr(3),SrU,SrV,SrW,SrE
    double precision :: dens
    double precision :: omegacrossr(3),omegacrossv(3)

    double precision :: old_rhoeint, new_rhoeint, old_ke, new_ke, old_re
    double precision :: old_xmom, old_ymom, old_zmom
    double precision :: E_added, xmom_added, ymom_added, zmom_added

    omega = get_omega(time)
    domegadt = get_domegadt(time)

    do k = lo(3), hi(3)
       z = problo(3) + dx(3)*(dble(k)+HALF) - center(3)

       do j = lo(2), hi(2)
          y = problo(2) + dx(2)*(dble(j)+HALF) - center(2)

          do i = lo(1), hi(1)
             x = problo(1) + dx(1)*(dble(i)+HALF) - center(1)

             ! **** Start Diagnostics ****
             old_re = uout(i,j,k,UEDEN)
             old_ke = HALF * (uout(i,j,k,UMX)**2 + uout(i,j,k,UMY)**2 + uout(i,j,k,UMZ)**2) / &
                               uout(i,j,k,URHO) 
             old_rhoeint = uout(i,j,k,UEDEN) - old_ke
             old_xmom = uout(i,j,k,UMX)
             old_ymom = uout(i,j,k,UMY)
             old_zmom = uout(i,j,k,UMZ)
             ! ****   End Diagnostics ****

             r = (/ x, y, z /)

             dens = uin(i,j,k,URHO)

             v = (/ uin(i,j,k,UMX)/dens, &
                    uin(i,j,k,UMY)/dens, &
                    uin(i,j,k,UMZ)/dens /)

             omegacrossr = cross_product(omega,r)
             omegacrossv = cross_product(omega,v)

             ! momentum sources: this is the Coriolis force
             ! (-2 rho omega x v) and the centrifugal force
             ! (-rho omega x ( omega x r))

             Sr = - TWO * dens * omegacrossv(:) - dens * cross_product(omega, omegacrossr) &
                  - dens * cross_product(domegadt, r)

             SrU = Sr(1)
             SrV = Sr(2)
             SrW = Sr(3)

             uout(i,j,k,UMX) = uout(i,j,k,UMX) + SrU * dt
             uout(i,j,k,UMY) = uout(i,j,k,UMY) + SrV * dt
             uout(i,j,k,UMZ) = uout(i,j,k,UMZ) + SrW * dt

             ! Kinetic energy source: this is v . the momentum source.
             ! We don't apply in the case of the conservative energy
             ! formulation.

             if (rot_source_type == 1 .or. rot_source_type == 2) then

               SrE = dot_product(v, Sr)

               uout(i,j,k,UEDEN) = uout(i,j,k,UEDEN) + SrE * dt

             else if (rot_source_type .eq. 3) then

                new_ke = HALF * (uout(i,j,k,UMX)**2 + uout(i,j,k,UMY)**2 + uout(i,j,k,UMZ)**2) / &
                                  uout(i,j,k,URHO) 
                uout(i,j,k,UEDEN) = old_rhoeint + new_ke

             else if (rot_source_type .eq. 4) then

                ! Do nothing here, for the conservative rotation option.

             else 
                call bl_error("Error:: Castro_rot_sources_3d.f90 :: bogus rot_source_type")
             end if

             ! **** Start Diagnostics ****
             new_ke = HALF * (uout(i,j,k,UMX)**2 + uout(i,j,k,UMY)**2 + uout(i,j,k,UMZ)**2) / &
                               uout(i,j,k,URHO) 

             new_rhoeint = uout(i,j,k,UEDEN) - new_ke
 
             E_added =  E_added + uout(i,j,k,UEDEN) - old_re

             xmom_added = xmom_added + uout(i,j,k,UMX) - old_xmom
             ymom_added = ymom_added + uout(i,j,k,UMY) - old_ymom
             zmom_added = zmom_added + uout(i,j,k,UMZ) - old_zmom
             ! ****   End Diagnostics ****

          enddo
       enddo
    enddo

  end subroutine add_rot_source

  function cross_product(x,y) result(r)

    implicit none

    double precision :: x(3), y(3)
    double precision :: r(3)

    r(1) = x(2)*y(3) - x(3)*y(2)
    r(2) = x(3)*y(1) - x(1)*y(3)
    r(3) = x(1)*y(2) - x(2)*y(1)

  end function cross_product

end module rot_sources_module



  subroutine ca_corrrsrc(lo,hi, &
                         uold,uold_l1,uold_l2,uold_l3,uold_h1,uold_h2,uold_h3, &
                         unew,unew_l1,unew_l2,unew_l3,unew_h1,unew_h2,unew_h3, &
                         flux1,flux1_l1,flux1_l2,flux1_l3,flux1_h1,flux1_h2,flux1_h3, &
                         flux2,flux2_l1,flux2_l2,flux2_l3,flux2_h1,flux2_h2,flux2_h3, &
                         flux3,flux3_l1,flux3_l2,flux3_l3,flux3_h1,flux3_h2,flux3_h3, &
                         dx,dt,time, &
                         vol,vol_l1,vol_l2,vol_l3,vol_h1,vol_h2,vol_h3, &
                         xmom_added,ymom_added,zmom_added,E_added)

    ! Corrector step for the rotation source terms. This is applied after the hydrodynamics 
    ! update to fix the time-level n prediction and add the time-level n+1 data.
    ! This subroutine exists outside of the Fortran module above because it needs to be called 
    ! directly from C++.

    use mempool_module, only : bl_allocate, bl_deallocate
    use meth_params_module, only: NVAR, URHO, UMX, UMY, UMZ, UEDEN, rot_period, rot_source_type, rot_axis
    use prob_params_module, only: coord_type, problo, center
    use bl_constants_module
    use rot_sources_module, only: cross_product, get_omega, get_domegadt

    implicit none

    integer         , intent(in   ) :: lo(3), hi(3)
    double precision, intent(in   ) :: dx(3), dt, time

    integer :: uold_l1,uold_l2,uold_l3,uold_h1,uold_h2,uold_h3
    integer :: unew_l1,unew_l2,unew_l3,unew_h1,unew_h2,unew_h3

    integer :: flux1_l1,flux1_l2,flux1_l3,flux1_h1,flux1_h2,flux1_h3
    integer :: flux2_l1,flux2_l2,flux2_l3,flux2_h1,flux2_h2,flux2_h3
    integer :: flux3_l1,flux3_l2,flux3_l3,flux3_h1,flux3_h2,flux3_h3

    integer :: vol_l1,vol_l2,vol_l3,vol_h1,vol_h2,vol_h3

    double precision :: uold(uold_l1:uold_h1,uold_l2:uold_h2,uold_l3:uold_h3,NVAR)
    double precision :: unew(unew_l1:unew_h1,unew_l2:unew_h2,unew_l3:unew_h3,NVAR)

    double precision :: flux1(flux1_l1:flux1_h1,flux1_l2:flux1_h2,flux1_l3:flux1_h3,NVAR)
    double precision :: flux2(flux2_l1:flux2_h1,flux2_l2:flux2_h2,flux2_l3:flux2_h3,NVAR)
    double precision :: flux3(flux3_l1:flux3_h1,flux3_l2:flux3_h2,flux3_l3:flux3_h3,NVAR)

    double precision :: vol(vol_l1:vol_h1,vol_l2:vol_h2,vol_l3:vol_h3)

    integer          :: i,j,k
    double precision :: x,y,z,r(3)
    double precision :: vnew(3),vold(3),omega_old(3),domegadt_old(3),omega_new(3),domegadt_new(3)
    double precision :: Sr_old(3), Sr_new(3), SrUcorr, SrVcorr, SrWcorr, SrEcorr, SrE_old, SrE_new
    double precision :: rhoo, rhon, rhooinv, rhoninv
    double precision :: omegacrossrold(3),omegacrossrnew(3)
    double precision :: omegacrossvold(3),omegacrossvnew(3)

    double precision :: old_ke, old_rhoeint, old_re, new_ke, new_rhoeint
    double precision :: old_xmom, old_ymom, old_zmom
    double precision :: E_added, xmom_added, ymom_added, zmom_added

    double precision, pointer :: phi(:,:,:)
    double precision, pointer :: drho1(:,:,:), drho2(:,:,:), drho3(:,:,:)

    double precision :: mom1, mom2

    integer :: idir1, idir2, midx1, midx2

    omega_old = get_omega(time)
    domegadt_old = get_domegadt(time)

    omega_new = get_omega(time + dt)
    domegadt_new = get_domegadt(time + dt)

    if (rot_source_type == 4) then

       ! Construct rotational potential, phi_R = -1/2 | omega x r |**2

       call bl_allocate(phi, lo(1)-1,hi(1)+1,lo(2)-1,hi(2)+1,lo(3)-1,hi(3)+1)
       call bl_allocate(drho1, lo(1),hi(1)+1,lo(2),hi(2),lo(3),hi(3))
       call bl_allocate(drho2, lo(1),hi(1),lo(2),hi(2)+1,lo(3),hi(3))
       call bl_allocate(drho3, lo(1),hi(1),lo(2),hi(2),lo(3),hi(3)+1)

       do k = lo(3)-1, hi(3)+1
          z = problo(3) + dx(3)*(dble(k)+HALF) - center(3)
          do j = lo(2)-1, hi(2)+1
             y = problo(2) + dx(2)*(dble(j)+HALF) - center(2)
             do i = lo(1)-1, hi(1)+1
                x = problo(1) + dx(1)*(dble(i)+HALF) - center(1)
                
                r = (/ x, y, z /)

                ! Average old and new time rotational potentials
                ! to get time-centered potential

                omegacrossrold = cross_product(omega_old, r)
                omegacrossrnew = cross_product(omega_new, r)

                phi(i,j,k) = HALF * HALF * (dot_product(omegacrossrold, omegacrossrold) + &
                                            dot_product(omegacrossrnew, omegacrossrnew) )

             enddo
          enddo
       enddo

       ! Construct the mass changes using the density flux from the hydro step. 
       ! Note that in the hydrodynamics step, these fluxes were already 
       ! multiplied by dA and dt, so dividing by the cell volume is enough to 
       ! get the density change (flux * dt / dx). This will be fine in the usual 
       ! case where the volume is the same in every cell, but may need to be 
       ! generalized when this assumption does not hold.

       do k = lo(3), hi(3)
          do j = lo(2), hi(2)
             do i = lo(1), hi(1)+1
                drho1(i,j,k) = flux1(i,j,k,URHO) / vol(i,j,k)
             enddo
          enddo
       enddo

       do k = lo(3), hi(3)
          do j = lo(2), hi(2)+1
             do i = lo(1), hi(1)
                drho2(i,j,k) = flux2(i,j,k,URHO) / vol(i,j,k)
             enddo
          enddo
       enddo

       do k = lo(3), hi(3)+1
          do j = lo(2), hi(2)
             do i = lo(1), hi(1)
                drho3(i,j,k) = flux3(i,j,k,URHO) / vol(i,j,k)
             enddo
          enddo
       enddo

    endif

    do k = lo(3), hi(3)
       z = problo(3) + dx(3)*(dble(k)+HALF) - center(3)
       do j = lo(2), hi(2)
          y = problo(2) + dx(2)*(dble(j)+HALF) - center(2)
          do i = lo(1), hi(1)
             x = problo(1) + dx(1)*(dble(i)+HALF) - center(1)

             ! **** Start Diagnostics ****
             old_re = unew(i,j,k,UEDEN)
             old_ke = HALF * (unew(i,j,k,UMX)**2 + unew(i,j,k,UMY)**2 + unew(i,j,k,UMZ)**2) / &
                               unew(i,j,k,URHO) 
             old_rhoeint = unew(i,j,k,UEDEN) - old_ke
             old_xmom = unew(i,j,k,UMX)
             old_ymom = unew(i,j,k,UMY)
             old_zmom = unew(i,j,k,UMZ)
             ! ****   End Diagnostics ****

             r = (/ x, y, z /)

             ! Define old source terms

             rhoo = uold(i,j,k,URHO)
             rhooinv = ONE / uold(i,j,k,URHO)

             vold = (/ uold(i,j,k,UMX) * rhooinv, &
                       uold(i,j,k,UMY) * rhooinv, &
                       uold(i,j,k,UMZ) * rhooinv /)

             omegacrossrold = cross_product(omega_old, r   )
             omegacrossvold = cross_product(omega_old, vold)

             Sr_old = - TWO * rhoo * omegacrossvold - rhoo * cross_product(omega_old, omegacrossrold) &
                      - rhoo * cross_product(domegadt_old, r)

             SrE_old = dot_product(vold, Sr_old) ! Energy update; only centrifugal term does work

             ! Define new source terms

             rhon = unew(i,j,k,URHO)
             rhoninv = ONE / unew(i,j,k,URHO)
             
             vnew = (/ unew(i,j,k,UMX) * rhoninv, &
                       unew(i,j,k,UMY) * rhoninv, &
                       unew(i,j,k,UMZ) * rhoninv /)

             omegacrossrnew = cross_product(omega_new, r   )
             omegacrossvnew = cross_product(omega_new ,vnew)

             Sr_new = - TWO * rhon * omegacrossvnew - rhon * cross_product(omega_new, omegacrossrnew) &
                      - rhon * cross_product(domegadt_new, r)

             SrE_new = dot_product(vnew, Sr_new)

             ! Define correction terms

             SrUcorr = HALF * (Sr_new(1) - Sr_old(1))
             SrVcorr = HALF * (Sr_new(2) - Sr_old(2))
             SrWcorr = HALF * (Sr_new(3) - Sr_old(3))

             SrEcorr = HALF * (SrE_new - SrE_old)

             ! Correct state with correction terms

             unew(i,j,k,UMX) = unew(i,j,k,UMX) + SrUcorr * dt
             unew(i,j,k,UMY) = unew(i,j,k,UMY) + SrVcorr * dt
             unew(i,j,k,UMZ) = unew(i,j,k,UMZ) + SrWcorr * dt

             if (rot_source_type == 1) then

               ! If rot_source_type == 1, then calculate SrEcorr before updating the velocities.

                unew(i,j,k,UEDEN) = unew(i,j,k,UEDEN) + SrEcorr * dt

             else if (rot_source_type == 2) then

                ! For this source type, we first update the momenta
                ! before we calculate the energy source term.

                vnew(1) = unew(i,j,k,UMX) * rhoninv
                vnew(2) = unew(i,j,k,UMY) * rhoninv
                vnew(3) = unew(i,j,k,UMZ) * rhoninv

                omegacrossrnew = cross_product(omega_new, r   )
                omegacrossvnew = cross_product(omega_new, vnew)

                Sr_new = - TWO * rhon * omegacrossvnew - rhon * cross_product(omega_new, omegacrossrnew) &
                         - rhon * cross_product(domegadt_new, r)

                SrE_new = dot_product(vnew, Sr_new)

                SrEcorr = HALF * (SrE_new - SrE_old)

                unew(i,j,k,UEDEN) = unew(i,j,k,UEDEN) + SrEcorr * dt

             else if (rot_source_type == 3) then

                ! Instead of calculating the energy source term explicitly,
                ! we simply set the total energy equal to the old internal
                ! energy plus the new kinetic energy.

                new_ke = HALF * (unew(i,j,k,UMX)**2 + unew(i,j,k,UMY)**2 + unew(i,j,k,UMZ)**2) / &
                                 unew(i,j,k,URHO) 
 
                unew(i,j,k,UEDEN) = old_rhoeint + new_ke

             else if (rot_source_type == 4) then

                ! Coupled momentum update.
                ! See Section 2.4 in the first wdmerger paper.

                ! Figure out which directions are updated, and then determine the right 
                ! array index relative to UMX (this works because UMX, UMY, UMZ are consecutive
                ! in the state array).

                idir1 = 1 + MOD(rot_axis    , 3)
                idir2 = 1 + MOD(rot_axis + 1, 3)

                midx1 = UMX + idir1 - 1
                midx2 = UMX + idir2 - 1

                ! We need to use vnew because the state has already been updated by this point
                ! using the standard method; we need to look at the velocities before that has occurred.

                mom1 = rhon * vnew(idir1)
                mom2 = rhon * vnew(idir2)

                ! Now do the implicit solve for the time-level n+1 Coriolis term. 
                ! It would be nice if this all could be generalized so that we don't 
                ! have to break it up by coordinate axis (in case the user wants to 
                ! rotate about multiple axes).

                unew(i,j,k,midx1) = (mom1 + dt * omega_new(rot_axis) * mom2) / (ONE + (dt * omega_new(rot_axis))**2)
                unew(i,j,k,midx2) = (mom2 - dt * omega_new(rot_axis) * mom1) / (ONE + (dt * omega_new(rot_axis))**2)

                ! Do the full corrector step with the centrifugal force (add 1/2 the new term, subtract 1/2 the old term)
                ! and do the remaining part of the corrector step for the Coriolis term (subtract 1/2 the old term). 

                unew(i,j,k,midx1) = unew(i,j,k,midx1) + HALF * dt * omega_new(rot_axis)**2 * r(idir1) * (rhon - rhoo) &
                                - dt * omega_old(rot_axis) * uold(i,j,k,midx2)
                unew(i,j,k,midx2) = unew(i,j,k,midx2) + HALF * dt * omega_new(rot_axis)**2 * r(idir2) * (rhon - rhoo) &
                                + dt * omega_old(rot_axis) * uold(i,j,k,midx1)

                ! The change in the gas energy is equal in magnitude to, and opposite in sign to,
                ! the change in the rotational potential energy, (1/2) rho * phi.
                ! This must be true for the total energy, rho * E_g + rho * phi, to be conserved.
                ! Consider as an example the zone interface i+1/2 in between zones i and i + 1.
                ! There is an amount of mass drho_{i+1/2} leaving the zone. It is going from 
                ! a potential of phi_i to a potential of phi_{i+1}. Therefore the new rotational
                ! energy is equal to the mass changed multiplied by the difference between these two
                ! potentials. This is a generalization of the cell-centered approach implemented in 
                ! the other source options, which effectively are equal to 
                ! SrEcorr = - HALF * drho(i,j,k) * phi(i,j,k),
                ! where drho(i,j,k) = unew(i,j,k,URHO) - uold(i,j,k,URHO).

                SrEcorr = - HALF * ( drho1(i  ,j,k) * (phi(i,j,k) - phi(i-1,j,k)) - &
                                     drho1(i+1,j,k) * (phi(i,j,k) - phi(i+1,j,k)) + &
                                     drho2(i,j  ,k) * (phi(i,j,k) - phi(i,j-1,k)) - &
                                     drho2(i,j+1,k) * (phi(i,j,k) - phi(i,j+1,k)) + &
                                     drho3(i,j,k  ) * (phi(i,j,k) - phi(i,j,k-1)) - &
                                     drho3(i,j,k+1) * (phi(i,j,k) - phi(i,j,k+1)) )

                ! Correct for the time rate of change of the potential, which acts 
                ! purely as a source term. For the velocities this is a corrector step
                ! and for the energy we add the full source term.

                Sr_old = - rhoo * cross_product(domegadt_old, r)
                Sr_new = - rhon * cross_product(domegadt_new, r)

                unew(i,j,k,UMX:UMZ) = unew(i,j,k,UMX:UMZ) + HALF * (Sr_new - Sr_old) * dt

                vnew = unew(i,j,k,UMX:UMZ) / rhon
                                
                SrEcorr = SrEcorr + HALF * (dot_product(vold, Sr_old) + dot_product(vnew, Sr_new)) * dt

                unew(i,j,k,UEDEN) = unew(i,j,k,UEDEN) + SrEcorr

             else 
                call bl_error("Error:: Castro_rot_sources_3d.f90 :: bogus rot_source_type")
             end if

             ! **** Start Diagnostics ****
             ! This is the new (rho e) as stored in (rho E) after the gravitational work is added
             new_ke = HALF * (unew(i,j,k,UMX)**2 + unew(i,j,k,UMY)**2 + unew(i,j,k,UMZ)**2) / &
                               unew(i,j,k,URHO) 
             new_rhoeint = unew(i,j,k,UEDEN) - new_ke
             E_added =  E_added + unew(i,j,k,UEDEN) - old_re
             xmom_added = xmom_added + unew(i,j,k,UMX) - old_xmom
             ymom_added = ymom_added + unew(i,j,k,UMY) - old_ymom
             zmom_added = zmom_added + unew(i,j,k,UMZ) - old_zmom
             ! ****   End Diagnostics ****

          enddo
       enddo
    enddo

    if (rot_source_type .eq. 4) then
       call bl_deallocate(phi)
       call bl_deallocate(drho1)
       call bl_deallocate(drho2)
       call bl_deallocate(drho3)
    endif

    end subroutine ca_corrrsrc



