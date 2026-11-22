# LANGPIB-WCH firmware (CH32V407RET6). Toolchain: xPack riscv-none-elf-gcc (newlib-nano); Homebrew's riscv64-elf-gcc
# has no C library. The app sits behind the DFU bootloader (bootloader/): APP_ORIGIN=0x08000000 builds it standalone.

PROJECT    := langpib
SIM        ?= 0 # 1: gpib-core's simulated instrument instead of the GPIB lines, to test USB and LAN with no GPIB plug
BUILD      := $(if $(filter 1,$(SIM)),build-sim,build)
CROSS      ?= $(firstword $(wildcard $(HOME)/opt/xpack-riscv-none-elf-gcc-*/bin/))riscv-none-elf-
CC         := $(CROSS)gcc
OBJCOPY    := $(CROSS)objcopy
OBJDUMP    := $(CROSS)objdump
SIZE       := $(CROSS)size
GDB        ?= riscv64-elf-gdb
PROBE      ?= /dev/cu.usbmodem0001 # RP2350-Zero SDI probe, GDB port (WCH repo programmer/)
APP_ORIGIN ?= 0x08004100
APP_VID    := $(shell sed -n 's/.*APP_USB_VID *0x\([0-9A-Fa-f]*\).*/\1/p' src/app_config.h)
APP_PID    := $(shell sed -n 's/.*APP_USB_PID *0x\([0-9A-Fa-f]*\).*/\1/p' src/app_config.h)
APP_BCD    := $(shell sed -n 's/.*APP_USB_BCD *\(0x[0-9A-Fa-f]*\).*/\1/p' src/app_config.h)

BOOT_BIN  := bootloader/build/boot.bin
TUSB      := lib/tinyusb
LWIP      := lib/lwip
GPIB_CORE := ../../gpib-core
include $(GPIB_CORE)/gpib-core.mk

LWIP_SRC := $(addprefix $(LWIP)/src/, \
	core/init.c core/def.c core/inet_chksum.c core/ip.c core/mem.c core/memp.c core/netif.c core/pbuf.c \
	core/raw.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/acd.c core/ipv4/autoip.c core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
	core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c core/ipv4/igmp.c netif/ethernet.c \
	apps/mdns/mdns.c apps/mdns/mdns_out.c apps/mdns/mdns_domain.c)

SRC_C := \
	src/main.c src/board.c src/boot_image.c src/clock.c src/trap.c src/net.c src/eth.c src/dcd_ch32v407.c src/pintest.c \
	src/update.c bootloader/stage.c bootloader/image.c bootloader/flash.c \
	$(GPIB_CORE_SRC) $(GPIB_CORE_VXI_SRC) $(GPIB_CORE_LWIP_SRC) \
	$(TUSB)/src/tusb.c \
	$(TUSB)/src/common/tusb_fifo.c \
	$(TUSB)/src/device/usbd.c \
	$(TUSB)/src/class/cdc/cdc_device.c \
	$(TUSB)/src/class/dfu/dfu_rt_device.c \
	$(LWIP_SRC)

SRC_S := src/startup.S

# bootloader/ last: its stage writer and flash code, not its board_hw.h or tusb_config.h
INC := src lib/wch $(GPIB_CORE_INC) $(TUSB)/src $(LWIP)/src/include bootloader

ifeq ($(SIM),1)
SRC_C += $(GPIB_CORE_SIM_SRC)
INC   += $(GPIB_CORE_SIM_INC)
DEFS  := -DGPIB_SIM -DSIM_MAX_DEVS=1
endif

# The core is rv32imac_zba_zbb_zbs_zve64x_zvbb; newlib-nano's multilib is plain rv32imac.
ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -msmall-data-limit=8

CFLAGS := $(ARCH) $(addprefix -I,$(INC)) \
	-std=gnu11 -Os -g3 -ffunction-sections -fdata-sections \
	-Wall -Wextra -Werror -MMD -MP $(DEFS)

LDFLAGS := $(ARCH) -T ch32v407_app.ld -Wl,--defsym=APP_ORIGIN=$(APP_ORIGIN) -nostartfiles -Wl,--gc-sections \
	-Wl,-Map=$(BUILD)/$(PROJECT).map -Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

# ../ becomes _/ so gpib-core's objects stay inside $(BUILD) (each build keeps its own)
OBJ := $(addprefix $(BUILD)/,$(subst ../,_/,$(SRC_C:.c=.o) $(SRC_S:.S=.o)))

all: $(BUILD)/$(PROJECT).dfu $(BUILD)/$(PROJECT).hex $(BUILD)/$(PROJECT).lst
	@$(SIZE) $(BUILD)/$(PROJECT).elf

# The app embeds the bootloader (src/boot_image.c), so that is built first and the app relinks when it changes
$(BOOT_BIN): FORCE
	@$(MAKE) -s -C bootloader

$(BUILD)/src/boot_image.o: CFLAGS += -DBOOT_BIN='"$(abspath $(BOOT_BIN))"'
$(BUILD)/src/boot_image.o: $(BOOT_BIN)

# .dfu for dfu-util; -sdi.hex is header + image at 0x08004000 for the probe
$(BUILD)/$(PROJECT).dfu: $(BUILD)/$(PROJECT).bin tools/mkdfu.py
	@python3 tools/mkdfu.py $< $@ --hex $(BUILD)/$(PROJECT)-sdi.hex --version $(APP_BCD)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/_/_/%.o: ../../%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS  $<"
	@$(CC) $(ARCH) -c $< -o $@

$(BUILD)/$(PROJECT).elf: $(OBJ) ch32v407_app.ld
	@echo "LD  $@"
	@$(CC) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	@$(OBJCOPY) -O binary $< $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	@$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.lst: $(BUILD)/%.elf
	@$(OBJDUMP) -d -S $< > $@

# SDI through the RP2350-Zero probe's GDB server: the bootloader, then header + app (a blank chip needs both)
flash: all
	$(GDB) -batch -ex 'target extended-remote $(PROBE)' -ex 'monitor reset halt' \
	    -ex 'load bootloader/build/boot.elf' -ex 'load $(BUILD)/$(PROJECT)-sdi.hex' \
	    -ex 'monitor reset run' -ex detach

# Production: mass erase, bootloader + app over the probe, boot log checked on its UART, a row in
# docs/production-log.csv (tools/production.py; ARGS=--keep-settings re-flashes without the erase)
production: all
	@test "$(SIM)" != 1 || { echo "production flashes the real firmware: no SIM=1"; exit 1; }
	python3 tools/production.py $(ARGS)

# Bring-up checks (tools/bringup.py): STAGE=flash|log|usb|usbtmc|net|all, HOST= for net; SIM=1 flashes and tests build-sim
bringup: all
	python3 tools/bringup.py $(or $(STAGE),all) $(HOST) $(if $(filter 1,$(SIM)),--sim)

# USB: dfu-util detaches the running app (DFU runtime) and downloads to the bootloader (same VID:PID); no -R: the
# bootloader resets itself 3 s after a good download, and a host-side reset fails on macOS as it re-enumerates
dfu: all
	dfu-util -w -d $(APP_VID):$(APP_PID) -a 0 -D $(BUILD)/$(PROJECT).dfu

# Network: POST the same .dfu to the web server (gpib-core http.c, src/update.c); PASSWORD only if one was set over
# USB with '++net password' (none: open). 'Expect:' is cleared: the server doesn't answer 100-continue. The password isn't echoed.
netdfu: all
	@test -n "$(HOST)" || { echo "make netdfu HOST=<ip or name> [PASSWORD=<update password>]"; exit 1; }
	@echo "curl -> http://$(HOST)/update"
	@curl -sS --fail-with-body -H 'Expect:' -H 'Content-Type: application/octet-stream' \
	    $(if $(PASSWORD),-H "X-Update-Password: $(PASSWORD)") --data-binary @$(BUILD)/$(PROJECT).dfu http://$(HOST)/update

# clangd: the same flags for editor diagnostics
compile_flags.txt: Makefile
	@printf '%s\n' --target=riscv32-unknown-elf $(addprefix -I,$(INC)) -std=gnu11 > $@

# --- Host tests (gpib-core, bootloader image) and the VXI-11 server (gpib-core) ---------------------------------
test:
	$(MAKE) -C $(GPIB_CORE) test
	$(MAKE) -C bootloader test

host:
	$(MAKE) -C $(GPIB_CORE) host

clean:
	rm -rf build build-sim
	$(MAKE) -C bootloader clean

.PHONY: all flash production bringup dfu netdfu test host clean FORCE

-include $(OBJ:.o=.d)
