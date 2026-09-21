"""Check every numbered PCB pad against the regenerated Zener legacy netlist."""
import hashlib
import json
import re
import sys
from pathlib import Path
from collections import defaultdict
import pcbnew as p

if not __debug__:
    raise SystemExit('Run without -O: this checker uses assertions.')
if len(sys.argv) != 2:
    raise SystemExit('Usage: check_parity.py LAYOUT_DIRECTORY (with KiCad Python)')
root = Path(sys.argv[1])

def parse(text):
    stack = [[]]
    for token in re.findall('\\(|\\)|"(?:\\\\.|[^"\\\\])*"|[^\\s()]+', text):
        if token == '(':
            item = []
            stack[-1].append(item)
            stack.append(item)
        elif token == ')':
            stack.pop()
        else:
            stack[-1].append(json.loads(token) if token.startswith('"') else token)
    assert len(stack) == 1
    return stack[0][0]

def field(node, key):
    return next((x[1] for x in node[1:] if isinstance(x, list) and x[0] == key))

def section(node, key):
    return next((x for x in node[1:] if isinstance(x, list) and x[0] == key))
netlist = parse((root / 'default.net').read_text())
expected = {field(c, 'ref'): c for c in section(netlist, 'components')[1:]}
expected_pins = {}
for n in section(netlist, 'nets')[1:]:
    for item in n[1:]:
        if isinstance(item, list) and item[0] == 'node':
            key = (field(item, 'ref'), field(item, 'pin'))
            assert key not in expected_pins, key
            expected_pins[key] = {field(n, 'name')}
b = p.LoadBoard(str(root / 'layout.kicad_pcb'))
fps = list(b.GetFootprints())
assert len({f.GetReference() for f in fps}) == len(fps)
actual = {f.GetReference(): f for f in fps}
assert set(actual) == set(expected), (set(actual) - set(expected), set(expected) - set(actual))
actual_pins = defaultdict(set)
numbered_pad_count = 0
for ref, f in actual.items():
    assert f.GetValue() == field(expected[ref], 'value'), ref
    assert f.GetFPIDAsString() == field(expected[ref], 'footprint'), ref
    assert f.GetPath().AsString().split('/')[-1] == field(expected[ref], 'tstamps'), ref
    for pad in f.Pads():
        num = pad.GetNumber()
        if not num:
            assert not pad.GetNetname(), ref
            continue
        numbered_pad_count += 1
        actual_pins[ref, num].add(pad.GetNetname())
assert actual_pins == expected_pins, [
    (key, expected_pins.get(key), actual_pins.get(key))
    for key in sorted(set(expected_pins) | set(actual_pins))
    if expected_pins.get(key) != actual_pins.get(key)
]

def sha(name):
    return hashlib.sha256((root / name).read_bytes()).hexdigest()
report = {
    'result': 'pass',
    'components': len(actual),
    'unique_pins': len(actual_pins),
    'numbered_physical_pads': numbered_pad_count,
    'nets': len(section(netlist, 'nets')) - 1,
    'checks': [
        'reference set', 'component values', 'footprint IDs', 'schematic UUIDs',
        'every numbered physical pad net, including duplicate pads and no-connect nets',
    ],
    'sha256': {
        name: sha(name) for name in [
            'layout.kicad_pcb', 'default.net', 'snapshot.layout.json',
            'layout.kicad_pro', 'layout.kicad_dru',
        ]
    },
}
print(json.dumps(report, indent=2))
