#!/usr/bin/env bash
# Frame-rate benchmark of a title build on the console, from a save: installs EBOOT (or the built one), loads SAVE through
# /data/portal2/args.txt, lets loading settle and averages the frame statistics the title writes to trace.txt every 10 s.
#   PS5_HOST=<console> tools/ps5/bench_title.sh SAVE [EBOOT] [--settle SECONDS] [--measure SECONDS]
# Prints the 10 s windows measured and their mean frame rate. The game is left running.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
host=${PS5_HOST:?set PS5_HOST to the console address}
save=${1:?save name, as for +load (without .sav)}; shift
eboot=""
settle=60 measure=60
while (($#)); do
    case $1 in --settle) settle=$2; shift ;; --measure) measure=$2; shift ;; *) eboot=$1 ;; esac
    shift
done
ftp=ftp://$host:2120
printf '+load %s\n' "$save" | curl -sS --disable-epsv --max-time 15 -T - "$ftp/data/portal2/args.txt"
shell() { printf '%s\nexit\n' "$1" | timeout 20 nc -w 10 "$host" 2323 2>&1 | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g'; }
# A running instance holds eboot.bin: stopped first (the shell's kill does not reach an app: killapp.c through the ELF loader).
app=$(shell ps | awk '/PPSA97622/ && /eboot\.bin/ && !found {print $7; found = 1}')
if [[ -n $app ]]; then
    killer=$here/../../build/ps5-killapp-$app.elf
    sdk=$(realpath "$here/../../../deps/ps5-sdk-full")
    [[ -f $killer ]] || PS5_PAYLOAD_SDK=$sdk "$sdk/bin/prospero-clang" -DKILL_APP_ID=0x$app -O2 -o "$killer" "$here/killapp.c" -lSceSystemService
    timeout 10 bash -c "cat '$killer' > /dev/tcp/$host/9021"
    for ((t = 0; t < 20; t++)); do sleep 1; shell ps | grep PPSA97622 > /dev/null || break; done
fi
if [[ -n $eboot ]]; then
    curl -sS --disable-epsv --max-time 600 -T "$eboot" "$ftp/data/homebrew/PPSA97622/eboot.bin"
    printf 'chmod 755 /data/homebrew/PPSA97622/eboot.bin\nexit\n' | timeout 20 nc -w 10 "$host" 2323 > /dev/null 2>&1
    echo "installed $(stat -c %s "$eboot") bytes"
    upload=--no-upload
else
    upload=""
fi
curl -s --disable-epsv --max-time 10 -Q "DELE /data/portal2/trace.txt" "$ftp/" > /dev/null 2>&1 || true
"$here/run_title.sh" $upload --wait 5 > /dev/null 2>&1 || true
sleep "$settle"
before=$(curl -s --disable-epsv --max-time 20 "$ftp/data/portal2/trace.txt" | tr -d '\000' | grep -c "frames:" || true)
sleep "$measure"
curl -s --disable-epsv --max-time 20 "$ftp/data/portal2/trace.txt" | tr -d '\000' | grep "frames:" | tail -n +$((before + 1)) |
    awk '{print; frames += $3; seconds += 10} END {if (seconds) printf "mean: %.1f fps over %d s\n", frames / seconds, seconds; else print "no frames measured"}'
