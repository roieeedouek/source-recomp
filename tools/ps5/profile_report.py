#!/usr/bin/env python3
"""Summarizes a console profile (/data/portal2/profile.txt, written on request by runtime/ps5_title.c).

    python3 tools/ps5/profile_report.py profile.txt build/ps5-title-game/llvm-pie-deployed.elf [--top 30]

Each sample is a host backtrace. The signal-handler frames are dropped, addresses are slid back to the ELF's (by `main`) and
symbolized with llvm-symbolizer; generated guest functions are named fn_<guest address>. Prints the share of samples per
innermost named function (self) and per function anywhere in the stack (inclusive).
"""
import argparse
import collections
import re
import subprocess

HEADER = re.compile(r'profile: (\w+) thread, main at (0x[0-9a-f]+|[0-9a-f]+), (\d+) samples')
SKIP = ('ps5_backtrace', 'watchdog_sample', 'backtrace')


def elf_main(elf):
    out = subprocess.run(['llvm-nm-18', elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'main':
            return int(parts[0], 16)
    raise SystemExit('no main in ' + elf)


def symbolize(elf, addresses):
    if not addresses:
        return {}
    query = '\n'.join(hex(a) for a in addresses) + '\n'
    out = subprocess.run(['llvm-symbolizer-18', '--obj=' + elf, '-f', '-C', '--no-inlines'], input=query,
                         capture_output=True, text=True, check=True).stdout
    blocks = [b for b in out.split('\n\n') if b.strip()]
    names = {}
    for address, block in zip(addresses, blocks):
        name = block.splitlines()[0].strip()
        names[address] = name if name != '??' else None
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('profile')
    parser.add_argument('elf')
    parser.add_argument('--top', type=int, default=30)
    args = parser.parse_args()
    lines = open(args.profile).read().splitlines()
    match = HEADER.match(lines[0]) if lines else None
    if not match:
        raise SystemExit('not a profile: ' + args.profile)
    thread, runtime_main = match.group(1), int(match.group(2), 16)
    slide = runtime_main - elf_main(args.elf)
    stacks = [[int(word, 16) for word in line.split()] for line in lines[1:] if line.strip()]
    wanted = sorted({a - slide for stack in stacks for a in stack if a - slide > 0})
    names = symbolize(args.elf, wanted)
    self_counts, inclusive = collections.Counter(), collections.Counter()
    for stack in stacks:
        frames = [names.get(a - slide) for a in stack]
        named = [f for f in frames if f and not f.startswith(SKIP)]
        # The innermost frames are often system code (no symbols here): they count as "system" under the first named caller.
        first_named = next((i for i, f in enumerate(frames) if f and not f.startswith(SKIP)), None)
        leading_system = first_named is not None and any(f is None for f in frames[3:first_named])
        top = named[0] if named else '(unknown)'
        self_counts[('[system] ' if leading_system else '') + top] += 1
        for name in set(named):
            inclusive[name] += 1
    total = len(stacks) or 1
    print(f'{thread} thread: {len(stacks)} samples (10 ms apart)')
    print('\nself (innermost named function; [system] = inside the console libraries under it):')
    for name, count in self_counts.most_common(args.top):
        print(f'  {100 * count / total:5.1f}%  {name[:110]}')
    print('\ninclusive:')
    for name, count in inclusive.most_common(args.top):
        print(f'  {100 * count / total:5.1f}%  {name[:110]}')


if __name__ == '__main__':
    main()
