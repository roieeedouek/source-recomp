#!/usr/bin/env bash
# Uploads the built title, launches it through the console's shell server and prints its log.
#   PS5_HOST=<console address> tools/ps5/run_title.sh [--no-upload] [--wait SECONDS] [TITLE_ID]
#   tools/ps5/run_title.sh --shot OUT.png      screenshot of the running game (it writes /data/portal2/screen.bmp on request)
#   tools/ps5/run_title.sh --profile main|submit|record   10 s of instruction-address samples of the running game, symbolized (tools/ps5/profile_report.py)
# Needs on the console: an FTP server (port 2120 or 2121: probed), shsrv on 2323. The game writes /data/portal2/log.txt; this prints
# it from the last "=== portal2 title starts" marker. Ends when the log shows an exit or a fatal signal, or after the wait.
set -euo pipefail
host=${PS5_HOST:?set PS5_HOST to the console address}
upload=1 wait=120 shot="" profile=""
while [[ ${1:-} == --* ]]; do
    case $1 in --no-upload) upload=0 ;; --wait) wait=$2; shift ;; --shot) shot=$2; shift ;; --profile) profile=$2; shift ;; *) echo "unknown option $1" >&2; exit 2 ;; esac
    shift
done
title=${1:-PPSA97622}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
folder=$here/../../build/ps5-title-game/$title
open_port() { timeout 2 bash -c "echo > /dev/tcp/$host/$1" 2>/dev/null; }
ftp_port=""
for port in 2120 2121; do open_port $port && { ftp_port=$port; break; }; done
[[ -n $ftp_port ]] || { echo "no FTP server on $host (2120, 2121)" >&2; exit 1; }
open_port 2323 || { echo "no shsrv on $host:2323" >&2; exit 1; }
shell() { printf '%s\nexit\n' "$1" | timeout 20 nc -w 10 "$host" 2323 2>&1 | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g'; }
log() { curl -s --disable-epsv --max-time 30 "ftp://$host:$ftp_port/data/portal2/log.txt" | tr -d '\000'; }
if [[ -n $profile ]]; then
    curl -s --disable-epsv --max-time 10 -Q "DELE /data/portal2/profile.txt" "ftp://$host:$ftp_port/" > /dev/null 2>&1 || true
    printf '%s\n' "$profile" | curl -sS --disable-epsv --max-time 15 -T - "ftp://$host:$ftp_port/data/portal2/profile.request"
    sleep 14
    raw=$(mktemp)
    for ((t = 0; t < 20; t++)); do
        curl -s --disable-epsv --max-time 30 "ftp://$host:$ftp_port/data/portal2/profile.txt" -o "$raw" && [[ -s $raw ]] && break
        sleep 2
    done
    python3 -I "$here/profile_report.py" "$raw" "$here/../../build/ps5-title-game/llvm-pie-deployed.elf"
    rm -f "$raw"
    exit 0
fi
if [[ -n $shot ]]; then
    curl -sS --disable-epsv --max-time 15 -T /dev/null "ftp://$host:$ftp_port/data/portal2/screenshot.request"
    for ((t = 0; t < 15; t++)); do
        sleep 1
        curl -s --disable-epsv --max-time 10 "ftp://$host:$ftp_port/data/portal2/" | grep "screenshot.request" > /dev/null || break
    done
    sleep 1
    bmp=$(mktemp --suffix=.bmp)
    curl -sS --disable-epsv --max-time 60 "ftp://$host:$ftp_port/data/portal2/screen.bmp" -o "$bmp"
    python3 -I -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$bmp" "$shot"
    rm -f "$bmp"
    echo "screenshot: $shot"
    exit 0
fi

# A running (or hung) instance is stopped first. The shell's kill does not reach an app: a payload (killapp.c) asks the system to
# kill it by AppId (ps column 7), through elfldr on port 9021.
app=$(shell ps | awk -v t="$title" '$0 ~ t && /eboot\.bin/ && !found {print $7; found = 1}')
if [[ -n $app ]]; then
    killer=$here/../../build/ps5-killapp-$app.elf
    sdk=$(realpath "$here/../../../deps/ps5-sdk-full")
    [[ -f $killer ]] || PS5_PAYLOAD_SDK=$sdk "$sdk/bin/prospero-clang" -DKILL_APP_ID=0x$app -O2 -o "$killer" "$here/killapp.c" -lSceSystemService
    timeout 10 bash -c "cat '$killer' > /dev/tcp/$host/9021"
    for ((t = 0; t < 20; t++)); do sleep 1; shell ps | grep "$title" > /dev/null || break; done
    echo "stopped running instance (AppId $app)"
fi
if ((upload)); then
    curl -sS --disable-epsv --max-time 580 -T "$folder/eboot.bin" "ftp://$host:$ftp_port/data/homebrew/$title/eboot.bin"
    shell "chmod 755 /data/homebrew/$title/eboot.bin" > /dev/null
    echo "uploaded $(stat -c %s "$folder/eboot.bin") bytes"
    cp "$folder/../link/llvm-pie.elf" "$folder/../llvm-pie-deployed.elf"   # symbols of what runs, for --profile
    curl -sS --disable-epsv --max-time 30 -T "$here/fonts.conf" "ftp://$host:$ftp_port/data/portal2/fonts.conf"
fi
before=$(log | wc -l)
shell "launch $title" | grep -v -E "^(Welcome|Model|S/N|S/W|SoC|CPU|Type|$)" | tail -2 || true
for ((t = 0; t < wait; t += 5)); do
    sleep 5
    new=$(log | tail -n +$((before + 1)))
    grep -q -E "exit status|fatal signal|terminating|guest trap" <<<"$new" && break
done
log | tail -n +$((before + 1)) | cut -c1-300 | head -${PS5_LOG_LINES:-120}
