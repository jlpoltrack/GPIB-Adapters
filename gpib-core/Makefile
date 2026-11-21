# gpib-core: host unit tests and the host VXI-11 server. Boards build the sources through gpib-core.mk.

BUILD  := build
HOSTCC ?= cc
FLAGS  := -std=gnu11 -Wall -Wextra -Werror -fno-common -DGPIB_SIM -Itest -Isim -Isrc
TFLAGS := $(FLAGS) -fsanitize=address,undefined
SIM    := sim/gpib_sim.c test/sim_host.c src/gpib_phy.c src/gpib_bus.c

test:
	@mkdir -p $(BUILD)/test
	@$(HOSTCC) $(TFLAGS) test/test_settings.c src/settings.c -o $(BUILD)/test/test_settings
	@$(BUILD)/test/test_settings
	@$(HOSTCC) $(TFLAGS) -DBOARD_FLASH_ERASED=0xE339E339u test/test_settings.c src/settings.c -o $(BUILD)/test/test_settings_wch
	@$(BUILD)/test/test_settings_wch
	@$(HOSTCC) $(TFLAGS) test/test_bus.c $(SIM) -o $(BUILD)/test/test_bus
	@$(BUILD)/test/test_bus
	@$(HOSTCC) $(TFLAGS) test/test_prologix.c src/prologix.c src/netcfg.c src/settings.c $(SIM) -o $(BUILD)/test/test_prologix
	@$(BUILD)/test/test_prologix
	@$(HOSTCC) $(TFLAGS) test/test_instr.c src/instr.c src/miniparser.c src/netcfg.c src/settings.c $(SIM) -o $(BUILD)/test/test_instr
	@$(BUILD)/test/test_instr
	@$(HOSTCC) $(TFLAGS) test/test_vxi11.c src/vxi11.c $(SIM) -o $(BUILD)/test/test_vxi11
	@$(BUILD)/test/test_vxi11
	@$(HOSTCC) $(TFLAGS) test/test_scpi_raw.c src/scpi_raw.c $(SIM) -o $(BUILD)/test/test_scpi_raw
	@$(BUILD)/test/test_scpi_raw
	@$(HOSTCC) $(TFLAGS) test/test_netcfg.c src/netcfg.c src/instr.c src/miniparser.c src/settings.c $(SIM) \
		-o $(BUILD)/test/test_netcfg
	@$(BUILD)/test/test_netcfg
	@$(HOSTCC) $(TFLAGS) test/test_http.c src/http.c src/netcfg.c src/settings.c -o $(BUILD)/test/test_http
	@$(BUILD)/test/test_http

# VXI-11 server with a simulated instrument; timeouts on the wall clock (SIM_WALL_CLOCK)
host:
	@mkdir -p $(BUILD)/host
	@$(HOSTCC) $(FLAGS) -O1 -g -DSIM_WALL_CLOCK host/vxi11_host.c sim/sim_scpi.c src/vxi11.c src/instr.c src/miniparser.c \
		src/settings.c src/scpi_raw.c src/prologix.c src/netcfg.c src/http.c $(SIM) -o $(BUILD)/host/vxi11_host

clean:
	rm -rf $(BUILD)

.PHONY: test host clean
