# Všetko pre test na PowerBooku na jednom HFS obraze:
#   NV34 NDRV           driver
#   NV34 Info, NV34 Bench, NV34 Accel (test 2D enginu)
#   NV34 GL Test        prvý 3D trojuholník (fáza 4)
#   NV34 QD Accel       INIT (68k stub + PPC fragment): GPU CopyBits obrazovka → obrazovka
#   NV34 RAVE           QuickDraw 3D RAVE engine ('shlb'/'tnsl', Extensions) + NV34 RAVE Test
#   NV34 MiniGL         vlastná OpenGLLibrary pre hry (do priečinka hry, nie do Extensions)
#   make            → dist/NV34 test.dsk + dist/*.bin
RETRO68 ?= $(HOME)/Retro68-build/toolchain
H := $(RETRO68)/bin
TC := -DCMAKE_TOOLCHAIN_FILE=$(RETRO68)/powerpc-apple-macos/cmake/retroppc.toolchain.cmake

all: dist/NV34\ test.dsk

driver:
	$(MAKE) -C driver clean
	$(MAKE) -C driver dist

apps:
	cmake -S nv34info -B nv34info/build $(TC) >/dev/null
	cmake --build nv34info/build
	cmake -S nv34bench -B nv34bench/build $(TC) >/dev/null
	cmake --build nv34bench/build
	cmake -S nv34accel -B nv34accel/build $(TC) >/dev/null
	cmake --build nv34accel/build
	cmake -S nv34gl -B nv34gl/build $(TC) >/dev/null
	cmake --build nv34gl/build
	cmake -S nv34ravetest -B nv34ravetest/build $(TC) >/dev/null
	cmake --build nv34ravetest/build
	cmake -S nv34glcheck -B nv34glcheck/build $(TC) >/dev/null
	cmake --build nv34glcheck/build
	cmake -S nv34dsptest -B nv34dsptest/build $(TC) >/dev/null
	cmake --build nv34dsptest/build
	cmake -S nv34cpu -B nv34cpu/build $(TC) >/dev/null
	cmake --build nv34cpu/build
	$(MAKE) -C nv34qd
	$(MAKE) -C nv34rave
	VERS=1.1 VERS_INFO="NV34 RAVE engine (QuickDraw 3D RAVE)" NDRV_TYPE=shlb NDRV_CREATOR=tnsl tools/mkndrv.sh nv34rave/build/NV34RAVE.pef "NV34 RAVE" nv34rave/build >/dev/null
	cp "nv34rave/build/NV34 RAVE.bin" dist/NV34RAVE.bin
	$(MAKE) -C minigl
	VERS=1.0 VERS_INFO="NV34 MiniGL (OpenGL 1.1 + AGL)" NDRV_TYPE=shlb NDRV_CREATOR=NV34 tools/mkndrv.sh minigl/build/MiniGL.pef "OpenGLLibrary" minigl/build >/dev/null
	cp minigl/build/OpenGLLibrary.bin dist/OpenGLLibrary.bin
	cp nv34info/build/NV34Info.bin nv34bench/build/NV34Bench.bin nv34accel/build/NV34Accel.bin \
	   nv34gl/build/NV34GL.bin nv34ravetest/build/NV34RAVETest.bin nv34glcheck/build/NV34GLCheck.bin \
	   nv34dsptest/build/NV34DSpTest.bin nv34cpu/build/NV34CPU.bin dist/
	cp nv34qd/build/NV34QD-INIT.bin dist/NV34QD.bin

dist/NV34\ test.dsk: driver apps
	rm -f "$@"
	dd if=/dev/zero of="$@" bs=1024 count=6144 2>/dev/null
	$(H)/hformat -l "NV34 test" "$@" >/dev/null
	$(H)/hmount "$@" >/dev/null
	$(H)/hcopy -m "dist/NV34 NDRV.bin" ":NV34 NDRV"
	$(H)/hcopy -m dist/NV34Info.bin ":NV34 Info"
	$(H)/hcopy -m dist/NV34Bench.bin ":NV34 Bench"
	$(H)/hcopy -m dist/NV34Accel.bin ":NV34 Accel"
	$(H)/hcopy -m dist/NV34QD.bin ":NV34 QD Accel"
	$(H)/hcopy -m dist/NV34GL.bin ":NV34 GL Test"
	$(H)/hcopy -m dist/NV34RAVE.bin ":NV34 RAVE"
	$(H)/hcopy -m dist/NV34RAVETest.bin ":NV34 RAVE Test"
	$(H)/hmkdir ":NV34 MiniGL"
	$(H)/hcopy -m dist/OpenGLLibrary.bin ":NV34 MiniGL:OpenGLLibrary"
	$(H)/hdir -l
	$(H)/humount >/dev/null

clean:
	$(MAKE) -C driver clean
	rm -rf nv34info/build nv34bench/build nv34accel/build nv34gl/build nv34ravetest/build nv34glcheck/build \
	       nv34dsptest/build nv34cpu/build dist
	$(MAKE) -C nv34rave clean
	$(MAKE) -C minigl clean
	$(MAKE) -C nv34qd clean

# Balík na inštaláciu: dist/NV34 Release.dsk (priečinky podľa cieľa + Citaj ma) — docs/INSTALL.md
R := :NV34 GeForce FX
release: driver apps
	python3 tools/mkreadme.py docs/INSTALL.md dist/Citaj_ma.txt
	rm -f "dist/NV34 Release.dsk"
	dd if=/dev/zero of="dist/NV34 Release.dsk" bs=1024 count=8192 2>/dev/null
	$(H)/hformat -l "NV34 Release" "dist/NV34 Release.dsk" >/dev/null
	$(H)/hmount "dist/NV34 Release.dsk" >/dev/null
	$(H)/hmkdir "$(R)"
	$(H)/hmkdir "$(R):Do Extensions"
	$(H)/hmkdir "$(R):Do priecinka hry"
	$(H)/hmkdir "$(R):Nastroje"
	$(H)/hcopy -t dist/Citaj_ma.txt "$(R):Citaj ma"
	$(H)/hattrib -t TEXT -c ttxt "$(R):Citaj ma"
	$(H)/hcopy -m "dist/NV34 NDRV.bin" "$(R):Do Extensions:NV34 NDRV"
	$(H)/hcopy -m dist/NV34QD.bin "$(R):Do Extensions:NV34 QD Accel"
	$(H)/hcopy -m dist/NV34RAVE.bin "$(R):Do Extensions:NV34 RAVE"
	$(H)/hcopy -m dist/OpenGLLibrary.bin "$(R):Do priecinka hry:OpenGLLibrary"
	$(H)/hcopy -m dist/NV34Info.bin "$(R):Nastroje:NV34 Info"
	$(H)/hcopy -m dist/NV34GLCheck.bin "$(R):Nastroje:NV34 GL Check"
	$(H)/hcopy -m dist/NV34DSpTest.bin "$(R):Nastroje:NV34 DSp Test"
	$(H)/hcopy -m dist/NV34RAVETest.bin "$(R):Nastroje:NV34 RAVE Test"
	$(H)/hcopy -m dist/NV34CPU.bin "$(R):Nastroje:NV34 CPU"
	$(H)/hls -lR ":"
	$(H)/humount >/dev/null

.PHONY: all driver apps clean release
