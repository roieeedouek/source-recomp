# Plan: running guest threads in parallel

Branch `parallel-guest`. Goal: let the recompiled game's threads run at the same time on the PS5
(and Linux x86-64), so Source's render thread and job threads stop taking turns with the main thread.
Target: a locked 120 fps in the chambers that run at ~86–96 fps today.

## Where the frame time goes today (PS5, profile-guided build)

- The frame is guest code, one thread at a time: every guest thread runs under one mutex, the guest
  gate (`runtime/scheduler.c`), released only at blocking calls and at yield points.
- The main thread waits ~28% of the frame for the game's other guest threads (job, sound) to take
  their turn; the gate itself is rarely contended (0.4–0.8%).
- Host-side threads are idle: DXVK's command-recording thread 86%, the GPU-submission thread 99%.
- ~25% of the main thread is rendering code (shaderapivk, materialsystem, stdshader, the D3D9 bridge)
  that Source moves to its own thread with `mat_queue_mode 2` when it sees several CPUs. Today the
  runtime reports one CPU (`P2_GUEST_CPUS`, default 1), and `docs/usage.md` records that 2 or 4 were
  slower: more threads, still serialized.

Expected gain if the render thread and the job threads run in parallel: 1.3–1.6× on the main thread's
frame, enough for 120 fps in most chambers. That is an estimate to check in step 1.

## What makes this hard

1. **Memory ordering of lifted code.** Guest loads and stores are plain C accesses into the guest
   window. The original x86 code relied on x86 ordering (TSO: loads not reordered with loads, stores
   not with stores) and on its compiler's barriers; the C compiler compiling the lifted code may
   reorder, merge or drop plain accesses as soon as two threads touch the same memory. Under the gate
   this cannot be observed; in parallel, lock-free code in the game or its libraries (spin locks
   released with a plain store, flags, ring buffers) can break.
   Guest atomic instructions (`lock` prefix, `xchg`, `cmpxchg`, `xadd`) are already real
   sequentially consistent host atomics (`rt_atomic32`, `rt_compare_exchange32/64`, `rt.h`).
2. **Runtime state that relies on the gate.** About 165 mutable file-level globals in 32 runtime files.
   Documented cases: per-call-site caches (`RtSiteCache`, two fields read without synchronization,
   `rt.h`), the guest TLS cache (`tls.c`), the thread registry (`pthreads.c`), the guest memory map
   (`vm.c`), the D3D9 bridge's tables and counters (`d3d9.c`), the PE loader and Miles (`pe.c`),
   the heap (`heap.c`, already has its own lock). Undocumented ones are the main risk.
3. **Host libraries called from several guest threads**: DXVK's D3D9 device (safe if only Source's
   render thread uses it, which is what queued mode does), SDL, OpenAL, the Steam stub.

## Approach: from one gate to a reader–writer gate

Replace the mutex with a reader–writer gate and move code from the exclusive side to the shared side
one piece at a time:

- Running guest code takes the gate **shared**: several guest threads run at once.
- Anything not yet made thread-safe takes it **exclusive**: exactly today's behaviour (all guest
  threads stopped at a yield point). Writers set a flag that `rt_poll` (already inlined at loop back
  edges and calls) checks, so running threads step aside within one poll interval.
- A build option keeps the plain mutex (`RT_SERIAL_GUEST`), so every step can be compared and
  bisected, and macOS (ARM, where x86 ordering costs real barriers) can stay serial.

At every step the game keeps working: unaudited shims and runtime paths simply stay exclusive.

## Steps

Each step ends with the PS5 benchmark (`tools/ps5/bench_title.sh`, the two saved chambers),
a play session, and the host tests.

0. **Groundwork.**
   - Make the C checks (`make check`) build on Linux (the `linux` parameter name in `rt.h` collides
     with GNU mode's predefined macro), so the runtime can be tested here, including under
     ThreadSanitizer.
   - Count the guest threads Portal 2 starts and what each one does with `P2_GUEST_CPUS` at 1, 2, 4:
     thread starts, per-thread guest time (the profiler, extended to every guest thread).
1. **Measure the ceiling before building it.** With the current serial gate and `P2_GUEST_CPUS=4`,
   profile how much guest work moves to the render thread under `mat_queue_mode 2`. If the render
   thread's share is small, the expected gain shrinks and the plan stops here (a few days of work, no
   risk taken).
2. **Memory model for generated code (x86-64 hosts).** Emit guest loads as acquire and guest stores
   as release (`__atomic_load_n`/`__atomic_store_n`). On x86-64 these compile to the same `mov`
   instructions; they only stop the compiler from reordering across them, which gives the lifted code
   x86's own ordering guarantees. Accesses relative to the guest stack pointer stay plain (a stack is
   private to its thread; the rare shared stack slot is a known risk to check). Measure the cost with
   the serial gate still in place: this step alone must not lose more than ~5%.
3. **Reader–writer gate.** Shared/exclusive gate with the writer flag polled by `rt_poll`; every
   shim and runtime entry point exclusive by default. Behaviour must be identical to today (only one
   thread runs while any thread is in an exclusive section). Benchmark: no change expected.
4. **Make the hot paths shared, one at a time**, each with a check in `tests/checks_*.c`:
   - per-call-site caches: one atomic pointer to an immutable `(target, fn)` entry instead of two
     fields;
   - guest TLS: per-thread cache instead of the shared `tls_cache`;
   - the hottest imports (`___tls_get_addr`, `memcpy`, `memcmp`, `pthread_self`, `strcmp`, maths):
     stateless or per-thread already, so they can run shared once audited;
   - guest heap (`heap.c` has its own lock), thread registry, guest memory map (its own lock);
   - errno and other per-thread shim state into the thread's CPU structure.
   The game runs shared only between these paths; everything else still stops the world.
5. **Turn on parallelism in Source**: report several CPUs (`P2_GUEST_CPUS`, CPUID and
   `/proc/cpuinfo` together), `mat_queue_mode 2`. The D3D9 bridge stays exclusive at first, then is
   made shared for the render thread only (one D3D9 caller at a time is what DXVK expects).
6. **Widen and harden.** Move the remaining frequent shims to shared as profiles show them hot; long
   play sessions through several chapters, level loads, saves, audio; ThreadSanitizer on the host
   runtime (not on generated code, whose races are by design).
7. **Defaults.** Parallel on the PS5 and Linux x86-64 when it is faster everywhere measured; serial
   stays available (`RT_SERIAL_GUEST`) and remains the macOS default.

## Risks and how they are caught

| Risk | Detection | Fallback |
|---|---|---|
| Lifted code relies on an ordering acquire/release does not give (a store–load fence written as a plain instruction sequence) | Hangs or corruption in play sessions; a stress test of the game's own spin locks | Treat the pattern in the lifter; keep that module serial |
| A runtime global missed by the audit | ThreadSanitizer on the host runtime; random crashes under parallel only | Make the path exclusive again (one line) |
| Acquire/release costs more than it saves | Step 2's benchmark | Limit it to non-stack accesses only (planned) or to modules known to share data |
| Source's queued renderer misbehaves on this port | Step 1 already runs it, serially | Keep `mat_queue_mode 0`, keep job threads parallel only |
| Heisenbugs that only show on the console | Same build on Linux x86-64 first, the PS5 crash report (faulting address, thread) | `RT_SERIAL_GUEST` build |

## Out of scope

- macOS / iOS on ARM (weaker hardware ordering): stays serial in this plan.
- The GPU side: it is idle at today's frame rates.
