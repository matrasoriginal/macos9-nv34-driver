# NV34 RAVE — QuickDraw 3D RAVE drawing engine ('shlb' / 'tnsl', goes in Extensions).
RETRO68    ?= $(HOME)/Retro68-build/toolchain
CC         := $(RETRO68)/bin/powerpc-apple-macos-gcc
MAKEPEF    := $(RETRO68)/bin/MakePEF

CFLAGS  := -O2 -Wall -Wno-pointer-sign -Wno-multichar -ffreestanding -fno-builtin \
           -fno-tree-loop-distribute-patterns
LDFLAGS := -nostartfiles -nostdlib -Wl,-bE:exports.txt -Wl,-bnoentry
LIBS    := -lQuickDraw3DRAVELib -lNameRegistryLib -lInterfaceLib

OBJS := build/engine.o build/nv34_accel.o build/nv34_3d.o
HDRS := rave.h ../accel/nv34_accel.h ../accel/nv34_3d.h ../accel/nv34_shared.h

all: build/NV34RAVE.pef

build/engine.o: engine.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_accel.o: ../accel/nv34_accel.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_3d.o: ../accel/nv34_3d.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/NV34RAVE.xcoff: $(OBJS) exports.txt
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@ 2>&1 | grep -v "entry symbol" || true

build/NV34RAVE.pef: build/NV34RAVE.xcoff
	$(MAKEPEF) $< -o $@
	python3 ../tools/pef.py setinit $@ TtRegister TtDeRegister
	python3 ../tools/pef.py info $@ | grep -E "export|import lib"

build:
	mkdir -p build

clean:
	rm -rf build
