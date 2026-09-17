# Builds the real firmware logic for the host, so the C can be tested
# off-target. metadata_mgr.c and protocol_mgr.c come from their normal
# locations; only the hardware drivers are replaced.

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O1 -g -fPIC
CFLAGS += -I../../shared

# The firmware casts uint32_t addresses to pointers, which is exact on
# a 32-bit target and merely widening here. Silenced deliberately: the
# alternative is editing the code under test. Struct sizes are asserted
# at runtime instead, which is the property that actually matters.
CFLAGS += -Wno-int-to-pointer-cast

# Under test, unmodified.
FIRMWARE = ../../drivers/metadata_mgr.c ../../bootloader/src/protocol_mgr.c

# Host stand-ins for the register-level drivers.
HARNESS  = fake_hw.c shim.c

all: libfirmware.so

libfirmware.so: $(FIRMWARE) $(HARNESS)
	$(CC) $(CFLAGS) -shared -o $@ $(FIRMWARE) $(HARNESS)

test: libfirmware.so
	python3 test_c_firmware.py

clean:
	rm -f libfirmware.so

.PHONY: all test clean
