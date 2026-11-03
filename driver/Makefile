# NV34 NDRV — cross-build with Retro68 (PowerPC PEF).
#
#   make                  → build/NV34NDRV.pef (data fork) + info
#   make NV_SCALED_MODES=0  without the scaled modes 800×600 / 640×480
#
RETRO68 ?= $(HOME)/Retro68-build/toolchain
CC      := $(RETRO68)/bin/powerpc-apple-macos-gcc
MAKEPEF := $(RETRO68)/bin/MakePEF

NV_SCALED_MODES ?= 1
NV_SET_ARB      ?= 0
NV_FB_CACHE     ?= 0

CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin \
           -fno-tree-loop-distribute-patterns \
           -DNV_SCALED_MODES=$(NV_SCALED_MODES) -DNV_SET_ARB=$(NV_SET_ARB) -DNV_FB_CACHE=$(NV_FB_CACHE)
LDFLAGS := -nostartfiles -nostdlib -Wl,-bE:exports.txt -Wl,-bnoentry
LIBS    := -lDriverServicesLib -lNameRegistryLib -lVideoServicesLib

OBJS := build/ndrv.o build/nv34.o

all: build/NV34NDRV.pef
	python3 ../tools/pef.py info $<

build/%.o: %.c ndrv_api.h nv34.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/NV34NDRV.xcoff: $(OBJS) exports.txt
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

build/NV34NDRV.pef: build/NV34NDRV.xcoff
	$(MAKEPEF) $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean

dist: build/NV34NDRV.pef
	../tools/mkndrv.sh $< "NV34 NDRV" ../dist

.PHONY: dist
