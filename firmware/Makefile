# RongYuan RY5088 (Artery AT32F405) — profile-driven keyboard firmware.
#
# One shared firmware in common/ is specialised per board by a profile in
# profiles/<dev_id>.toml. gen/gen_profile.py turns the profile into the
# board_config.h / board_keymap.h / board_led_map.c that common/ compiles
# against (written to build/<dev_id>/).
#
#   make PROFILE=2307           -> build/2307/ry5088_2307.bin (+ vector-table check)
#   make test                   -> host self-tests (native gcc; no BSP needed)
#   make check                  -> validate every profile through the generator
#   make models                 -> print the dev_id <-> name table
#   make flash-romdfu PROFILE=2307  -> dfu-util via AT32 ROM-DFU, app slot only
#   make clean
#
# The ARM build needs the Artery AT32F402_405 Firmware Library (BSP), fetched
# separately into vendor/ (default BSP_DIR below). See README.md.

PROFILE ?= 2307

SRC     = common
GEN     = gen/gen_profile.py
GENDIR  = build/$(PROFILE)
OBJ     = $(GENDIR)/obj
TARGET  = $(GENDIR)/ry5088_$(PROFILE)

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# ---- Artery BSP location (override with: make BSP_DIR=/path/to/bsp) ----
BSP_DIR ?= vendor/AT32F402_405_Firmware_Library
CMSIS   = $(BSP_DIR)/libraries/cmsis/cm4
DRV     = $(BSP_DIR)/libraries/drivers
USBDRV  = $(BSP_DIR)/middlewares/usb_drivers

INCS = -I$(SRC) -I$(GENDIR) \
       -I$(CMSIS)/core_support \
       -I$(CMSIS)/device_support \
       -I$(DRV)/inc \
       -I$(USBDRV)/inc

# Stock RY5088 bootloader + app drive OTGHS (0x40040000; enumerates high-speed), never OTGFS1 —
# OTGFS1 (PA11/PA12) is not wired to the USB-C port, so an OTG_FS build never enumerates.
USB_CORE ?= USB_OTG_HS
DEFS = -DAT32F405RCT7 -DUSE_STDPERIPH_DRIVER -D$(USB_CORE) $(EXTRA_DEFS)   # e.g. EXTRA_DEFS=-DDIAG_NO_HID

ARCH    = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS  = $(ARCH) $(DEFS) $(INCS) -ffunction-sections -fdata-sections -Os -g3 -Wall
LDFLAGS = $(ARCH) -specs=nano.specs -specs=nosys.specs -T $(SRC)/at32f405_app.ld \
          -Wl,--gc-sections -Wl,-Map=$(TARGET).map,--print-memory-usage \
          -Wl,--no-warn-rwx-segments

# ---- sources ----
# application engine + USB stack + vendor protocol (shared across all boards).
# NB: led_layout.c is NOT here — the LED map is the generated board_led_map.c.
APP_C = main.c keyscan.c hall.c hid_report.c vendor_proto.c monsgeek_class.c \
        monsgeek_desc.c descriptors.c board.c led_effects.c rgb.c \
        persist.c flash_efc.c wireless.c
# system / interrupt glue derived from the Artery BSP templates.
SYS_C = at32f402_405_int.c at32f402_405_clock.c system_at32f402_405.c
# Artery BSP drivers + USB device stack (compiled from the fetched BSP tree).
BSP_C = at32f402_405_crm.c at32f402_405_gpio.c at32f402_405_misc.c \
        at32f402_405_pwc.c at32f402_405_flash.c at32f402_405_usb.c \
        at32f402_405_adc.c at32f402_405_spi.c at32f402_405_dma.c \
        usb_core.c usbd_core.c usbd_int.c usbd_sdr.c
LOCAL_S = startup_at32f402_405.s

# the generated LED map is compiled from the per-profile build dir.
GEN_OBJS = $(OBJ)/board_led_map.o
OBJS = $(addprefix $(OBJ)/,$(APP_C:.c=.o) $(SYS_C:.c=.o) $(BSP_C:.c=.o) $(LOCAL_S:.s=.o)) \
       $(GEN_OBJS)

vpath %.c $(SRC):$(DRV)/src:$(USBDRV)/src
vpath %.s $(SRC)

all: $(TARGET).bin

# ---- code generation (one profile -> board_config.h / _keymap.h / _led_map.c) --
$(GENDIR)/.gen: profiles/$(PROFILE).toml $(GEN)
	@mkdir -p $(GENDIR)
	python3 $(GEN) profiles/$(PROFILE).toml $(GENDIR)
	@touch $@

$(GENDIR)/board_config.h $(GENDIR)/board_keymap.h $(GENDIR)/board_led_map.c: $(GENDIR)/.gen

$(OBJ):
	@mkdir -p $(OBJ)

# objects depend on generation NORMALLY, so changing the profile regenerates the headers
# and rebuilds every object (board_config.h is included widely). $(OBJ) stays order-only.
$(OBJS): $(GENDIR)/.gen
$(OBJS): | $(OBJ)

$(OBJ)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: $(GENDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary -S $< $@
	@echo "--- vector table check (app slot @0x08005200) ---"
	@printf 'initial SP / reset vector (first 2 words of .bin): '
	@od -An -tx4 -N8 $(TARGET).bin
	@echo "expect SP=20019800, reset in 08005201..08025800 (thumb bit => odd)"
	@echo "image $(TARGET).bin built (load @0x08005200 via RY DFU / ROM-DFU)"

# ---- host self-tests (pure logic; native cc; needs the generated headers) ----
TEST_CFLAGS = -std=gnu11 -Wall -Wextra -I$(SRC) -I$(GENDIR)
test: $(GENDIR)/.gen
	cc $(TEST_CFLAGS) $(SRC)/test/test_engine.c $(SRC)/hall.c $(SRC)/hid_report.c -o $(GENDIR)/test_engine
	@$(GENDIR)/test_engine
	cc $(TEST_CFLAGS) $(SRC)/test/test_functional.c $(SRC)/vendor_proto.c $(SRC)/led_effects.c \
	   $(GENDIR)/board_led_map.c $(SRC)/persist.c -o $(GENDIR)/test_functional
	@$(GENDIR)/test_functional

test-wireless: $(GENDIR)/.gen
	cc $(TEST_CFLAGS) -DHOST_TEST $(SRC)/test/test_wireless.c $(SRC)/wireless.c -o $(GENDIR)/test_wireless
	@$(GENDIR)/test_wireless

# ---- profile lint: run every profile through the generator (validates it) ----
check:
	@ok=1; for p in profiles/*.toml; do \
	  id=$$(basename $$p .toml); \
	  python3 $(GEN) $$p build/.check/$$id >/dev/null && echo "ok    $$p" || { echo "FAIL  $$p"; ok=0; }; \
	done; rm -rf build/.check; [ $$ok = 1 ]

# ---- print the dev_id <-> display_name table from the profiles ----
models:
	@python3 gen/models_table.py

flash-romdfu: $(TARGET).bin
	dfu-util -a 0 -d 2E3C:DF11 -s 0x08005200:leave -D $(TARGET).bin

clean:
	rm -rf build

.PHONY: all test test-wireless check models flash-romdfu clean
