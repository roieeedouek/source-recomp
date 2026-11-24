#!/usr/bin/env python3
"""Writes the C file for the title's dlsym table: every Steam* function the offline stub (runtime/ios/steam_stub*.c) defines.

    gen_dl_table.py OUT.c OBJECT_OR_RSP...      (an @file argument is a linker response file listing objects)

A title has no dynamic symbol table to look names up in, so runtime/ps5.c's dlsym searches this array instead.
"""
import re
import shlex
import subprocess
import sys
from pathlib import Path

NM = 'llvm-nm-18'


def objects(arguments):
    for argument in arguments:
        if argument.startswith('@'):
            for name in shlex.split(Path(argument[1:]).read_text()):
                if name.endswith('.o'):
                    yield name
        elif argument.endswith('.o'):
            yield argument


def main():
    out = Path(sys.argv[1])
    files = list(objects(sys.argv[2:]))
    names = set()
    for start in range(0, len(files), 100):
        text = subprocess.run([NM, '-g', '--defined-only', *files[start:start + 100]], capture_output=True, text=True).stdout
        names.update(m.group(1) for m in re.finditer(r'^\S+ [TDB] (Steam\w+)$', text, re.M))
    ordered = sorted(names)
    lines = ['// Written by tools/ps5/gen_dl_table.py: the symbols runtime/ps5.c\'s dlsym can find in the executable.',
             'struct rt_dl_entry { const char *name; void *address; };']
    lines += [f'extern char {n}[];' for n in ordered]
    lines += ['const struct rt_dl_entry rt_dl_table[] = {']
    lines += [f'    {{"{n}", {n}}},' for n in ordered]
    lines += ['    {0, 0}', '};', f'const unsigned rt_dl_count = {len(ordered)};', '']
    out.write_text('\n'.join(lines))
    print(f'{out}: {len(ordered)} symbols')


if __name__ == '__main__':
    main()
