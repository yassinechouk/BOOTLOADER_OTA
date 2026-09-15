# ---------- Tools ----------
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# ---------- Project name ----------
TARGET  = bootloader

# ---------- Source files ----------
SRCS = main.c startup.s \
       ../drivers/crc.c ../drivers/flash.c ../drivers/metadata_mgr.c ../drivers/iwdg.c ../drivers/fault.c \
       src/uart.c src/protocol_mgr.c src/systick.c

# ---------- Compilation options ----------
CPU     = -mcpu=cortex-m4 -mthumb
FPU     = -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS  = $(CPU) $(FPU)
CFLAGS += -Wall -Wextra
CFLAGS += -O0 -g3
CFLAGS += -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I../shared

# make DEBUG=1 freezes the watchdog while the core is halted so that
# breakpoints do not reset the board. Never enable for a shipped image.
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG_BUILD
endif

# ---------- Linker options ----------
LDFLAGS  = $(CPU) $(FPU)
LDFLAGS += -T linker.ld
LDFLAGS += -nostdlib
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(TARGET).map

# ---------- Rules ----------
all: $(TARGET).bin

$(TARGET).elf: $(SRCS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
flash: $(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program $(TARGET).elf verify reset exit"

clean:
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map

.PHONY: all flash clean