#!/usr/bin/env python3
"""Packages the built PS5 title for installing on a console, without any game files.

    python3 tools/ps5/package_title.py [--title-dir build/ps5-title-game/PPSA97622] [--out build/ps5-release]
                                       [--with-art] [--no-fps60]

Writes build/ps5-release/portal2-ps5-<version>/ and a .zip of it:

    homebrew/PPSA97622/      the title: eboot.bin, sce_sys/param.json and icon0.png, sce_module/libc.prx
    portal2/                 support files for /data/portal2: fonts.conf, fps60 (60 fps lock; --no-fps60 leaves it out)
    game-files.sha256        the game's Linux binaries this build was made from (tools/gameprep.py's plan)
    check_game.py            checks a Portal 2 installation against game-files.sha256
    INSTALL.txt              short installation steps (README.md, "PS5", has the full ones)

The player supplies the game: their own Portal 2 (Linux files) copied to /data/portal2/game, and /data/portal2/prep made
from it with tools/gameprep.py. The title only runs with the exact game build it was compiled from, hence the checksums.

The icon is a plain text one: artwork made from the game's screenshots (build/ps5-art) is only included with --with-art,
for a package that stays with you. eboot.bin contains code translated from the game's own binaries: keep such a package
for yourself, do not publish it.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]

CHECK_GAME = '''#!/usr/bin/env python3
"""Checks a Portal 2 installation (its Linux files) against game-files.sha256.

    python3 check_game.py /path/to/Portal 2

The PS5 build in this package only runs with exactly these binaries: install the Linux version of the game
(Steam: Properties > Compatibility, untick forced compatibility tools) and point this at its folder.
"""
import hashlib
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
bad = 0
for line in (Path(__file__).with_name('game-files.sha256')).read_text().splitlines():
    digest, name = line.split(None, 1)
    path = root / name
    if not path.is_file():
        print(f'missing    {name}')
        bad += 1
    elif hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        print(f'different  {name}')
        bad += 1
print('this installation matches the build' if not bad else f'{bad} file(s) do not match: this build will not run with it')
sys.exit(1 if bad else 0)
'''

INSTALL = '''Portal 2 for a jailbroken PS5 (source-recomp), package {version}
=====================================================================

No game files are included. You need your own copy of Portal 2 (Linux files), and a PC with Python 3
and a clone of https://github.com/roieeedouek/source-recomp (branch ps5-port).

1. Check your game files:       python3 check_game.py "<your Portal 2 folder>"
2. Prepare its libraries:       in the clone, set "linux_root"/"game_root" in config/local.json
                                to your Portal 2 folder, then: python3 tools/gameprep.py
                                (writes build/game/prep)
3. Copy to the console (FTP), keeping the folder names:
     homebrew/PPSA97622        -> /data/homebrew/PPSA97622
     <your Portal 2 folder>    -> /data/portal2/game
     build/game/prep           -> /data/portal2/prep
     portal2/*                 -> /data/portal2/
4. Make /data/homebrew/PPSA97622/eboot.bin executable (chmod 755) and register the title with your
   homebrew launcher (ShadowMountPlus), then start "Portal 2 (source-recomp)".

Optional files in /data/portal2: fps60 (60 fps lock), show_fps (frame counter), args.txt (extra game
arguments, e.g. +load <save>). The log is /data/portal2/log.txt. See README.md, "PS5", for details.

This package contains code translated from Portal 2's own binaries: keep it for yourself, do not publish it.
'''


def text_icon(path):
    """A plain 512x512 icon: the title's name on dark blue, no game artwork."""
    image = Image.new('RGB', (512, 512), (18, 40, 78))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 330, 512, 338), fill=(80, 170, 255))
    for text, size, y in (('Portal 2', 92, 150), ('source-recomp', 46, 270), ('PS5', 40, 380)):
        font = ImageFont.load_default(size=size)
        width = draw.textlength(text, font=font)
        draw.text(((512 - width) / 2, y), text, font=font, fill=(235, 240, 250))
    image.save(path, optimize=True)


def version():
    try:
        return subprocess.run(['git', '-C', str(ROOT), 'describe', '--always', '--dirty'], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--title-dir', type=Path, default=ROOT / 'build/ps5-title-game/PPSA97622')
    parser.add_argument('--prep', type=Path, default=ROOT / 'build/game/prep', help="tools/gameprep.py's output (its plan)")
    parser.add_argument('--out', type=Path, default=ROOT / 'build/ps5-release')
    parser.add_argument('--with-art', action='store_true', help='include build/ps5-art (made from screenshots: personal use)')
    parser.add_argument('--no-fps60', action='store_true', help='leave out the 60 fps lock file')
    args = parser.parse_args()

    title = args.title_dir
    for needed in ('eboot.bin', 'sce_sys/param.json', 'sce_module/libc.prx'):
        if not (title / needed).is_file():
            parser.exit(1, f'{title / needed} missing: build the title first (tools/ps5/build_title.sh)\n')
    plan = json.loads((args.prep / 'plan.json').read_text())

    name = f'portal2-ps5-{version()}'
    package = args.out / name
    if package.exists():
        shutil.rmtree(package)
    target = package / 'homebrew' / title.name
    (target / 'sce_sys').mkdir(parents=True)
    (target / 'sce_module').mkdir()
    shutil.copy2(title / 'eboot.bin', target / 'eboot.bin')
    shutil.copy2(title / 'sce_sys/param.json', target / 'sce_sys/param.json')
    shutil.copy2(title / 'sce_module/libc.prx', target / 'sce_module/libc.prx')
    art = ROOT / 'build/ps5-art'
    if args.with_art and (art / 'icon0.png').is_file():
        for file in ('icon0.png', 'pic0.dds', 'pic1.dds'):
            if (art / file).is_file():
                shutil.copy2(art / file, target / 'sce_sys' / file)
    else:
        text_icon(target / 'sce_sys/icon0.png')

    data = package / 'portal2'
    data.mkdir()
    shutil.copy2(ROOT / 'tools/ps5/fonts.conf', data / 'fonts.conf')
    if not args.no_fps60:
        (data / 'fps60').write_bytes(b'')

    # The game's binaries, relative to the game folder: gameprep's plan has their paths in this machine's copy.
    first = Path(plan[0]['path'])
    game_root = next(parent for parent in first.parents if (parent / 'portal2_linux').exists() or (parent / 'bin').is_dir())
    files = [Path(entry['path']) for entry in plan]
    if (game_root / 'portal2_linux').is_file():
        files.append(game_root / 'portal2_linux')
    lines = [f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(game_root).as_posix()}' for p in files]
    (package / 'game-files.sha256').write_text('\n'.join(lines) + '\n')
    (package / 'check_game.py').write_text(CHECK_GAME)
    (package / 'INSTALL.txt').write_text(INSTALL.format(version=version()))

    archive = shutil.make_archive(str(args.out / name), 'zip', root_dir=args.out, base_dir=name)
    size = Path(archive).stat().st_size
    print(f'{package}\n{archive} ({size / 2**20:.0f} MiB): title {(target / "eboot.bin").stat().st_size / 2**20:.0f} MiB, '
          f'{len(files)} game binaries fingerprinted, icon {"from build/ps5-art" if args.with_art else "plain text"}')


if __name__ == '__main__':
    main()
