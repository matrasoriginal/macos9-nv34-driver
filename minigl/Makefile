# NV34 MiniGL — vlastná „OpenGLLibrary“ (OpenGL 1.1 + AGL) pre hry, do priečinka hry.
RETRO68    ?= $(HOME)/Retro68-build/toolchain
CC         := $(RETRO68)/bin/powerpc-apple-macos-gcc
NM         := $(RETRO68)/bin/powerpc-apple-macos-nm
MAKEPEF    := $(RETRO68)/bin/MakePEF

CFLAGS  := -O2 -Wall -Wno-pointer-sign -Wno-multichar -ffreestanding -fno-builtin \
           -fno-tree-loop-distribute-patterns
LDFLAGS := -nostartfiles -nostdlib -Wl,-bE:build/exports.txt -Wl,-bnoentry
LIBS    := -lNameRegistryLib -lInterfaceLib

OBJS := build/minigl.o build/agl.o build/hw.o build/nv34_accel.o build/nv34_3d.o
HDRS := minigl.h gldefs.h ../accel/nv34_accel.h ../accel/nv34_3d.h ../accel/nv34_shared.h

all: build/MiniGL.pef

build/%.o: %.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_accel.o: ../accel/nv34_accel.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nv34_3d.o: ../accel/nv34_3d.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/stubs.c build/exports.txt: $(OBJS) apple_exports.txt gen_stubs.py
	$(NM) $(OBJS) > build/nm.txt
	python3 gen_stubs.py apple_exports.txt build/nm.txt build/stubs.c build/exports.txt

build/stubs.o: build/stubs.c
	$(CC) $(CFLAGS) -c $< -o $@

build/MiniGL.xcoff: $(OBJS) build/stubs.o build/exports.txt
	$(CC) $(LDFLAGS) $(OBJS) build/stubs.o $(LIBS) -o $@ 2>&1 | grep -v "entry symbol" || true

build/MiniGL.pef: build/MiniGL.xcoff
	$(MAKEPEF) $< -o $@
	python3 ../tools/pef.py fixhash $@
	python3 ../tools/pef.py setinit $@ MiniGLInit MiniGLTerm
	python3 ../tools/pef.py info $@ | grep -E "export|import lib"

build:
	mkdir -p build

clean:
	rm -rf build
