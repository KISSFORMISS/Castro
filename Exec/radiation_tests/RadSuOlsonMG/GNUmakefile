COMP	   = gnu

DIM        = 1

USE_MPI    = TRUE
DEBUG      = FALSE

PROFILE    = FALSE

USE_RAD = TRUE
USE_NEUTRINO = FALSE

CASTRO_HOME = ../../..

EOS_dir     := gamma_law

Network_dir := general_null
NETWORK_INPUTS := gammalaw.net

Opacity_dir := null

Bpack   := ./Make.package
Blocs   := .

include $(CASTRO_HOME)/Exec/Make.Castro

#F90FLAGS += -x f95-cpp-input -D MG_SU_OLSON

CPPFLAGS += -DMG_SU_OLSON 
