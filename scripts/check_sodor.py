#!/usr/bin/env python3
"""Checks EventTrace's records of Sodor's five-stage pipeline against Sodor itself.

Stage 5 tooling, which may not stay. Run riscv-tests on TracedSodor5StageConfig, writing each test's records next to
its printf log, and optionally on the plain Sodor5StageConfig too:

  out=$PWD/sims/verilator/output/chipyard.harness.TestHarness.TracedSodor5StageConfig
  make -C sims/verilator CONFIG=TracedSodor5StageConfig run-binary BINARY=$test \\
    EXTRA_SIM_FLAGS=+eventtrace=$out/$(basename $test).eventtrace
  make -C sims/verilator CONFIG=Sodor5StageConfig run-binary BINARY=$test

then

  uv run --project tools/eventtrace/scripts tools/eventtrace/scripts/check_sodor.py \\
    <traced output dir> --manifest <manifest> [--plain <plain output dir>] [--header <eventtrace_sites.h>] <test>...

The manifest is Chipyard's sims/verilator/generated-src/<long name>/<long name>.eventtrace.json. For each test it reads
<test>.out, Sodor's per-cycle printf log, and <test>.eventtrace, and checks:
 1. Function: with --plain, the printf log is identical without tracing, so tracing changes nothing Sodor does.
 2. Retirement: EventTrace retires the instructions Sodor's printf reports retiring, in order, with the PC and bits of
    their decode records, a constant number of cycles apart.
 3. Lifetimes: replaying the records, each event acts on an instruction in the stage it names, no instruction changes
    twice in a cycle, each stall's blocker and each squash's branch are in Execute, and no stage holds two
    instructions. If the trace also has `occupancy` records from a check-only event at the end of DatPath,
      EventTrace.event("occupancy") { "d" := dec_reg_valid; "e" := exe_reg_valid; "m" := mem_reg_valid; "w" := wb_reg_valid }
    the stages the tracked instructions occupy before each clock edge must equal Sodor's valid bits on every cycle;
    each cycle with all four low is a drained boundary at which no instruction may be live.
 4. Bandwidth: records, trace bytes, and, with --header, payload bits per cycle, over the cycles Sodor's printf covers.

The test's own outcome is reported separately: a test that fails both with and without tracing is no tracing problem.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eventtrace_trace import Trace  # noqa: E402

PRINTF = re.compile(r'^Cyc=\s*(\d+) \[(\d)\] pc=\[([0-9a-f]+)\].*inst=\[([0-9a-f]+)\]')
VALID_BITS = {'d': 'Decode', 'e': 'Execute', 'm': 'Memory', 'w': 'Writeback'}
# The stages each event takes its subject from, and the stage it moves it to; None destroys it.
MOVES = {
    'instruction.issue': ({'Decode'}, 'Execute'),
    'instruction.memory': ({'Execute'}, 'Memory'),
    'instruction.writeback': ({'Memory'}, 'Writeback'),
    'instruction.retire': ({'Writeback'}, None),
    'instruction.squash': ({'Decode'}, None),
    'instruction.flush': ({'Decode', 'Execute'}, None),
    'instruction.trap': ({'Memory'}, None),
}
# Events that leave their subject where it is.
STAYS = {'instruction.stall': 'Decode', 'instruction.mem_wait': 'Memory'}


def payload_bits(header):
    """Bits each site transmits, from the generated decoders' `// bits hi..lo` comments."""
    text = open(header).read()
    bits = collections.Counter()
    for m in re.finditer(r'decodeSite(\d+)\(.*?\n\}', text, re.S):
        bits[int(m[1])] = sum(int(hi) - int(lo) + 1 for hi, lo in re.findall(r'// bits (\d+)\.\.(\d+)', m[0]))
    return bits


def replay(records, problems):
    """Replays the records cycle by cycle; returns the retirements as (cycle, pc, bits) and the event counts."""
    live, decoded, retired = {}, {}, []
    counts = collections.Counter()
    next_id, last_occupancy = 0, None
    by_cycle = collections.defaultdict(list)
    for cycle, site, name, subject, fields in records:
        by_cycle[cycle].append((site, name, subject, fields))
    for cycle in sorted(by_cycle):
        before = dict(live)  # every event of a cycle sees the state before its clock edge
        stages = collections.Counter(before.values())
        if any(n > 1 for n in stages.values()):
            problems.append(f'cycle {cycle}: a stage holds two instructions: {dict(stages)}')
        changes = {}
        for site, name, subject, f in by_cycle[cycle]:
            counts[name] += 1
            if name == 'occupancy':
                if last_occupancy is not None and cycle != last_occupancy + 1:
                    problems.append(f'cycle {cycle}: no occupancy record since cycle {last_occupancy}')
                last_occupancy = cycle
                valid = {VALID_BITS[k] for k, v in f.items() if v}
                if set(stages) != valid:
                    problems.append(f'cycle {cycle}: Sodor holds {sorted(valid)}, the tracked instructions {dict(stages)}')
                continue
            i = subject
            if name == 'instruction.decode':
                if i != next_id:
                    problems.append(f'cycle {cycle}: created instruction#{i}, expected #{next_id}')
                next_id = i + 1
                changes[i] = 'Decode'
                decoded[i] = (f['pc'], f['bits'])
            elif name in MOVES:
                sources, target = MOVES[name]
                if before.get(i) not in sources:
                    problems.append(f'cycle {cycle}: {name} on instruction#{i}, which is in {before.get(i)}')
                if i in changes:
                    problems.append(f'cycle {cycle}: instruction#{i} changes twice')
                changes[i] = target
                if name == 'instruction.retire':
                    retired.append((cycle,) + decoded[i])
                if name == 'instruction.squash' and before.get(f['by']) != 'Execute':
                    problems.append(f'cycle {cycle}: squash by instruction#{f["by"]}, which is not in Execute')
            elif name in STAYS:
                if before.get(i) != STAYS[name]:
                    problems.append(f'cycle {cycle}: {name} on instruction#{i}, which is in {before.get(i)}')
                if name == 'instruction.stall' and before.get(f['blocker']) != 'Execute':
                    problems.append(f'cycle {cycle}: stall blocked by instruction#{f["blocker"]}, which is not in Execute')
            else:
                problems.append(f'cycle {cycle}: unknown event {name}')
        for i, target in changes.items():
            if target is None:
                live.pop(i, None)
            else:
                live[i] = target
    return retired, counts


def check(test, args, bits, totals):
    problems = []
    out = open(f'{args.traced}/{test}.out').read()
    passed = '*** PASSED ***' in out
    if args.plain and open(f'{args.plain}/{test}.out').read() != out:
        problems.append('the printf log differs without tracing')

    sodor = [(int(m[1]), int(m[3], 16), int(m[4], 16)) for m in map(PRINTF.match, out.splitlines()) if m and m[2] == '1']
    cycles = sum(1 for line in out.splitlines() if line.startswith('Cyc='))

    trace_path = f'{args.traced}/{test}.eventtrace'
    records = [(e.cycle, e.site_id, e.name, e.subject[1] if e.subject else None, e.fields)
               for e in Trace(args.manifest, trace_path).events()]
    ours, counts = replay(records, problems)

    if len(ours) != len(sodor):
        problems.append(f'EventTrace retires {len(ours)} instructions, Sodor {len(sodor)}')
    mismatch = next(((o, s) for o, s in zip(ours, sodor) if o[1:] != s[1:]), None)
    if mismatch:
        problems.append(f'first retirement mismatch: EventTrace (cycle, pc, bits) {mismatch[0]}, Sodor {mismatch[1]}')
    offsets = {o[0] - s[0] for o, s in zip(ours, sodor)}
    if len(offsets) > 1:
        problems.append(f'retirements are not a constant number of cycles apart: {sorted(offsets)[:5]}')

    traced = [r for r in records if r[2] != 'occupancy']
    per_cycle = collections.Counter(r[0] for r in traced)
    totals['cycles'] += cycles
    totals['records'] += len(traced)
    totals['bits'] += sum(bits[r[1]] for r in traced)
    totals['bytes'] += os.path.getsize(trace_path)
    totals['peak'] = max(totals['peak'], max(per_cycle.values(), default=0))
    totals['retired'] += len(ours)
    totals['events'].update({k: v for k, v in counts.items() if k != 'occupancy'})
    print(f'{test:28} {"pass" if passed else "FAIL":>4} {len(ours):>8} {cycles:>8} '
          f'{len(traced) / max(cycles, 1):>8.3f}  {"MISMATCH" if problems else "ok"}')
    for p in problems[:5]:
        print(f'    {p}')
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('traced', help='output directory of the TracedSodor5StageConfig runs')
    parser.add_argument('--manifest', required=True, help="the design's manifest, <long name>.eventtrace.json")
    parser.add_argument('--plain', help='output directory of the Sodor5StageConfig runs')
    parser.add_argument('--header', help='the generated eventtrace_sites.h, for payload bits')
    parser.add_argument('tests', nargs='+')
    args = parser.parse_args()
    bits = payload_bits(args.header) if args.header else collections.Counter()
    totals = collections.Counter()
    totals['events'] = collections.Counter()

    print(f'{"test":28} {"test":>4} {"retired":>8} {"cycles":>8} {"rec/cyc":>8}  tracing')
    ok = sum(check(test, args, bits, totals) for test in args.tests)
    print(f'\ntracing matches Sodor in {ok} of {len(args.tests)} tests; {totals["retired"]} retirements compared')
    print('events: ' + ', '.join(f'{k.split(".")[-1]} {v}' for k, v in sorted(totals['events'].items())))
    cycles = max(totals['cycles'], 1)
    retired = max(totals['retired'], 1)
    line = f'bandwidth: {totals["records"] / cycles:.3f} records per cycle, at most {totals["peak"]} in one cycle; ' \
           f'{totals["records"] / retired:.2f} per retired instruction; ' \
           f'{totals["bytes"] / cycles:.2f} trace bytes per cycle, {totals["bytes"] / retired:.1f} per retired instruction'
    if args.header:
        line += f'; {totals["bits"] / cycles:.1f} payload bits per cycle, {totals["bits"] / retired:.1f} per retired instruction'
    print(line)
    sys.exit(0 if ok == len(args.tests) else 1)


main()
