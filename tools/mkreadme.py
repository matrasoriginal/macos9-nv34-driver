#!/usr/bin/env python3
"""docs/INSTALL.md → plain ASCII text without Markdown (SimpleText on OS 9 has no
UTF-8). Usage: mkreadme.py INSTALL.md OUTPUT.txt"""
import re, sys, unicodedata

src, dst = sys.argv[1:3]
out = []
for line in open(src, encoding='utf-8'):
    line = line.rstrip('\n')
    if line.startswith('```'):
        continue
    if re.match(r'^\|[-| ]+\|$', line):                 # table separator
        continue
    if line.startswith('|'):
        cells = [c.strip() for c in line.strip('|').split('|')]
        line = '  - ' + ' / '.join(c for c in cells if c)
    line = re.sub(r'^#+\s*', '', line)
    line = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', line)
    line = line.replace('**', '').replace('`', '"')
    line = line.replace('→', '->').replace('–', '-').replace('—', '-').replace('×', 'x')
    line = line.replace('„', '"').replace('“', '"').replace('…', '...')
    line = unicodedata.normalize('NFKD', line).encode('ascii', 'ignore').decode()
    out.append(line)
open(dst, 'w', encoding='ascii', newline='\r').write('\r'.join(out) + '\r')
