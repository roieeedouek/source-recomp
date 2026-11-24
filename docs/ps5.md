# PS5 port (experimental)

Target: a jailbroken PS5 (tested on firmware 13.60) running the game as a native homebrew title
(`/data/homebrew/<TITLE_ID>`, loaded by ShadowMountPlus). Portal 2 boots, plays, saves and renders at
3840x2160, presenting at 119.88 Hz where the display takes it (about 96 fps in a busy chamber with a
profile-guided build).

The console's game data comes from your own copy of Portal 2 (Linux depot), copied to
`/data/portal2/game`; nothing of the game is in this repository.

## Why a title, not an ELF-loader payload

A payload gets no GPU memory heaps from the system (Vulkan reports zero heaps), and a title cannot
`dlopen` anything. So the whole game, the Vulkan driver (RADV), DXVK Native, SDL2 and the console glue
are linked statically into one executable, converted to the console's signed format.

## Dependencies (outside this repository, not committed)

Cloned next to the checkout, in `../deps/`:

- `ps5-vulkan`: RADV for the console (`tools/build-radv.sh release`), its link recipe and the native
  tool that turns the ELF into a title executable. Needs LLD 22 for `-z nodynamic-undefined-weak`.
  Apply `tools/ps5/patches/radv-videoout-120hz.patch` to its RADV source (`.deps/work/radv-src`,
  `patch -p1`) before building: it keeps 119.88 Hz on a variable-refresh display (whose idle floor,
  ~20.9 ms, the unpatched check took for a failed switch) and adds the refresh getter the `fps60`
  switch uses.
- `ps5-vulkan-template`: the payload SDK revision the title links against.
- `ps5-sdk-full`: the payload SDK used to compile the game and the runtime (`PS5_PAYLOAD_SDK`).
- `dxvk-native`, built for the console in `build-ps5` (`ninja -C deps/dxvk-native/build-ps5`).
- `sdl2-ps5` (SDL2 with the console's video, audio and pad drivers) and `ps5-doom3` (the console
  glue those drivers use); built by `tools/ps5/build_title_deps.sh`.

## Build

```sh
python3 tools/gameprep.py                                   # prep (once)
tools/ps5/build_title_deps.sh                               # SDL2 + console glue
python3 tools/build_game.py --target ps5 --release --title  # lift and compile; writes build/game/link.rsp
tools/ps5/build_title.sh                                    # link the title: build/ps5-title-game/PPSA97622
```

Profile-guided optimization (about 13% faster on the console): build the Linux host version with
`--release --pgo-generate`, play a level with `LLVM_PROFILE_FILE=…/portal2-%p.profraw`, merge with
`llvm-profdata merge`, then build the console with `--pgo-use <profdata>`. The generated C and the
compiler are the same on both, so a host profile applies to the console build. (An instrumented console
build is over 1 GB and does not run.)

## Run and diagnose

`PS5_HOST=<console> tools/ps5/run_title.sh` uploads the title over FTP, stops a running instance
(`tools/ps5/killapp.c`, sent to the ELF loader: the shell's `kill` does not reach an app), launches it
through the shell server and prints its log. On the console:

| Path under `/data/portal2` | Use |
|---|---|
| `log.txt` | standard error (written by a thread of its own: a slow write held RADV's submit lock) |
| `trace.txt` | startup steps, frame pacing every 10 s, slow Vulkan calls, stall backtraces |
| `args.txt` | extra game arguments, e.g. `+load <save>` or `+map <map>` |
| `show_fps` | present: frame counter in the top left |
| `fps60` | present: 60 frames a second, paced by the display (each frame shown for two refreshes at 119.88 Hz); checked every second, so it can be switched while the game runs. Needs the RADV with `wsi_videoout_refresh_millihertz` |
| `screenshot.request` | present: the next frame is written to `screen.bmp` (`run_title.sh --shot`) |
| `profile.request` | 10 s of instruction-address samples to `profile.txt` (`run_title.sh --profile`) |

## Console specifics handled in the runtime

- Memory: the guest window is backed by direct memory (a title gets about 12 GiB of it but only about
  420 MiB of flexible memory). DXVK's lock shadows are reused per size class, since mapping and
  unmapping are costly kernel calls (`runtime/d3d9.c`).
- No `/proc`, `chdir` refused, `pipe`/`dup` refused: `/proc/self/exe` is `/app0/eboot.bin`, the working
  directory is kept by the runtime, standard error goes through an in-memory queue.
- Sockets ignore `FIONBIO`/`O_NONBLOCK`: receives and sends on a socket the game made non-blocking carry
  `MSG_DONTWAIT` (otherwise the engine's polled socket blocks the main thread).
- The signal context has 48 more bytes before `uc_mcontext` than FreeBSD's.
- Exit goes through `sceSystemServiceLoadExec("exit")`; returning from `main` crashes.
- `clock_gettime` is not exported to titles: the link binds it to the platform layer's stand-in.

## Limits

- The recompiled game runs one guest thread at a time (the runtime's guest lock), so Source's queued
  rendering gains nothing; the main thread is the frame-rate limit.
- Steam: the offline stub (`runtime/ios/steam_stub*.c`) is linked in; `tools/ps5/gen_dl_table.py` lists its functions for the game's `dlsym` on itself.
