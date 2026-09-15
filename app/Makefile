# =========================================================
# Application — producing both binaries
#
# A compiled binary is linked to a fixed address. The dual-slot
# architecture therefore requires compiling it twice, once per
# slot: app_slotA.bin only works at 0x08008000,
# app_slotB.bin only at 0x08080000.
#
# The PC tool queries the board for the free slot, then sends
# the corresponding binary. Only one file crosses the serial
# link; both exist only on disk.
# =========================================================

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

SRCS    = main.c startup.s \
          ../drivers/crc.c ../drivers/flash.c ../drivers/metadata_mgr.c ../drivers/iwdg.c

CPU     = -mcpu=cortex-m4 -mthumb
FPU     = -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS  = $(CPU) $(FPU)
CFLAGS += -Wall -Wextra
CFLAGS += -O0 -g3
CFLAGS += -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I../shared

LDBASE  = $(CPU) $(FPU)
LDBASE += -nostdlib
LDBASE += -Wl,--gc-sections

# ---------------------------------------------------------
all: app_slotA.bin app_slotB.bin
	@echo ""
	@echo "  Two binaries produced:"
	@ls -l app_slotA.bin app_slotB.bin | awk '{printf "    %-16s %6s bytes\n", $$9, $$5}'
	@echo ""

# --- Slot A ---
app_slotA.elf: $(SRCS) linker_slotA.ld
	$(CC) $(CFLAGS) $(LDBASE) -T linker_slotA.ld \
	      -Wl,-Map=app_slotA.map $(SRCS) -o $@
	@$(SIZE) $@

app_slotA.bin: app_slotA.elf
	$(OBJCOPY) -O binary $< $@

# --- Slot B ---
app_slotB.elf: $(SRCS) linker_slotB.ld
	$(CC) $(CFLAGS) $(LDBASE) -T linker_slotB.ld \
	      -Wl,-Map=app_slotB.map $(SRCS) -o $@
	@$(SIZE) $@

app_slotB.bin: app_slotB.elf
	$(OBJCOPY) -O binary $< $@

# ---------------------------------------------------------
# Verification: both binaries must be linked at different
# addresses. An error here would otherwise go unnoticed until
# the application silently crashes.
verify: app_slotA.elf app_slotB.elf
	@echo ""
	@echo "  Link addresses:"
	@arm-none-eabi-objdump -h app_slotA.elf | \
	   awk '/\.isr_vector/{printf "    slot A : 0x%s  (expected 08008000)\n", $$4}'
	@arm-none-eabi-objdump -h app_slotB.elf | \
	   awk '/\.isr_vector/{printf "    slot B : 0x%s  (expected 08080000)\n", $$4}'
	@echo ""

# ---------------------------------------------------------
# Direct SWD flash, useful for bootstrapping a slot without
# going through the bootloader.
flashA: app_slotA.elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program app_slotA.elf verify reset exit"

flashB: app_slotB.elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program app_slotB.elf verify reset exit"

clean:
	rm -f app_slotA.elf app_slotA.bin app_slotA.map
	rm -f app_slotB.elf app_slotB.bin app_slotB.map

.PHONY: all verify flashA flashB clean
