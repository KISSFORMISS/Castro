module ctu_advection_module

  use bl_constants_module, only : ZERO, HALF, ONE, FOURTH, TWO
  use amrex_fort_module, only : rt => amrex_real

  implicit none

  private

  public umeth2d, consup

contains

! ::: ---------------------------------------------------------------
! ::: :: UMETH2D     Compute hyperbolic fluxes using unsplit second
! ::: ::               order Godunov integrator.
! ::: ::
! ::: :: inputs/outputs
! ::: :: q           => (const)  input state, primitives
! ::: :: qaux        => (const)  auxillary hydro info
! ::: :: flatn       => (const)  flattening parameter
! ::: :: srcQ        => (const)  primitive variable source
! ::: :: nx          => (const)  number of cells in X direction
! ::: :: ny          => (const)  number of cells in Y direction
! ::: :: dx          => (const)  grid spacing in X direction
! ::: :: dy          => (const)  grid spacing in Y direction
! ::: :: dt          => (const)  time stepsize
! ::: :: flux1      <=  (modify) flux in X direction on X edges
! ::: :: flux2      <=  (modify) flux in Y direction on Y edges
! ::: ----------------------------------------------------------------

  subroutine umeth2d(q, q_lo, q_hi, &
                     flatn, &
                     qaux, qa_lo, qa_hi, &
                     srcQ, src_lo, src_hi, &
                     lo, hi, dx, dy, dt, &
                     uout, uout_lo, uout_hi, &
                     flux1, fd1_lo, fd1_hi, &
                     flux2, fd2_lo, fd2_hi, &
#ifdef RADIATION
                     rflux1, rfd1_lo, rfd1_hi, &
                     rflux2, rfd2_lo, rfd2_hi, &
#endif
                     q1, q1_lo, q1_hi, &
                     q2, q2_lo, q2_hi, &
                     area1, area1_lo, area1_hi, &
                     area2, area2_lo, area2_hi, &
                     pdivu, vol, vol_lo, vol_hi, &
                     dloga, dloga_lo, dloga_hi, &
                     domlo, domhi)

    use meth_params_module, only : QVAR, NVAR, ppm_type, hybrid_riemann, &
                                   GDU, GDV, GDPRES, NGDNV, NQ, &
                                   NQAUX
    use trace_module, only : trace
#ifdef RADIATION
    use rad_params_module, only : ngroups
    use trace_ppm_rad_module, only : trace_ppm_rad
#else
    use trace_ppm_module, only : trace_ppm
#endif
    use transverse_module, only : transx, transy
    use riemann_module, only: cmpflx, shock
#ifdef SHOCK_VAR
    use meth_params_module, only : USHK
#endif
    use amrex_fort_module, only : rt => amrex_real

    implicit none

    integer, intent(in) :: q_lo(3), q_hi(3)
    integer, intent(in) :: qa_lo(3), qa_hi(3)
    integer, intent(in) :: dloga_lo(3), dloga_hi(3)
    integer, intent(in) :: src_lo(3), src_hi(3)
    integer, intent(in) :: uout_lo(3), uout_hi(3)
    integer, intent(in) :: fd1_lo(3), fd1_hi(3)
    integer, intent(in) :: fd2_lo(3), fd2_hi(3)
#ifdef RADIATION
    integer, intent(in) :: rfd1_lo(3), rfd1_hi(3)
    integer, intent(in) :: rfd2_lo(3), rfd2_hi(3)
#endif
    integer, intent(in) :: q1_lo(3), q1_hi(3)
    integer, intent(in) :: q2_lo(3), q2_hi(3)
    integer, intent(in) :: area1_lo(3), area1_hi(3)
    integer, intent(in) :: area2_lo(3), area2_hi(3)
    integer, intent(in) :: vol_lo(3), vol_hi(3)
    integer, intent(in) :: lo(2), hi(2)
    integer, intent(in) :: domlo(2), domhi(2)

    real(rt)        , intent(in) :: dx, dy, dt
    real(rt)        , intent(in) :: q(q_lo(1):q_hi(1),q_lo(2):q_hi(2),NQ)
    real(rt)        , intent(inout) :: qaux(qa_lo(1):qa_hi(1),qa_lo(2):qa_hi(2),NQAUX)
    real(rt)        , intent(in) :: flatn(q_lo(1):q_hi(1),q_lo(2):q_hi(2))
    real(rt)        , intent(in) :: srcQ(src_lo(1):src_hi(1),src_lo(2):src_hi(2),QVAR)
    real(rt)        , intent(in) :: dloga(dloga_lo(1):dloga_hi(1),dloga_lo(2):dloga_hi(2))
    real(rt)        , intent(inout) :: q1(q1_lo(1):q1_hi(1),q1_lo(2):q1_hi(2),NGDNV)
    real(rt)        , intent(inout) :: q2(q2_lo(1):q2_hi(1),q2_lo(2):q2_hi(2),NGDNV)
    real(rt)        , intent(inout) :: uout(uout_lo(1):uout_hi(1),uout_lo(2):uout_hi(2),NVAR)
    real(rt)        , intent(inout) :: flux1(fd1_lo(1):fd1_hi(1),fd1_lo(2):fd1_hi(2),NVAR)
    real(rt)        , intent(inout) :: flux2(fd2_lo(1):fd2_hi(1),fd2_lo(2):fd2_hi(2),NVAR)
#ifdef RADIATION
    real(rt)        , intent(inout) :: rflux1(rfd1_lo(1):rfd1_hi(1),rfd1_lo(2):rfd1_hi(2),0:ngroups-1)
    real(rt)        , intent(inout) :: rflux2(rfd2_lo(1):rfd2_hi(1),rfd2_lo(2):rfd2_hi(2),0:ngroups-1)
#endif
    real(rt)        , intent(in) :: area1(area1_lo(1):area1_hi(1),area1_lo(2):area1_hi(2))
    real(rt)        , intent(in) :: area2(area2_lo(1):area2_hi(1),area2_lo(2):area2_hi(2))
    real(rt)        , intent(inout) :: pdivu(lo(1):hi(1),lo(2):hi(2))
    real(rt)        , intent(in) :: vol(vol_lo(1):vol_hi(1),vol_lo(2):vol_hi(2))

    ! Left and right state arrays (edge centered, cell centered)
    real(rt)        , allocatable::  qm(:,:,:),  qp(:,:,:)
    real(rt)        , allocatable:: qxm(:,:,:), qym(:,:,:)
    real(rt)        , allocatable:: qxp(:,:,:), qyp(:,:,:)

    ! Work arrays to hold riemann state and conservative fluxes

    real(rt)        , allocatable ::  fx(:,:,:),  fy(:,:,:)
#ifdef RADIATION
    real(rt)        , allocatable ::  rfx(:,:,:),  rfy(:,:,:)
#endif
    real(rt)        , allocatable ::  qgdxtmp(:,:,:)
    real(rt)        , allocatable :: shk(:,:)

    ! Local scalar variables
    real(rt)         :: dtdx
    real(rt)         :: hdtdx, hdt, hdtdy
    integer          :: i,j

    integer :: tflx_lo(3), tflx_hi(3)
    integer :: tfly_lo(3), tfly_hi(3)
    integer :: shk_lo(3), shk_hi(3)
    integer :: qp_lo(3), qp_hi(3)

    tflx_lo = [lo(1), lo(2)-1, 0]
    tflx_hi = [hi(1)+1, hi(2)+1, 0]

    tfly_lo = [lo(1)-1, lo(2), 0]
    tfly_hi = [hi(1)+1, hi(2)+1, 0]

    shk_lo = [lo(1)-1, lo(2)-1, 0]
    shk_hi = [hi(1)+1, hi(2)+1, 0]

    qp_lo = [lo(1)-1, lo(2)-1, 0]
    qp_hi = [hi(1)+2, hi(2)+2, 0]

    allocate ( qgdxtmp(q1_lo(1):q1_hi(1),q1_lo(2):q1_hi(2),NGDNV))

    allocate (  qm(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )
    allocate (  qp(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )
    allocate ( qxm(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )
    allocate ( qxp(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )
    allocate ( qym(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )
    allocate ( qyp(lo(1)-1:hi(1)+2,lo(2)-1:hi(2)+2,NQ) )

    allocate (  fx(tflx_lo(1):tflx_hi(1),tflx_lo(2):tflx_hi(2),NVAR) )
    allocate (  fy(tfly_lo(1):tfly_hi(1),tfly_lo(2):tfly_hi(2),NVAR) )
#ifdef RADIATION
    allocate ( rfx(tflx_lo(1):tflx_hi(1),tflx_lo(2):tflx_hi(2),0:ngroups-1) )
    allocate ( rfy(tfly_lo(1):tfly_hi(1),tfly_lo(2):tfly_hi(2),0:ngroups-1) )
#endif

    allocate (shk(shk_lo(1):shk_hi(1),shk_lo(2):shk_hi(2)))


    ! Local constants
    dtdx = dt/dx
    hdtdx = HALF*dtdx
    hdtdy = HALF*dt/dy
    hdt = HALF*dt

    ! multidimensional shock detection

#ifdef SHOCK_VAR
    uout(lo(1):hi(1),lo(2):hi(2),USHK) = ZERO

    call shock(q, q_lo, q_hi, &
               shk, shk_lo, shk_hi, &
               lo, hi, dx, dy)

    ! Store the shock data for future use in the burning step.

    do j = lo(2), hi(2)
       do i = lo(1), hi(1)
          uout(i,j,USHK) = shk(i,j)
       enddo
    enddo

    ! Discard it locally if we don't need it in the hydro update.

    if (hybrid_riemann /= 1) then
       shk(:,:) = ZERO
    endif
#else
    ! multidimensional shock detection -- this will be used to do the
    ! hybrid Riemann solver
    if (hybrid_riemann == 1) then
       call shock(q, q_lo, q_hi, &
                  shk, shk_lo, shk_hi, &
                  lo, hi, dx, dy)
    else
       shk(:,:) = ZERO
    endif
#endif

    ! NOTE: Geometry terms need to be punched through

    ! Trace to edges w/o transverse flux correction terms.  Here,
    !      qxm and qxp will be the states on either side of the x interfaces
    ! and  qym and qyp will be the states on either side of the y interfaces
    if (ppm_type .eq. 0) then
#ifdef RADIATION
       call bl_error("ppm_type <=0 is not supported in umeth1d_rad")
#else
       call trace(q, flatn, q_lo, q_hi, &
                  qaux, qa_lo, qa_hi, &
                  dloga, dloga_lo, dloga_hi, &
                  qxm, qxp, qym, qyp, qp_lo, qp_hi, &
                  srcQ, src_lo, src_hi, &
                  lo(1), lo(2), hi(1), hi(2), dx, dy, dt)
#endif
    else
#ifdef RADIATION
       call trace_ppm_rad(q, flatn, q_lo, q_hi, &
                          qaux, qa_lo, qa_hi, &
                          dloga, dloga_lo, dloga_hi, &
                          qxm, qxp, qym, qyp, qp_lo, qp_hi, &
                          srcQ, src_lo, src_hi, &
                          lo(1), lo(2), hi(1), hi(2), dx, dy, dt)

#else
       call trace_ppm(q, flatn, q_lo, q_hi, &
                      qaux, qa_lo, qa_hi, &
                      dloga, dloga_lo, dloga_hi, &
                      qxm, qxp, qym, qyp, qp_lo, qp_hi, &
                      srcQ, src_lo, src_hi, &
                      lo(1),lo(2),hi(1),hi(2),dx,dy,dt)
#endif
    end if

    ! Solve the Riemann problem in the x-direction using these first
    ! guesses for the x-interface states.  This produces the flux fx
    call cmpflx(qxm, qxp, qp_lo, qp_hi, &
                fx, tflx_lo, tflx_hi, &
                qgdxtmp, q1_lo, q1_hi, &
#ifdef RADIATION
                rfx, tflx_lo, tflx_hi, &
#endif
                qaux, qa_lo, qa_hi, &
                shk, shk_lo, shk_hi, &
                1, lo(1), hi(1), lo(2)-1, hi(2)+1, domlo, domhi)

    ! Solve the Riemann problem in the y-direction using these first
    ! guesses for the y-interface states.  This produces the flux fy
    call cmpflx(qym, qyp, qp_lo, qp_hi, &
                fy, tfly_lo, tfly_hi, &
                q2, q2_lo, q2_hi, &
#ifdef RADIATION
                rfy, tfly_lo, tfly_hi, &
#endif
                qaux, qa_lo, qa_hi, &
                shk, shk_lo, shk_hi, &
                2, lo(1)-1, hi(1)+1, lo(2), hi(2), domlo, domhi)

    ! Correct the x-interface states (qxm, qxp) by adding the
    ! transverse flux difference in the y-direction to the x-interface
    ! states.  This results in the new x-interface states qm and qp
    call transy(qxm, qm, qxp, qp, qp_lo, qp_hi, &
                qaux, qa_lo, qa_hi, &
                fy, tfly_lo, tfly_hi, &
#ifdef RADIATION
                rfy, tfly_lo, tfly_hi, &
#endif
                q2, q2_lo, q2_hi, &
                srcQ, src_lo, src_hi, &
                hdt, hdtdy, &
                lo(1)-1, hi(1)+1, lo(2), hi(2))

    ! Solve the final Riemann problem across the x-interfaces with the
    ! full unsplit states.  The resulting flux through the x-interfaces
    ! is flux1
    call cmpflx(qm, qp, qp_lo, qp_hi, &
                flux1, fd1_lo, fd1_hi, &
                q1, q1_lo, q1_hi, &
#ifdef RADIATION
                rflux1, rfd1_lo, rfd1_hi, &
#endif
                qaux, qa_lo, qa_hi, &
                shk, shk_lo, shk_hi, &
                1, lo(1), hi(1), lo(2), hi(2), domlo, domhi)

    ! Correct the y-interface states (qym, qyp) by adding the
    ! transverse flux difference in the x-direction to the y-interface
    ! states.  This results in the new y-interface states qm and qp
    call transx(qym, qm, qyp, qp, qp_lo, qp_hi, &
                qaux, qa_lo, qa_hi, &
                fx, tflx_lo, tflx_hi, &
#ifdef RADIATION
                rfx, tflx_lo, tflx_hi, &
#endif
                qgdxtmp, q1_lo, q1_hi, &
                srcQ, src_lo, src_hi, &
                hdt, hdtdx, &
                area1, area1_lo, area1_hi, &
                vol, vol_lo, vol_hi, &
                lo(1), hi(1), lo(2)-1, hi(2)+1)

    ! Solve the final Riemann problem across the y-interfaces with the
    ! full unsplit states.  The resulting flux through the y-interfaces
    ! is flux2
    call cmpflx(qm, qp, qp_lo, qp_hi, &
                flux2, fd2_lo, fd2_hi, &
                q2, q2_lo, q2_hi, &
#ifdef RADIATION
                rflux2, rfd2_lo, rfd2_hi, &
#endif
                qaux, qa_lo, qa_hi, &
                shk, shk_lo, shk_hi, &
                2, lo(1), hi(1), lo(2), hi(2), domlo, domhi)

    ! Construct p div{U} -- this will be used as a source to the internal
    ! energy update.  Note we construct this using the interface states
    ! returned from the Riemann solver.
    do j = lo(2), hi(2)
       do i = lo(1), hi(1)
          pdivu(i,j) = HALF*( &
               (q1(i+1,j,GDPRES) + q1(i,j,GDPRES)) * &
               (q1(i+1,j,GDU)*area1(i+1,j) - q1(i,j,GDU)*area1(i,j)) + &
               (q2(i,j+1,GDPRES) + q2(i,j,GDPRES)) * &
               (q2(i,j+1,GDV)*area2(i,j+1) - q2(i,j,GDV)*area2(i,j)) ) / vol(i,j)
       end do
    end do

    deallocate(qm,qp,qxm,qxp,qym,qyp)
    deallocate(fx,fy)
#ifdef RADIATION
    deallocate(rfx,rfy)
#endif
    deallocate(shk)
    deallocate(qgdxtmp)

  end subroutine umeth2d

! :::
! ::: ------------------------------------------------------------------
! :::

  subroutine consup( uin, uin_lo, uin_hi, &
                     q, q_lo, q_hi, &
                     uout, uout_lo, uout_hi, &
                     update, updt_lo, updt_hi, &
                     q1, q1_lo, q1_hi, &
                     q2, q2_lo, q2_hi, &
                     flux1, flux1_lo, flux1_hi, &
                     flux2, flux2_lo, flux2_hi, &
#ifdef RADIATION
                     Erin, Erin_lo, Erin_hi, &
                     Erout, Erout_lo, Erout_hi, &
                     rflux1, rflux1_lo, rflux1_hi, &
                     rflux2, rflux2_lo, rflux2_hi, &
                     nstep_fsp, &
#endif
                     area1, area1_lo, area1_hi, &
                     area2, area2_lo, area2_hi, &
                     vol, vol_lo, vol_hi, &
                     div,pdivu,lo,hi,dx,dy,dt, &
                     mass_lost,xmom_lost,ymom_lost,zmom_lost, &
                     eden_lost,xang_lost,yang_lost,zang_lost, &
                     verbose)

    use meth_params_module, only : difmag, NVAR, URHO, UMX, UMY, UMZ, &
#ifdef RADIATION
                                   GDLAMS, GDERADS, &
                                   comoving, fspace_type, &
                                   GDU, GDV, GDPRES, &
#endif
                                   UEDEN, UEINT, UTEMP, NGDNV, GDPRES, track_grid_losses, &

                                   limit_fluxes_on_small_dens, NQ
    use prob_params_module, only : mom_flux_has_p, domlo_level, domhi_level, center
    use bl_constants_module, only : ZERO, HALF
    use advection_util_2d_module, only : normalize_species_fluxes
    use advection_util_module, only: limit_hydro_fluxes_on_small_dens
    use castro_util_module, only : position, linear_to_angular_momentum
    use amrinfo_module, only : amr_level
#ifdef RADIATION
    use rad_params_module, only : ngroups, nugroup, dlognu
    use radhydro_nd_module, only : advect_in_fspace
    use fluxlimiter_module, only : Edd_factor
#endif
#ifdef SHOCK_VAR
    use meth_params_module, only : USHK
#endif

    use amrex_fort_module, only : rt => amrex_real
    integer, intent(in) :: lo(2), hi(2)
    integer, intent(in) :: uin_lo(3), uin_hi(3)
    integer, intent(in) :: q_lo(3), q_hi(3)
    integer, intent(in) :: uout_lo(3), uout_hi(3)
    integer, intent(in) :: updt_lo(3), updt_hi(3)
    integer, intent(in) :: q1_lo(3), q1_hi(3)
    integer, intent(in) :: q2_lo(3), q2_hi(3)
    integer, intent(in) :: flux1_lo(3), flux1_hi(3)
    integer, intent(in) :: flux2_lo(3), flux2_hi(3)
#ifdef RADIATION
    integer, intent(in) :: Erout_lo(3), Erout_hi(3)
    integer, intent(in) :: Erin_lo(3), Erin_hi(3)
    integer, intent(in) :: rflux1_lo(3), rflux1_hi(3)
    integer, intent(in) :: rflux2_lo(3), rflux2_hi(3)
#endif
    integer, intent(in) :: area1_lo(3), area1_hi(3)
    integer, intent(in) :: area2_lo(3), area2_hi(3)
    integer, intent(in) :: vol_lo(3), vol_hi(3)

    integer, intent(in) :: verbose

    real(rt)        , intent(in) :: uin(uin_lo(1):uin_hi(1),uin_lo(2):uin_hi(2),NVAR)
    real(rt)        , intent(in) :: q(q_lo(1):q_hi(1),q_lo(2):q_hi(2),NQ)
    real(rt)        , intent(in) :: uout(uout_lo(1):uout_hi(1),uout_lo(2):uout_hi(2),NVAR)
    real(rt)        , intent(inout) :: update(updt_lo(1):updt_hi(1),updt_lo(2):updt_hi(2),NVAR)
    real(rt)        , intent(in) :: q1(q1_lo(1):q1_hi(1),q1_lo(2):q1_hi(2),NGDNV)
    real(rt)        , intent(in) :: q2(q2_lo(1):q2_hi(1),q2_lo(2):q2_hi(2),NGDNV)
    real(rt)        , intent(inout) :: flux1(flux1_lo(1):flux1_hi(1),flux1_lo(2):flux1_hi(2),NVAR)
    real(rt)        , intent(inout) :: flux2(flux2_lo(1):flux2_hi(1),flux2_lo(2):flux2_hi(2),NVAR)
#ifdef RADIATION
    real(rt)        , intent(in) :: Erin(Erin_lo(1):Erin_hi(1),Erin_lo(2):Erin_hi(2),0:ngroups-1)
    real(rt)        , intent(inout) :: Erout(Erout_lo(1):Erout_hi(1),Erout_lo(2):Erout_hi(2),0:ngroups-1)
    real(rt)        , intent(inout) :: rflux1(rflux1_lo(1):rflux1_hi(1),rflux1_lo(2):rflux1_hi(2),0:ngroups-1)
    real(rt)        , intent(inout) :: rflux2(rflux2_lo(1):rflux2_hi(1),rflux2_lo(2):rflux2_hi(2),0:ngroups-1)
    integer, intent(inout) :: nstep_fsp
#endif
    real(rt)        , intent(in) :: area1(area1_lo(1):area1_hi(1),area1_lo(2):area1_hi(2))
    real(rt)        , intent(in) :: area2(area2_lo(1):area2_hi(1),area2_lo(2):area2_hi(2))
    real(rt)        , intent(in) :: vol(vol_lo(1):vol_hi(1),vol_lo(2):vol_hi(2))
    real(rt)        , intent(in) :: div(lo(1):hi(1)+1,lo(2):hi(2)+1)
    real(rt)        , intent(in) :: pdivu(lo(1):hi(1),lo(2):hi(2))
    real(rt)        , intent(in) :: dx, dy, dt
    real(rt)        , intent(inout) :: mass_lost, xmom_lost, ymom_lost, zmom_lost
    real(rt)        , intent(inout) :: eden_lost, xang_lost, yang_lost, zang_lost

    integer i, j, k, n

    real(rt)         div1
    !real(rt)         rho, Up, Vp, SrE
    integer domlo(3), domhi(3)
    real(rt)         loc(3), ang_mom(3)

#ifdef RADIATION
    integer :: g
    real(rt)         :: SrU, Up, SrE

    real(rt)        , dimension(0:ngroups-1) :: Erscale
    real(rt)        , dimension(0:ngroups-1) :: ustar, af
    real(rt)         :: Eddf, Eddfxm, Eddfxp, Eddfym, Eddfyp, f1, f2, f1xm, f1xp, f1ym, f1yp
    real(rt)         :: Gf1E(2)
    real(rt)         :: ux, uy, divu, lamc, Egdc
    real(rt)         :: dudx(2), dudy(2), nhat(2), GnDotu(2), nnColonDotGu
    real(rt)         :: dpdx, dprdx, dpdy, dprdy, ek1, ek2, dek
    real(rt)         :: urho_new
    real(rt)         :: umx_new1, umy_new1, umz_new1
    real(rt)         :: umx_new2, umy_new2, umz_new2
#endif

#ifdef RADIATION
    if (ngroups .gt. 1) then
       if (fspace_type .eq. 1) then
          Erscale = dlognu
       else
          Erscale = nugroup*dlognu
       end if
    end if
#endif

    ! Correct the fluxes to include the effects of the artificial viscosity.

    do n = 1, NVAR
       if (n == UTEMP) then
          flux1(lo(1):hi(1)+1,lo(2):hi(2),n) = ZERO
          flux2(lo(1):hi(1),lo(2):hi(2)+1,n) = ZERO
#ifdef SHOCK_VAR
       else if (n == USHK) then
          flux1(lo(1):hi(1)+1,lo(2):hi(2),n) = ZERO
          flux2(lo(1):hi(1),lo(2):hi(2)+1,n) = ZERO
#endif
       else
          do j = lo(2), hi(2)
             do i = lo(1), hi(1)+1
                div1 = HALF*(div(i,j) + div(i,j+1))
                div1 = difmag*min(ZERO, div1)

                flux1(i,j,n) = flux1(i,j,n) + &
                     dx*div1*(uin(i,j,n) - uin(i-1,j,n))
             enddo
          enddo

          do j = lo(2), hi(2)+1
             do i = lo(1), hi(1)
                div1 = HALF*(div(i,j) + div(i+1,j))
                div1 = difmag*min(ZERO,div1)

                flux2(i,j,n) = flux2(i,j,n) + &
                     dy*div1*(uin(i,j,n) - uin(i,j-1,n))
             enddo
          enddo

       endif
    enddo

    if (limit_fluxes_on_small_dens == 1) then
       call limit_hydro_fluxes_on_small_dens(uin, uin_lo, uin_hi, &
                                             q, q_lo, q_hi, &
                                             vol, vol_lo, vol_hi, &
                                             flux1, flux1_lo, flux1_hi, &
                                             area1, area1_lo, area1_hi, &
                                             flux2, flux2_lo, flux2_hi, &
                                             area2, area2_lo, area2_hi, &
                                             [lo(1), lo(2), 0], [hi(1), hi(2), 0], dt, [dx, dy, ZERO])
    endif

    ! Normalize the species fluxes.

    call normalize_species_fluxes(flux1, flux1_lo, flux1_hi, &
                                  flux2, flux2_lo, flux2_hi, &
                                  lo,hi)

#ifdef RADIATION
    do g = 0, ngroups-1
       do j = lo(2),hi(2)
          do i = lo(1), hi(1)+1
             div1 = HALF*(div(i,j) + div(i,j+1))
             div1 = difmag*min(ZERO, div1)

             rflux1(i,j,g) = rflux1(i,j,g) &
                  + dx*div1*(Erin(i,j,g) - Erin(i-1,j,g))
          enddo
       enddo
    enddo

    do g = 0, ngroups-1
       do j = lo(2), hi(2)+1
          do i = lo(1), hi(1)
             div1 = HALF*(div(i,j) + div(i+1,j))
             div1 = difmag*min(ZERO, div1)

             rflux2(i,j,g) = rflux2(i,j,g) &
                  + dy*div1*(Erin(i,j,g) - Erin(i,j-1,g))
          enddo
       enddo
    enddo
#endif


    ! For hydro, we will create an update source term that is
    ! essentially the flux divergence.  This can be added with dt to
    ! get the update

    do n = 1, NVAR
       do j = lo(2), hi(2)
          do i = lo(1), hi(1)

             update(i,j,n) = update(i,j,n) + &
                  ( flux1(i,j,n) * area1(i,j) - flux1(i+1,j,n) * area1(i+1,j) + &
                    flux2(i,j,n) * area2(i,j) - flux2(i,j+1,n) * area2(i,j+1) ) / vol(i,j)

             if (n == UEINT) then
                ! Add p div(u) source term to (rho e)
                update(i,j,n) = update(i,j,n) - pdivu(i,j)
             endif

          enddo
       enddo
    enddo


#ifndef RADIATION
    ! Add gradp term to momentum equation -- only for axisymmetric
    ! coords (and only for the radial flux).

    if (.not. mom_flux_has_p(1)%comp(UMX)) then
       do j = lo(2), hi(2)
          do i = lo(1), hi(1)
             update(i,j,UMX) = update(i,j,UMX) - (q1(i+1,j,GDPRES) - q1(i,j,GDPRES)) / dx
             !update(i,j,UMY) = update(i,j,UMY) - (pgdy(i,j+1)-pgdy(i,j)) / dy
          enddo
       enddo
    endif

#else

    ! radiation energy update.  For the moment, we actually update things
    ! fully here, instead of creating a source term for the update
    do g = 0, ngroups-1
       do j = lo(2), hi(2)
          do i = lo(1), hi(1)
             Erout(i,j,g) = Erin(i,j,g) + dt * &
                  ( rflux1(i,j,g) * area1(i,j) - rflux1(i+1,j,g) * area1(i+1,j) + &
                    rflux2(i,j,g) * area2(i,j) - rflux2(i,j+1,g) * area2(i,j+1) ) / vol(i,j)
          enddo
       enddo
    end do


    ! Add gradp term to momentum equation -- only for axisymmetry coords
    ! (and only for the radial flux);  also add the radiation pressure gradient
    ! to the momentum for all directions

    do j = lo(2), hi(2)
       do i = lo(1), hi(1)

          ! pgdnv from the Riemann solver is only the gas contribution,
          ! not the radiation contribution.  Note that we've already included
          ! the gas pressure in the momentum flux for all Cartesian coordinate
          ! directions
          if (.not. mom_flux_has_p(1)%comp(UMX)) then
             dpdx = ( q1(i+1,j,GDPRES) - q1(i,j,GDPRES))/ dx
          else
             dpdx = ZERO
          endif
          dpdy = ZERO

          update(i,j,UMX) = update(i,j,UMX) - dpdx


          ! radiation contribution -- this is sum{lambda E_r}
          dprdx = ZERO
          dprdy = ZERO
          do g= 0, ngroups-1
             lamc = FOURTH*(q1(i,j,GDLAMS+g) + q1(i+1,j,GDLAMS+g) + &
                            q2(i,j,GDLAMS+g) + q2(i,j+1,GDLAMS+g))
             dprdx = dprdx + lamc*(q1(i+1,j,GDERADS+g) - q1(i,j,GDERADS+g))/dx
             dprdy = dprdy + lamc*(q2(i,j+1,GDERADS+g) - q2(i,j,GDERADS+g))/dy
          end do

          ! we now want to compute the change in kinetic energy -- we
          ! base this off of uout, since that has the source terms in it.
          ! But note that this does not have the fluxes (since we are
          ! using update)

          ! note, we need to include the Z component here too, since rotation
          ! might be in play

          urho_new = uout(i,j,URHO) + dt * update(i,j,URHO)

          ! this update includes the hydro fluxes and grad{p} from hydro
          umx_new1 = uout(i,j,UMX) + dt * update(i,j,UMX)
          umy_new1 = uout(i,j,UMY) + dt * update(i,j,UMY)
          umz_new1 = uout(i,j,UMZ) + dt * update(i,j,UMZ)

          ek1 = (umx_new1**2 + umy_new1**2 + umz_new1**2) / (TWO*urho_new)

          ! now add the radiation pressure gradient
          update(i,j,UMX) = update(i,j,UMX) - dprdx
          update(i,j,UMY) = update(i,j,UMY) - dprdy

          umx_new2 = umx_new1 - dt * dprdx
          umy_new2 = umy_new1 - dt * dprdy
          umz_new2 = umz_new1

          ek2 = (umx_new2**2 + umy_new2**2 + umz_new2**2) / (TWO*urho_new)

          dek = ek2 - ek1

          ! the update is a source / dt, so scale accordingly
          update(i,j,UEDEN) = update(i,j,UEDEN) + dek/dt

          if (.not. comoving) then ! mixed-frame (single group only)
             Erout(i,j,0) = Erout(i,j,0) - dek
          end if
       enddo
    enddo

    ! Add radiation source term to rho*u, rhoE, and Er
    if (comoving) then
       do j = lo(2), hi(2)
          do i = lo(1), hi(1)

             ux = HALF*(q1(i,j,GDU) + q1(i+1,j,GDU))
             uy = HALF*(q2(i,j,GDV) + q2(i,j+1,GDV))

             divu = (q1(i+1,j,GDU)*area1(i+1,j) - q1(i,j,GDU)*area1(i,j) + &
                     q2(i,j+1,GDV)*area2(i,j+1) - q2(i,j,GDV)*area2(i,j)) / vol(i,j)

             dudx(1) = (q1(i+1,j,GDU) - q1(i,j,GDU))/dx
             dudx(2) = (q1(i+1,j,GDV) - q1(i,j,GDV))/dx

             dudy(1) = (q2(i,j+1,GDU) - q2(i,j,GDU))/dy
             dudy(2) = (q2(i,j+1,GDV) - q2(i,j,GDV))/dy

             ! Note that for single group, fspace_type is always 1
             do g=0, ngroups-1

                nhat(1) = (q1(i+1,j,GDERADS+g) - q1(i,j,GDERADS+g))/dx
                nhat(2) = (q2(i,j+1,GDERADS+g) - q2(i,j,GDERADS+g))/dy

                GnDotu(1) = dot_product(nhat, dudx)
                GnDotu(2) = dot_product(nhat, dudy)

                nnColonDotGu = dot_product(nhat, GnDotu) / (dot_product(nhat,nhat)+1.e-50_rt)

                lamc = 0.25e0_rt*(q1(i,j,GDLAMS+g) + q1(i+1,j,GDLAMS+g) + &
                               q2(i,j,GDLAMS+g) + q2(i,j+1,GDLAMS+g))
                Eddf = Edd_factor(lamc)
                f1 = (ONE - Eddf)*HALF
                f2 = (3.e0_rt*Eddf - ONE)*HALF
                af(g) = -(f1*divu + f2*nnColonDotGu)

                if (fspace_type .eq. 1) then
                   Eddfxp = Edd_factor(q1(i+1,j  ,GDLAMS+g))
                   Eddfxm = Edd_factor(q1(i  ,j  ,GDLAMS+g))
                   Eddfyp = Edd_factor(q2(i  ,j+1,GDLAMS+g))
                   Eddfym = Edd_factor(q2(i  ,j  ,GDLAMS+g))

                   f1xp = HALF*(ONE-Eddfxp)
                   f1xm = HALF*(ONE-Eddfxm)
                   f1yp = HALF*(ONE-Eddfyp)
                   f1ym = HALF*(ONE-Eddfym)

                   Gf1E(1) = (f1xp*q1(i+1,j,GDERADS+g) - f1xm*q1(i,j,GDERADS+g)) / dx
                   Gf1E(2) = (f1yp*q2(i,j+1,GDERADS+g) - f1ym*q2(i,j,GDERADS+g)) / dy

                   Egdc = 0.25e0_rt*(q1(i,j,GDERADS+g) + q1(i+1,j,GDERADS+g) + &
                                  q2(i,j,GDERADS+g) + q2(i,j+1,GDERADS+g))

                   Erout(i,j,g) = Erout(i,j,g) + dt*(ux*Gf1E(1)+uy*Gf1E(2)) &
                        - dt*f2*Egdc*nnColonDotGu
                end if

             end do

             if (ngroups.gt.1) then
                ustar = Erout(i,j,:) / Erscale
                call advect_in_fspace(ustar, af, dt, nstep_fsp)
                Erout(i,j,:) = ustar * Erscale
             end if
          end do
       end do
    end if
#endif

    ! Scale the fluxes for the form we expect later in refluxing.

    do n = 1, NVAR
       do j = lo(2), hi(2)
          do i = lo(1), hi(1)+1

             flux1(i,j,n) = dt * flux1(i,j,n) * area1(i,j)

          enddo
       enddo
    enddo

    do n = 1, NVAR
       do j = lo(2), hi(2)+1
          do i = lo(1), hi(1)

             flux2(i,j,n) = dt * flux2(i,j,n) * area2(i,j)

          enddo
       enddo
    enddo

#ifdef RADIATION
    do g = 0, ngroups-1
       do j = lo(2), hi(2)
          do i = lo(1),hi(1)+1
             rflux1(i,j,g) = dt * rflux1(i,j,g) * area1(i,j)
          enddo
       enddo
    end do

    do g = 0, ngroups-1
       do j = lo(2), hi(2)+1
          do i = lo(1), hi(1)
             rflux2(i,j,g) = dt * rflux2(i,j,g) * area2(i,j)
          enddo
       enddo
    end do
#endif


    ! Add up some diagnostic quantities. Note that we are not dividing by the cell volume.

    if (track_grid_losses .eq. 1) then

       domlo = domlo_level(:,amr_level)
       domhi = domhi_level(:,amr_level)

       k = 0

       if (lo(2) .le. domlo(2) .and. hi(2) .ge. domlo(2)) then

          j = domlo(2)
          do i = lo(1), hi(1)

             loc = position(i,j,k,ccy=.false.)

             mass_lost = mass_lost - flux2(i,j,URHO)
             xmom_lost = xmom_lost - flux2(i,j,UMX)
             ymom_lost = ymom_lost - flux2(i,j,UMY)
             zmom_lost = zmom_lost - flux2(i,j,UMZ)
             eden_lost = eden_lost - flux2(i,j,UEDEN)

             ang_mom   = linear_to_angular_momentum(loc - center, flux2(i,j,UMX:UMZ))
             xang_lost = xang_lost - ang_mom(1)
             yang_lost = yang_lost - ang_mom(2)
             zang_lost = zang_lost - ang_mom(3)

          enddo

       endif

       if (lo(2) .le. domhi(2) .and. hi(2) .ge. domhi(2)) then

          j = domhi(2) + 1
          do i = lo(1), hi(1)

             loc = position(i,j,k,ccy=.false.)

             mass_lost = mass_lost + flux2(i,j,URHO)
             xmom_lost = xmom_lost + flux2(i,j,UMX)
             ymom_lost = ymom_lost + flux2(i,j,UMY)
             zmom_lost = zmom_lost + flux2(i,j,UMZ)
             eden_lost = eden_lost + flux2(i,j,UEDEN)

             ang_mom   = linear_to_angular_momentum(loc - center, flux2(i,j,UMX:UMZ))
             xang_lost = xang_lost + ang_mom(1)
             yang_lost = yang_lost + ang_mom(2)
             zang_lost = zang_lost + ang_mom(3)

          enddo

       endif

       if (lo(1) .le. domlo(1) .and. hi(1) .ge. domlo(1)) then

          i = domlo(1)
          do j = lo(2), hi(2)

             loc = position(i,j,k,ccx=.false.)

             mass_lost = mass_lost - flux1(i,j,URHO)
             xmom_lost = xmom_lost - flux1(i,j,UMX)
             ymom_lost = ymom_lost - flux1(i,j,UMY)
             zmom_lost = zmom_lost - flux1(i,j,UMZ)
             eden_lost = eden_lost - flux1(i,j,UEDEN)

             ang_mom   = linear_to_angular_momentum(loc - center, flux1(i,j,UMX:UMZ))
             xang_lost = xang_lost - ang_mom(1)
             yang_lost = yang_lost - ang_mom(2)
             zang_lost = zang_lost - ang_mom(3)

          enddo

       endif

       if (lo(1) .le. domhi(1) .and. hi(1) .ge. domhi(1)) then

          i = domhi(1) + 1
          do j = lo(2), hi(2)

             loc = position(i,j,k,ccx=.false.)

             mass_lost = mass_lost + flux1(i,j,URHO)
             xmom_lost = xmom_lost + flux1(i,j,UMX)
             ymom_lost = ymom_lost + flux1(i,j,UMY)
             zmom_lost = zmom_lost + flux1(i,j,UMZ)
             eden_lost = eden_lost + flux1(i,j,UEDEN)

             ang_mom   = linear_to_angular_momentum(loc - center, flux1(i,j,UMX:UMZ))
             xang_lost = xang_lost + ang_mom(1)
             yang_lost = yang_lost + ang_mom(2)
             zang_lost = zang_lost + ang_mom(3)

          enddo

       endif

    endif

  end subroutine consup

end module ctu_advection_module
