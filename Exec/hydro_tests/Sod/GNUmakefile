PRECISION  = DOUBLE
PROFILE    = FALSE

DEBUG      = TRUE


DIM        = 1
#DIM        = 2
#DIM        = 3

COMP	   = gnu

USE_MPI    = FALSE
USE_OMP    = FALSE

# define the location of the CASTRO top directory
CASTRO_HOME  := ../../..

# This sets the EOS directory in Castro/EOS
EOS_dir     := gamma_law

# This sets the network directory in Castro/Networks
Network_dir := general_null
GENERAL_NET_INPUTS = $(CASTRO_HOME)/Microphysics/networks/$(Network_dir)/gammalaw.net

Bpack   := ./Make.package
Blocs   := .

include $(CASTRO_HOME)/Exec/Make.Castro
