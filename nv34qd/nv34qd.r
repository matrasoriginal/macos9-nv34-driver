/* NV34 QD Accel — 'INIT' súbor: 68k stub + PPC fragment v resource 'nvqd'. */
#include "Retro68.r"
#include "MacTypes.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, sysheap, locked) {
	dontBreakAtEntry, $$read("build/init68k.flt");
};

data 'nvqd' (128, sysheap, locked) {
	$$read("build/NV34QD.pef")
};

resource 'vers' (1) {
	0x01, 0x90, release, 0x00, 0,
	"1.9",
	"1.9, NV34 QD Accel — GPU CopyBits pre GeForce FX Go5200"
};
