#!/usr/bin/env python3
"""Vygeneruje stubs.c (neimplementované exporty Apple OpenGLLibrary) a exports.txt.

  gen_stubs.py apple_exports.txt NM_OUTPUT stubs.c exports.txt

NM_OUTPUT = výstup nm z objektov MiniGL (definované symboly). Stub zaloguje prvé volanie
a vráti 0. Dáta (glc_cc) sa exportujú ako vynulované pole.
"""
import re, sys

apple, nmout, stubs, exports = sys.argv[1:5]
names = []
for line in open(apple):
    m = re.match(r'(\S+)\s+\[(\w+)\]', line)
    if m:
        names.append((m.group(1), m.group(2)))
defined = set()
for line in open(nmout):
    p = line.split()
    if len(p) >= 3 and p[1] in 'TDBC':
        defined.add(p[2].lstrip('.'))
    elif len(p) == 2 and p[0] in 'TDBC':
        defined.add(p[1].lstrip('.'))

out = ['/* generované gen_stubs.py — neupravovať */',
       'typedef unsigned long NVU32;',
       'void stub_hit(const char *name, NVU32 idx);', '']
n = 0
for i, (name, kind) in enumerate(names):
    if name in defined:
        continue
    if kind == 'data':
        out.append(f'long {name}[64];')
    else:
        out.append(f'long {name}(void) {{ stub_hit("{name}", {i}); return 0; }}')
    n += 1
open(stubs, 'w').write('\n'.join(out) + '\n')
open(exports, 'w').write('\n'.join([nm for nm, _ in names] + ['MiniGLInit', 'MiniGLTerm']) + '\n')
print(f'{len(names)} exportov, {len(names) - n} implementovanych, {n} stubov')
