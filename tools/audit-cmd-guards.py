#!/usr/bin/env python3
"""Lint the companion command parser for unguarded frame reads.

handleCmdFrame() is a long if/else chain. Each branch may read cmd_frame[1..],
but the frame length is whatever the host sent. A read that happens before any
length gate acts on a byte that may not have been transmitted.

For every branch this checks, in source order, whether a length gate appears
before the first read past byte 0. A gate is a comparison involving `len`, or a
memcmp against a magic string (which cannot match absent bytes).

This is a lint, not a proof: it does not follow variable indices or helper
calls. It catches the class of defect that has actually occurred here.

Exit 1 if any branch reads past byte 0 with no preceding gate.
"""
import io, re, sys

SRC = 'examples/companion_radio/MyMesh.cpp'
lines = io.open(SRC, encoding='utf-8').read().split('\n')

start = next(i for i, l in enumerate(lines) if 'void MyMesh::handleCmdFrame' in l)
heads = [i for i in range(start, len(lines))
         if re.search(r'cmd_frame\[0\] == (CMD_[A-Z_]+)', lines[i])]
heads.append(next(i for i in range(heads[-1], len(lines)) if lines[i] == '}'))

GATE = re.compile(r'\blen\b\s*(?:>=|>|<=|<|==|!=)|memcmp\(\s*&cmd_frame\[')
READ = re.compile(r'cmd_frame\[([1-9]\d*)\]')

findings = []
for k in range(len(heads) - 1):
    body = lines[heads[k]:heads[k + 1]]
    name = re.search(r'cmd_frame\[0\] == (CMD_[A-Z_]+)', body[0]).group(1)
    gated = False
    for off, line in enumerate(body):
        if GATE.search(line):
            gated = True
        m = READ.search(line)
        if m and not gated:
            findings.append((heads[k] + off + 1, name, int(m.group(1))))
            break

for ln, name, byte in findings:
    print('%s:%d: %s reads cmd_frame[%d] before any length gate' % (SRC, ln, name, byte))
print('%d branches checked, %d unguarded' % (len(heads) - 1, len(findings)))
sys.exit(1 if findings else 0)
