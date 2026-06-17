# Osirus (Virus) CPU optimization opportunities

_Investigated 2026-06-17. Cursory analysis only — nothing implemented yet. Goal: see what's
worth pursuing, learning from the JV-880 module's ~50% CPU cut
(`../schwung-jv880/docs/2026-06-12-cpu-optimization-analysis.md`)._

## TL;DR

The **expensive stuff is already handled**, so the ceiling is lower than JV-880's was — but
there are a few **proven, low-risk wins Osirus simply never got**. Priority order:

1. **Denormal flush (FTZ/DAZ)** — missing; cheapest, plausibly biggest, zero quality cost.
2. **Build flags** (`-mcpu=cortex-a72` + LTO + visibility) — missing; was JV-880's single largest win.
3. **Thread pin + SCHED_FIFO** — missing; mostly stability/headroom.
4. **DSP-clock % re-tune** — only *after* 1–3, to convert reclaimed CPU into B/C fidelity.
5. Poll/resampler micro-cleanups — opportunistic.

Measure before/after with the existing harness: `src/benchmark/dsp_bench.cpp` (`bench_virus` target).

## Already optimal — do NOT chase

- **JIT is ON and aarch64-native.** `libs/gearmulator/source/dsp56300/source/dsp56kEmu/dspconfig.h:14`
  (`g_jitSupported = true`), `jittypes.h:8` (`HAVE_ARM64`), full ARM64 JIT backend present
  (`jitops_*_aarch64.cpp`), asmjit builds for aarch64. JIT-vs-interpreter is the multi-x lever and
  it's already the fast path. (Unlike JV-880, which is a Nuked-style interpreter.)
- **Resampler is already cheap** — linear interpolation in the render loop
  (`src/dsp/virus_plugin.cpp:~1495`), not the vendored HQ libresample. JV-880's single biggest win
  (ripping out libresample) **does not apply** — Osirus never had that cost.
- **`-O3`** — CMake `Release` already provides it. The gap is LTO / `-mcpu` / visibility (see #2).
- **Block size** `EMU_CHUNK=64` is fine; not a lever.

## Opportunities (prioritized)

### 1. Denormal flush (FTZ/DAZ) on the emu render thread — HIGHEST VALUE, LOW RISK
The Virus filters/reverb/delay-feedback tails decay into denormal floats, which stall hard on ARM.
The child sets **no** FPCR flush bit anywhere (zero `FPCR`/`FZ` hits in `src/`). JV-880 added exactly
this (commit `2d5fd6d`) and called it part of "the bulk of the win."
- **Where:** set `FPCR.FZ` (bit 24) at the top of the forked child's render thread, near
  `child_main` (`src/dsp/virus_plugin.cpp:~1190`), before the emu loop. Copy JV-880's pattern.
- **Impact:** bursty — denormal stalls spike on release/idle reverb tails, exactly where Virus
  emulation is notorious. Workload-dependent few-to-many percent.
- **Risk:** none audible (denormals are sub −300 dB). NOT bit-exact by construction → validate by
  level/ear, not byte-diff.

### 2. Build flags: `-mcpu=cortex-a72` + LTO + visibility — HIGH VALUE, LOW RISK, PROVEN
Osirus compiles the whole DSP56300 JIT emitter / synthLib / virusLib at
`-march=armv8-a -mtune=cortex-a72` with **no LTO, no visibility hardening**
(`cmake/aarch64-toolchain.cmake:16-17`). JV-880's phase-1 flag change (commit `3517083`) was its
single largest measured win.
- **Where:** `cmake/aarch64-toolchain.cmake:16-17` — `-march=armv8-a -mtune=cortex-a72` →
  `-mcpu=cortex-a72`. Add (scoped to the DSP/JIT/synthLib/virusLib targets, not necessarily asmjit):
  `-flto -funroll-loops -fvisibility=hidden -fno-semantic-interposition`.
  **Keep the module's exported entry symbol at default visibility** (JV-880 hit exactly this and had
  to re-export its init symbol).
- **Impact:** smaller than on JV-880 (the hot path is JIT'd machine code, not C++), but the per-block
  dispatch/glue (`dsp.h:176-217`, blockchain lookup) is interpreted C++ and benefits.
- **Risk:** low; LTO can expose latent UB → validate audio after. asmjit + whole-tree LTO can be
  slow/fragile to build → scope LTO if it fights you.

### 3. Pin emu thread + SCHED_FIFO — MEDIUM (stability/headroom), LOW RISK, PROVEN
`<sched.h>` is included (`virus_plugin.cpp:34`) but no `sched_setscheduler`/`sched_setaffinity` runs
in the shipping render path (only in `src/benchmark/dsp_bench.cpp:~223`). JV-880 pins to cores 0–2
(reserving core 3 for the SPI callback) at `SCHED_FIFO` prio 45 (below MoveOriginal's 70).
- **Where:** `child_main` (`virus_plugin.cpp:~1190`), before the emu loop. Copy JV-880 `2d5fd6d`
  (affinity mask cores 0–2, FIFO 45, treat `EPERM` as non-fatal).
- **Risk:** low; don't exceed MoveOriginal's audio priority.

### 4. Virus B/C DSP-clock headroom — TUNING DIAL (quality↔CPU), not "found savings"
The emulated DSP clock is throttled via `EsxiClock::setSpeedPercent`. Defaults: **A=100%, B=45%,
C/other=35%** (`virus_plugin.cpp:~1340`), user-overridable 10–100% (`~:2448`). CPU scales ~linearly.
After 1–3 land, re-tune B/C defaults to convert reclaimed CPU into fidelity (or confirm headroom).
Too low → the Virus DSP underruns its own internal rate (artifacts/detuning), which is why B/C are
already throttled. It's a fidelity tradeoff, not free CPU.

### 5. Render-loop / resampler micro-cleanups — LOW, opportunistic
- `virus_plugin.cpp:~1434` `usleep(500)` when the ring is at target → raise toward 1ms (ring is full
  when it sleeps, so no added latency). ~2k wakeups/s today.
- `virus_plugin.cpp:~1521` per-sample `% AUDIO_RING_SIZE` in the ring write — minor.
Do these alongside the above, not on their own (sub-1pt by JV-880 analogy).

## JV-880 techniques that do NOT transfer
- HQ-resampler replacement (`c98a5e6`) — Osirus already uses cheap linear interp.
- PCM-skip for inactive voices (`858ff5a`), MCU SLEEP fast-forward (`1fafb8c`), interrupt-scan gating
  (`0dadc1d`) — all JV/SH-2-MCU-specific.

## Validation
Use `src/benchmark/dsp_bench.cpp` (`bench_virus`) to measure before/after. Add a null/level-diff
render if possible. Note FTZ (#1) is not bit-exact by design → validate by level, not byte diff.

## Key references
- JV-880 results + copy-able patterns: `../schwung-jv880/docs/2026-06-12-cpu-optimization-analysis.md`,
  commits `3517083` (flags), `2d5fd6d` (FTZ + sched + pin), `c98a5e6` (resampler), `e6cb978` (poll).
- Osirus hot path: `src/dsp/virus_plugin.cpp` (`child_main` ~1190, clock ~1337-1347, emu loop
  ~1427-1524, resampler ~1485-1513), build flags `cmake/aarch64-toolchain.cmake:16-17`,
  JIT confirmation `libs/gearmulator/source/dsp56300/source/dsp56kEmu/dspconfig.h:14` + `jittypes.h:8`.
