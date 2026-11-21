# UsbGpib STM32F103 application firmware (runs behind dapboot at 0x08002000)

PROJECT   := usbgpib
BUILD     := build
# BENCH=1: USB without an instrument, for a board without J2 (separate objects, never ships)
ifdef BENCH
BUILD     := build/bench
endif
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

TUSB      := lib/tinyusb
CMSIS_DEV := lib/cmsis_device_f1
CMSIS     := lib/CMSIS_6
DAPBOOT   := ../bootloader/dapboot/src
DAPBOOT_BIN := $(DAPBOOT)/dapboot.bin
GPIB_CORE := ../../gpib-core
include $(GPIB_CORE)/gpib-core.mk

SRC_C := \
	src/main.c src/board.c src/dapboot_image.c \
	$(GPIB_CORE_SRC) \
	$(CMSIS_DEV)/Source/Templates/system_stm32f1xx.c \
	$(TUSB)/src/tusb.c \
	$(TUSB)/src/common/tusb_fifo.c \
	$(TUSB)/src/device/usbd.c \
	$(TUSB)/src/class/cdc/cdc_device.c \
	$(TUSB)/src/class/dfu/dfu_rt_device.c \
	$(TUSB)/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c \
	$(TUSB)/src/portable/st/stm32_fsdev/fsdev_common.c

SRC_S := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32f103xb.s

INC := src $(GPIB_CORE_INC) $(TUSB)/src $(CMSIS_DEV)/Include $(CMSIS)/CMSIS/Core/Include

ARCH := -mcpu=cortex-m3 -mthumb

# 8 for dev boards with an 8 MHz crystal (make clean after changing it)
HSE_MHZ ?= 16

DEFS := \
	-DSTM32F103xB \
	-DHSE_MHZ=$(HSE_MHZ) \
	-DCFG_TUSB_MCU=OPT_MCU_STM32F1 \
	-DUSER_VECT_TAB_ADDRESS \
	-DVECT_TAB_OFFSET=0x2000U \
	-DDAPBOOT_BIN='"$(abspath $(DAPBOOT_BIN))"' \
	$(if $(BENCH),-DBENCH_NO_BUS)

CFLAGS := $(ARCH) $(DEFS) $(addprefix -I,$(INC)) \
	-std=gnu11 -Os -g3 -ffunction-sections -fdata-sections \
	-Wall -Wextra -MMD -MP

# Shared by the app and bring-up links; each rule names its own linker script and map
LDFLAGS_COMMON := $(ARCH) -Wl,--gc-sections -Wl,--print-memory-usage --specs=nano.specs

# ../ becomes _/ so gpib-core's objects stay inside $(BUILD) (each build keeps its own)
OBJ := $(addprefix $(BUILD)/,$(subst ../,_/,$(SRC_C:.c=.o) $(SRC_S:.s=.o)))

all: $(BUILD)/$(PROJECT).bin $(BUILD)/$(PROJECT).hex
	@$(SIZE) $(BUILD)/$(PROJECT).elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/_/_/%.o: ../../%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	@echo "AS  $<"
	@$(CC) $(ARCH) -c $< -o $@

$(BUILD)/$(PROJECT).elf: $(OBJ) stm32f103x8_app.ld stm32f103x8_common.ld
	@echo "LD  $@"
	@$(CC) $(LDFLAGS_COMMON) -T stm32f103x8_app.ld -Wl,-Map=$(BUILD)/$(PROJECT).map $(OBJ) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	@$(OBJCOPY) -O binary $< $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	@$(OBJCOPY) -O ihex $< $@

# --- Bootloader -------------------------------------------------------------
# Generic F103 target with this board's LED (PC13, sinks) and DFU button (PA1 to GND, internal pull-up).
# DEFS goes in via the environment: dapboot appends its own (-DSTM32F1) with +=, a command-line DEFS would replace them.
# src/dapboot_clock.h swaps dapboot's hard-coded 8 MHz clock call for this board's HSE_MHZ.
# dapboot doesn't track DEFS: make -C $(DAPBOOT) clean after changing HSE_MHZ.
BOOT_DEFS := -DHAVE_LED=1 -DLED_GPIO_PORT=GPIOC -DLED_GPIO_PIN=GPIO13 -DLED_OPEN_DRAIN=1 -DUSES_GPIOC=1 \
             -DHAVE_BUTTON=1 -DBUTTON_GPIO_PORT=GPIOA -DBUTTON_GPIO_PIN=GPIO1 -DBUTTON_ACTIVE_HIGH=0 -DBUTTON_USES_PULL=1 \
             -DDAPBOOT_HSE_CONFIG=RCC_CLOCK_HSE$(HSE_MHZ)_72MHZ -include $(abspath src/dapboot_clock.h)
# The app embeds dapboot.bin (src/dapboot_image.c), so it is built first and the app relinks when it changes
$(DAPBOOT_BIN): $(wildcard $(DAPBOOT)/*.c $(DAPBOOT)/*.h $(DAPBOOT)/stm32f103/*.c $(DAPBOOT)/stm32f103/*/*.h) src/dapboot_clock.h
	DEFS='$(BOOT_DEFS)' $(MAKE) -C $(DAPBOOT) TARGET=STM32F103

bootloader: $(DAPBOOT_BIN)

$(BUILD)/src/dapboot_image.o: $(DAPBOOT_BIN)

# --- Programming ------------------------------------------------------------
# PROG=pyocd (default) or PROG=openocd; both through an ST-Link on J4
PROG    ?= pyocd
OPENOCD := openocd -f interface/stlink.cfg -f target/stm32f1x.cfg
PYOCD   := pyocd flash -t stm32f103c8
ifeq ($(PROG),pyocd)
program = $(PYOCD) --base-address $(2) $(1)
else
program = $(OPENOCD) -c "program $(1) $(2) verify reset exit"
endif

# First-time setup over SWD: bootloader + app
flash-all: all
	$(call program,$(DAPBOOT_BIN),0x08000000)
	$(call program,$(BUILD)/$(PROJECT).bin,0x08002000)

flash: all
	$(call program,$(BUILD)/$(PROJECT).bin,0x08002000)

# --- Bring-up image ---------------------------------------------------------
# A bare F103 on SWD (no crystal, USB or transceivers): Prologix on an RTT console, no dapboot. tools/bringup.py
BU_BUILD := $(BUILD)/bringup
BU_SRC_C := bringup/bringup.c bringup/port_rtt.c src/board.c src/rtt.c \
	$(addprefix $(GPIB_CORE)/src/,settings.c gpib_phy.c gpib_bus.c prologix.c netcfg.c trace.c) \
	$(CMSIS_DEV)/Source/Templates/system_stm32f1xx.c
BU_OBJ   := $(addprefix $(BU_BUILD)/,$(subst ../,_/,$(BU_SRC_C:.c=.o) $(SRC_S:.s=.o)))
BU_CFLAGS := $(filter-out -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=%,$(CFLAGS)) -DBOARD_IO_DEFERRED

bringup: $(BU_BUILD)/bringup.bin
	@$(SIZE) $(BU_BUILD)/bringup.elf

$(BU_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(BU_CFLAGS) -c $< -o $@

$(BU_BUILD)/_/_/%.o: ../../%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(BU_CFLAGS) -c $< -o $@

$(BU_BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	@$(CC) $(ARCH) -c $< -o $@

$(BU_BUILD)/bringup.elf: $(BU_OBJ) stm32f103x8_bringup.ld stm32f103x8_common.ld
	@echo "LD  $@"
	@$(CC) $(LDFLAGS_COMMON) -T stm32f103x8_bringup.ld -Wl,-Map=$(BU_BUILD)/bringup.map $(BU_OBJ) -o $@

$(BU_BUILD)/bringup.bin: $(BU_BUILD)/bringup.elf
	@$(OBJCOPY) -O binary $< $@

flash-bringup: bringup
	$(call program,$(BU_BUILD)/bringup.bin,0x08000000)

# Update over USB: dfu-util detaches the app (1209:0001, DFU runtime) and waits for dapboot (1209:db42).
dfu: all
	dfu-util -w -d 1209:0001,1209:db42 -a 0 -D $(BUILD)/$(PROJECT).bin

# --- Host tests: in gpib-core -------------------------------------------------------------------------------
test:
	$(MAKE) -C $(GPIB_CORE) $@

clean:
	rm -rf $(BUILD)

.PHONY: all bootloader flash flash-all flash-bringup bringup dfu test clean

-include $(OBJ:.o=.d) $(BU_OBJ:.o=.d)
