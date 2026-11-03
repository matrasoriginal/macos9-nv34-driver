#!/bin/sh
# Wraps a PEF data fork as a Mac OS 9 'ndrv' file for System Folder:Extensions.
#
#   tools/mkndrv.sh INPUT.pef "Name in OS 9" [OUTDIR]
#
# Output in OUTDIR (default dist/):
#   <name>            data fork + resource fork ('cfrg', 'vers') + type 'ndrv' / creator 'NV34'
#                     (copy only via HFS/HFS+ or AFP — FAT/FTP/web drop the forks and type)
#   <name>.bin        MacBinary II — safe to transfer any way, unpack on OS 9 with StuffIt Expander
#   <name>.dsk        1.4 MB HFS image (Disk Copy / ShrinkWrap / FloppyEmu)
set -eu

IN=$1
NAME=$2
OUT=${3:-dist}
TYPE=${NDRV_TYPE:-ndrv}
CREATOR=${NDRV_CREATOR:-NV34}
HFS=${RETRO68:-$HOME/Retro68-build/toolchain}/bin
HERE=$(dirname "$0")

mkdir -p "$OUT"
python3 "$HERE/macres.py" ndrv "$IN" "$NAME" "$OUT" "$TYPE" "$CREATOR"

rm -f "$OUT/$NAME"
cp "$IN" "$OUT/$NAME"
hex() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
FI="$(hex "$TYPE")$(hex "$CREATOR")"
while [ ${#FI} -lt 64 ]; do FI="${FI}0"; done
xattr -wx com.apple.FinderInfo "$FI" "$OUT/$NAME"
cat "$OUT/$NAME.rsrc" > "$OUT/$NAME/..namedfork/rsrc"
rm -f "$OUT/$NAME.rsrc"

IMG="$OUT/$NAME.dsk"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1024 count=1440 2>/dev/null
"$HFS/hformat" -l "$NAME" "$IMG" >/dev/null
"$HFS/hmount" "$IMG" >/dev/null
"$HFS/hcopy" -m "$OUT/$NAME.bin" ":$NAME"
"$HFS/hdir" -l
"$HFS/humount" >/dev/null

echo "-> $OUT/$NAME, $OUT/$NAME.bin, $IMG"
