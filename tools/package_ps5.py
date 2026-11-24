#!/usr/bin/env python3
"""Lays out the PS5 package: the payload ELF, the prepared module images and
helper scripts, in `build/ps5-package/`. Game data is NOT copied here (about
11 GB of your own Portal 2 copy): the script prints the copy command.

    python3 tools/package_ps5.py [--elf build/portal2-ps5.elf] [--out build/ps5-package] [--game-dir ../Portal.2]

Layout on the console (RT_PS5_ROOT in src/main.c, default /data/portal2):
    /data/portal2/portal2-ps5.elf   the payload (sent to the ELF loader, or run by your loader of choice)
    /data/portal2/prep/             module images (this package, 77 MB)
    /data/portal2/game/             your Portal 2 folder (bin, platform, portal2, portal2_dlc*) + Linux depot
    /data/portal2/user/             saves and settings (created at run time)
See docs/ps5.md. Untested on hardware.
"""
import argparse
import shutil
import sys
from pathlib import Path

import games

ROOT = games.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--elf', type=Path, default=ROOT / 'build/portal2-ps5.elf')
    parser.add_argument('--prep', type=Path, default=ROOT / 'build/game/prep')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/ps5-package')
    parser.add_argument('--game-dir', type=Path, default=ROOT.parent / 'Portal.2')
    args = parser.parse_args()
    if not args.elf.exists():
        sys.exit(f'{args.elf} missing: python3 tools/build_game.py --release --target ps5 (PS5_PAYLOAD_SDK set)')
    if not (args.prep / 'layout.json').exists():
        sys.exit(f'{args.prep}/layout.json missing: python3 tools/gameprep.py')
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    (out / 'portal2').mkdir(parents=True)
    shutil.copy2(args.elf, out / 'portal2/portal2-ps5.elf')
    shutil.copytree(args.prep, out / 'portal2/prep')
    (out / 'send.sh').write_text('''#!/bin/sh
# Sends the payload to the PS5's ELF loader (default port 9021). Usage: ./send.sh PS5_IP
set -e
[ -n "$1" ] || { echo "usage: $0 PS5_IP" >&2; exit 2; }
socat -t 99999999 - TCP:"$1":9021 < "$(dirname "$0")/portal2/portal2-ps5.elf"
''')
    (out / 'send.sh').chmod(0o755)
    (out / 'README.txt').write_text(f'''Portal 2 (source-recomp) for a jailbroken PS5 -- EXPERIMENTAL, not yet run on hardware.

1. Copy to the console (FTP, port 1337 with ftpsrv, or USB):
     {out}/portal2/portal2-ps5.elf  ->  /data/portal2/portal2-ps5.elf
     {out}/portal2/prep/            ->  /data/portal2/prep/
     {args.game_dir}/               ->  /data/portal2/game/   (about 11 GB: bin platform portal2 portal2_dlc1 portal2_dlc2 + bin/linux32)
2. Jailbreak (Relapse on firmware 13.60), start the ELF loader (port 9021).
3. ./send.sh PS5_IP     (or let your loader run /data/portal2/portal2-ps5.elf)
Logs: the payload writes to stderr/klog; use klogsrv (port 3232) or ps5-payload-shsrv.
See source-recomp/docs/ps5.md for the architecture and known risks.
''')
    size = sum(p.stat().st_size for p in out.rglob('*') if p.is_file())
    print(f'package: {out} ({size / 2**20:.0f} MB)')
    print(f'game data to copy by hand: {args.game_dir} -> /data/portal2/game/')


if __name__ == '__main__':
    main()
