# Included by a board's Makefile. GPIB_CORE is where this folder sits (default: ../../gpib-core from <Board>/Firmware).
# The board's own src/ must come first on the include path: board_hw.h, gpib_pins.h, app_config.h, tusb_config.h.

GPIB_CORE ?= ../../gpib-core

GPIB_CORE_SRC := $(addprefix $(GPIB_CORE)/src/, \
	gpib_bus.c gpib_phy.c instr.c miniparser.c netcfg.c prologix.c settings.c trace.c \
	usbtmc.c usb_descriptors.c cdc_port.c)
GPIB_CORE_VXI_SRC := $(GPIB_CORE)/src/vxi11.c
GPIB_CORE_LWIP_SRC := $(addprefix $(GPIB_CORE)/src/, vxi11_lwip.c sockets_lwip.c scpi_raw.c http.c mdns_lwip.c) # VXI-11, TCP 5025, 1234 and 80 on lwIP; the board supplies lwipopts.h
GPIB_CORE_INC := $(GPIB_CORE)/src
GPIB_CORE_SIM_SRC := $(addprefix $(GPIB_CORE)/sim/, gpib_sim.c sim_scpi.c) # SIM=1: a simulated instrument, no GPIB plug
GPIB_CORE_SIM_INC := $(GPIB_CORE)/sim
