#!/usr/bin/env python3
"""Lint the companion command parser for unguarded frame reads.

WHAT THIS IS NOT: a proof that the parser is safe. It reads the source with
regular expressions and it has already been wrong.

It only recognises literal indices - cmd_frame[4]. Several handlers walk the
frame with a cursor and write cmd_frame[i], and those were invisible to it:
CMD_SET_TUNING_PARAMS read eight bytes and persisted them, CMD_SET_RADIO_PARAMS
read ten, and this script reported "0 unguarded" the whole time. It also used to
count memcmp(&cmd_frame[...]) as a gate, when memcmp is precisely the read that
needs guarding, and it does not model C++ evaluation order, so a condition like
`cmd_frame[1] == 0 && len >= 3` reads the byte before the length test beside it.

The real guarantee lives in examples/companion_radio/CompanionCmdLimits.h: a
minimum length per command, checked once before dispatch, with native tests. This
stays as a cheap second pair of eyes on the shape of the chain - it is what
caught six branches when it was written - but a green run here means only that
the pattern it knows about was not found.

For every branch this checks, in source order, whether a length gate appears
before the first literal read past byte 0.

Exit 1 if any branch reads past byte 0 with no preceding gate.
"""
import io, re, sys

SRC = 'examples/companion_radio/MyMesh.cpp'
SRC_TEXT = io.open(SRC, encoding='utf-8').read()
lines = SRC_TEXT.split('\n')

start = next(i for i, l in enumerate(lines) if 'void MyMesh::handleCmdFrame' in l)
heads = [i for i in range(start, len(lines))
         if re.search(r'cmd_frame\[0\] == (CMD_[A-Z_]+)', lines[i])]
heads.append(next(i for i in range(heads[-1], len(lines)) if lines[i] == '}'))

# memcmp on the frame is NOT a gate - it is the read being guarded. It was
# treated as one here, which is part of why this reported a clean parser.
GATE = re.compile(r'\blen\b\s*(?:>=|>|<=|<|==|!=)')
READ = re.compile(r'cmd_frame\[([1-9]\d*)\]')

# The central table is a real gate, so a command with a minimum in it is covered
# whatever its branch does. Parsed from the header rather than duplicated here:
# two copies of these numbers would drift, and the drift would be silent.
LIMITS = 'examples/companion_radio/CompanionCmdLimits.h'
gated_by_table = set()
try:
    lim = io.open(LIMITS, encoding='utf-8').read()
    for code, minimum in re.findall(r'case\s+(\d+):\s*return\s+(\d+);', lim):
        if int(minimum) > 1:
            gated_by_table.add(int(code))
except OSError:
    print('warning: could not read %s - table gating not applied' % LIMITS)

# Map command names to their numeric codes, so the table can be matched up.
codes = {}
for name, val in re.findall(r'#define\s+(CMD_[A-Z_0-9]+)\s+(\d+)', SRC_TEXT):
    codes[name] = int(val)

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
            if codes.get(name) in gated_by_table:
                break     # covered by the minimum-length table before dispatch
            findings.append((heads[k] + off + 1, name, int(m.group(1))))
            break

for ln, name, byte in findings:
    print('%s:%d: %s reads cmd_frame[%d] before any length gate' % (SRC, ln, name, byte))
print('%d branches checked, %d unguarded by the pattern this knows' % (len(heads) - 1, len(findings)))
print('this is a lint, not a proof - see CompanionCmdLimits.h')
sys.exit(1 if findings else 0)
