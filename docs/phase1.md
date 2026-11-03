# Phase 1 — extract an existing NVIDIA NDRV and test it on OS 9

Goal: find out whether some ready-made `ndrv` for `NVDA,NVMac` (from the card ROM or from OS X) gives
the PB 6,2 display modes on OS 9 without the NVIDIA 3.1 package. If so, our own driver (phase 2) is only
a fallback.

## 1. Candidate sources (run on OS X 10.3.9 / 10.4 on the same PowerBook)

**a) The card's FCode ROM** — Apple NVIDIA cards often have a `driver,AAPL,MacOS,PowerPC` property
with a native driver in ROM. If it is there, it is the best candidate.

```
ioreg -l -w0 -x -p IODeviceTree > ioreg.txt
```

On this Mac (or directly on the PowerBook; Python 3 is not needed there if you copy the file over here):

```
python3 tools/pef.py ioreg ioreg.txt candidates/
```

**b) OS X kexts** — NDRV fragments are embedded as PEF containers in kext binaries.

```
find /System/Library/Extensions -type f -path '*Contents/MacOS/*' > kexts.txt
```

Copy the list and the files (or the whole `/System/Library/Extensions`) here and run:

```
python3 tools/pef.py scan /path/to/Extensions/*.kext/Contents/MacOS/*
python3 tools/pef.py extract /path/to/GeForce.kext/Contents/MacOS/GeForce candidates/
```

The interesting containers are those where `pef.py info` shows a `TheDriverDescription` with
`deviceType` `NVDA,NVMac` (or `display`) and the `ndrv/vido` service. A catalog of known OS X NDRVs:
[elliotnunn/x-ndrv](https://github.com/elliotnunn/x-ndrv).

**c) Check the imports** — if a candidate imports `NVIDIA Resource Manager` / `NvRm*`, it will not load
without the 3.1 RM (a CFM error = OS 9 silently ignores it).

```
python3 tools/pef.py info candidates/NVDA,NVMac_0.pef
```

## 2. Packaging for OS 9

```
tools/mkndrv.sh candidates/NVDA,NVMac_0.pef "NV ROM ndrv" dist
```

This produces `dist/NV ROM ndrv` (type `ndrv`) and `dist/NV ROM ndrv.dsk` (HFS image).

## 3. Test

1. On OS 9 disable the NVIDIA 3.1 extensions (Extensions Manager), leave them in `Extensions (Disabled)`.
2. Put the driver into `System Folder:Extensions`, restart.
3. Monitors control panel: does it offer Millions / 256 / other resolutions?
4. If the screen goes black: restart holding **Shift** (Extensions Off) → delete the driver.

Record the result in PLAN.md. If no candidate works → phase 2 ([driver/](../driver)).

## Result

The ROM of the display node only contains Apple's generic `.Display_Video_Apple_COFB` (see
[phase0-hw.md](phase0-hw.md)), not an NVIDIA driver, so we went with our own driver (phase 2).
