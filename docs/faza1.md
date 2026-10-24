# Fáza 1 — vytiahnuť existujúci NVIDIA NDRV a otestovať ho v OS 9

Cieľ: zistiť, či nejaký hotový `ndrv` pre `NVDA,NVMac` (z ROM karty alebo z OS X) dá v OS 9 na
PB 6,2 režimy bez NVIDIA 3.1 balíka. Ak áno, vlastný driver (fáza 2) slúži len ako záloha.

## 1. Zdroje kandidátov (spúšťa sa v OS X 10.3.9 / 10.4 na tom istom PowerBooku)

**a) FCode ROM karty** — Apple NVIDIA karty majú v ROM často property
`driver,AAPL,MacOS,PowerPC` s natívnym driverom. Ak tam je, je to najlepší kandidát.

```
ioreg -l -w0 -x -p IODeviceTree > ioreg.txt
```

Na tomto Macu (alebo priamo na PowerBooku, Python 3 netreba ak skopíruješ súbor sem):

```
python3 tools/pef.py ioreg ioreg.txt candidates/
```

**b) OS X kexty** — NDRV fragmenty sú vložené ako PEF kontajnery v binárkach kextov.

```
find /System/Library/Extensions -type f -path '*Contents/MacOS/*' > kexts.txt
```

Skopíruj zoznam aj súbory (alebo celý `/System/Library/Extensions`) sem a:

```
python3 tools/pef.py scan /cesta/k/Extensions/*.kext/Contents/MacOS/*
python3 tools/pef.py extract /cesta/k/GeForce.kext/Contents/MacOS/GeForce candidates/
```

Zaujímavé sú kontajnery, kde `pef.py info` ukáže `TheDriverDescription` s `deviceType`
`NVDA,NVMac` (alebo `display`) a službu `ndrv/vido`. Katalóg známych OS X NDRV:
[elliotnunn/x-ndrv](https://github.com/elliotnunn/x-ndrv).

**c) Pozri importy** — ak kandidát importuje `NVIDIA Resource Manager` / `NvRm*`, bez 3.1 RM sa
nenačíta (CFM chyba = OS 9 ho potichu ignoruje).

```
python3 tools/pef.py info candidates/NVDA,NVMac_0.pef
```

## 2. Zabalenie pre OS 9

```
tools/mkndrv.sh candidates/NVDA,NVMac_0.pef "NV ROM ndrv" dist
```

Vznikne `dist/NV ROM ndrv` (type `ndrv`) a `dist/NV ROM ndrv.dsk` (HFS obraz).

## 3. Test

1. V OS 9 vypni NVIDIA 3.1 extensions (Extensions Manager), nechaj ich v `Extensions (Disabled)`.
2. Driver do `System Folder:Extensions`, restart.
3. Monitors CP: ponúka Millions / 256 / iné rozlíšenia?
4. Ak čierna obrazovka: reštart so **Shift** (Extensions Off) → driver zmazať.

Výsledok zapíš do PLAN.md. Ak žiadny kandidát nefunguje → fáza 2 ([driver/](../driver)).
