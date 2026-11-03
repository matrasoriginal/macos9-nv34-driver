# NV34 QD Accel — PPC fragment (nv34qd_install) + 68k INIT stub, packaged as an 'INIT' file.
RETRO68    ?= $(HOME)/Retro68-build/toolchain
RETRO68K   ?= $(HOME)/Retro68-build-68k/toolchain
CC         := $(RETRO68)/bin/powerpc-apple-macos-gcc
MAKEPEF    := $(RETRO68)/bin/MakePEF

CFLAGS  := -O2 -Wall -Wno-pointer-sign -Wno-multichar -ffreestanding -fno-builtin \
           -fno-tree-loop-distribute-patterns
LDFLAGS := -nostartfiles -nostdlib -Wl,-bE:exports.txt -Wl,-bnoentry
LIBS    := -lDriverServicesLib -lNameRegistryLib -lInterfaceLib

CC68K   := $(RETRO68K)/bin/m68k-apple-macos-gcc
REZ     := $(RETRO68K)/bin/Rez

all: build/NV34QD-INIT.bin

build/init68k.flt: init68k.c | build
	$(CC68K) -O2 -Wall -Wno-multichar -Wno-pointer-sign -Wno-array-bounds $< -o $@ -Wl,--mac-flat

build/NV34QD-INIT.bin: build/init68k.flt build/NV34QD.pef nv34qd.r
	$(REZ) -I $(RETRO68K)/RIncludes nv34qd.r -o $@ -t INIT -c NVQD

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_accel.o: ../accel/nv34_accel.c ../accel/nv34_accel.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_3d.o: ../accel/nv34_3d.c ../accel/nv34_3d.h ../accel/nv34_accel.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/qdpatch.o: qdpatch.c ../accel/nv34_accel.h ../accel/nv34_3d.h ../accel/nv34_shared.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/NV34QD.xcoff: build/qdpatch.o build/nv34_accel.o build/nv34_3d.o exports.txt
	$(CC) $(LDFLAGS) build/qdpatch.o build/nv34_accel.o build/nv34_3d.o $(LIBS) -o $@

build/NV34QD.pef: build/NV34QD.xcoff
	$(MAKEPEF) $< -o $@
	python3 ../tools/pef.py info $@ | grep -E "export|import lib"

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
