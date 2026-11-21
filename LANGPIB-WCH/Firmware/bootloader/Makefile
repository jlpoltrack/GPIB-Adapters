# LANGPIB-WCH DFU bootloader: 16 KB at 0x08000000, flashed over SDI once (then WRPR on sectors 0-3).
# Shares startup, trap entry, clocks and the USBHS driver with the app (../src).

PROJECT := boot
BUILD   := build
CROSS   ?= $(firstword $(wildcard $(HOME)/opt/xpack-riscv-none-elf-gcc-*/bin/))riscv-none-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
GDB     ?= riscv64-elf-gdb
PROBE   ?= /dev/cu.usbmodem0001
HOSTCC  ?= cc

TUSB := ../lib/tinyusb

SRC_C := boot.c image.c flash.c stage.c usb_desc.c \
	../src/clock.c ../src/trap.c ../src/dcd_ch32v407.c \
	$(TUSB)/src/tusb.c $(TUSB)/src/common/tusb_fifo.c $(TUSB)/src/device/usbd.c \
	$(TUSB)/src/class/dfu/dfu_device.c
SRC_S := ../src/startup.S

# this directory first: its board_hw.h and tusb_config.h, not the app's
INC := . ../src ../lib/wch ../../../gpib-core/src $(TUSB)/src

ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -msmall-data-limit=8

CFLAGS := $(ARCH) $(addprefix -I,$(INC)) -std=gnu11 -Os -g3 -ffunction-sections -fdata-sections \
	-Wall -Wextra -Werror -MMD -MP

LDFLAGS := $(ARCH) -T boot.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
	-Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

OBJ := $(patsubst %,$(BUILD)/obj/%.o,$(subst ../,_/,$(basename $(SRC_C) $(SRC_S))))

all: $(BUILD)/$(PROJECT).bin $(BUILD)/$(PROJECT).hex $(BUILD)/$(PROJECT).lst
	@$(SIZE) $(BUILD)/$(PROJECT).elf

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/_/%.o: ../%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/_/%.o: ../%.S
	@mkdir -p $(dir $@)
	@echo "AS  $<"
	@$(CC) $(ARCH) -c $< -o $@

$(BUILD)/$(PROJECT).elf: $(OBJ) boot.ld
	@echo "LD  $@"
	@$(CC) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	@$(OBJCOPY) -O binary $< $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	@$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.lst: $(BUILD)/%.elf
	@$(OBJDUMP) -d -S $< > $@

flash: all
	$(GDB) -batch -ex 'target extended-remote $(PROBE)' -ex 'monitor reset halt' \
	    -ex load -ex 'monitor reset run' -ex detach $(BUILD)/$(PROJECT).elf

# host tests: the image header, CRC and DFU block addressing against tools/mkdfu.py's output; the network update
# stage on a simulated flash with power cuts (two images: the running app, the update)
HOSTFLAGS := -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.

test:
	@mkdir -p $(BUILD)/test
	@$(HOSTCC) $(HOSTFLAGS) test/test_image.c image.c -o $(BUILD)/test/test_image
	@$(HOSTCC) $(HOSTFLAGS) test/test_stage.c stage.c image.c -o $(BUILD)/test/test_stage
	@head -c 70001 /dev/urandom > $(BUILD)/test/blob.bin
	@head -c 150001 /dev/urandom > $(BUILD)/test/blob2.bin
	@python3 ../tools/mkdfu.py $(BUILD)/test/blob.bin $(BUILD)/test/blob.dfu
	@python3 ../tools/mkdfu.py $(BUILD)/test/blob2.bin $(BUILD)/test/blob2.dfu
	@$(BUILD)/test/test_image $(BUILD)/test/blob.dfu
	@$(BUILD)/test/test_stage $(BUILD)/test/blob.dfu $(BUILD)/test/blob2.dfu

clean:
	rm -rf $(BUILD)

.PHONY: all flash test clean

-include $(OBJ:.o=.d)
