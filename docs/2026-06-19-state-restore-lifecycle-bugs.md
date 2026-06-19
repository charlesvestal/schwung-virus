# State-restore lifecycle bugs (#15 DSP-clock dips, #14 stuck patch)

Date: 2026-06-19
Branch: `fix/state-restore-lifecycle`
Status: **UNTESTED — needs device validation** (code written, not built/deployed)

Two reported bugs share one seam: the `v2_set_param("state", …)` restore path in
`src/dsp/virus_plugin.cpp`. Saved state is applied either immediately (child
already running) or staged as `pending_state` and replayed from
`v2_render_block` after a forked child reports `child_ready`
(`virus_plugin.cpp:2717-2724`).

---

## Bug #15 — loads with A ROM, audible dips/drops ("DSP ~45%")

### Root cause (code-verified)

"45%" is **not** a host CPU meter. `45` is the **Virus B default DSP clock**.
The child auto-selects a per-model DSP clock only when `dsp_clock_percent <= 0`:

- `virus_plugin.cpp:1490-1502`:
  - `case DeviceModel::A: pct = 100;` (A's correct default)
  - `case DeviceModel::B: pct = 45;`
  - `default (C/other): pct = 35;`
- The dynamic clock loop re-applies `dsp_clock_percent` every block when it
  differs from `dsp_clock_applied` (`virus_plugin.cpp:1620-1626`).

A stale clock survives a ROM-switch restart:

- State save always serializes `dsp_clock` (from `dsp_clock_applied`, else 40):
  `virus_plugin.cpp:2614-2616`.
- `kill_child_and_reset` **preserves** `dsp_clock_percent` (comment at
  `:1726`; it zeros `dsp_clock_applied` at `:1759` but NOT `dsp_clock_percent`).
- The **explicit** `rom_index` setter DOES reset it: `shm->dsp_clock_percent = 0;`
  (`virus_plugin.cpp:2167`, now `:2217` after this branch's edits).
- **But** the state-restore ROM-switch branch (`virus_plugin.cpp:2001-2014`)
  set `shm->rom_index = ival;` and triggered the restart **without** that reset.
  A preserved `45` then skips the child's `pct <= 0` auto-select, so a Virus-A
  child runs at 45% clock → underruns its internal rate → audible dips/drops.

### Discrepancy found vs. the original investigation summary (important)

The summary said F1 alone (reset `dsp_clock_percent = 0` in the ROM-switch
branch) is sufficient because "the new child re-derives the correct per-model
default." That is true **only at boot**. But after the restart, `pending_state`
(which still contains the stale `dsp_clock`) is **replayed** by
`v2_render_block` → `v2_set_param("state", …)` (`:2717-2724`). On replay the ROM
already matches, so the ROM-switch branch is skipped and execution falls through
to the self-contained branch (`dsp_clock` applied at the old `:2037-2039`) or the
legacy branch (old `:2081-2083`) — **both of which re-applied the stale clock
verbatim**, clamped only to `[10,100]`, with no model guard. So F1 by itself is
necessary-but-not-sufficient: the replay re-clobbers the freshly-derived
default. This required the additional guard (F2) below.

### Fix implemented

**F1 — reset clock to auto on ROM-switch restore** (`virus_plugin.cpp` ~`:2003`):
mirrors the explicit `rom_index` setter so the restarting child re-derives the
correct per-model default during the gap before replay.

Before:
```c
if (ival >= 0 && (...) && ival != shm->rom_index) {
    shm->rom_index = ival;
    if (inst->pending_state) free(inst->pending_state);
```
After:
```c
if (ival >= 0 && (...) && ival != shm->rom_index) {
    shm->rom_index = ival;
    /* Bug #15: reset DSP clock to auto ... */
    shm->dsp_clock_percent = 0;
    if (inst->pending_state) free(inst->pending_state);
```

**F2 — model-guarded restored clock** (new helper `apply_restored_dsp_clock`,
`virus_plugin.cpp` ~`:1986-2010`; called from both restore branches, ~`:2078`
and ~`:2127`). A restored clock is applied only if it is at or above the loaded
model's safe floor; otherwise it resets to auto (`0`) so the child picks the
model default. Floors: A=100, B=45, C/other=35, read from
`shm->rom_model_name[0]` (populated by the child before `child_ready`, so valid
on the post-restart replay too).

Before (both branches):
```c
if (json_get_int(val, "dsp_clock", &ival) == 0) {
    if (ival < 10) ival = 10; if (ival > 100) ival = 100;
    shm->dsp_clock_percent = ival;
}
```
After (both branches):
```c
if (json_get_int(val, "dsp_clock", &ival) == 0) {
    apply_restored_dsp_clock(shm, ival);  /* Bug #15: model-guarded */
}
```

F2 was implemented (rather than deferred) because, per the discrepancy above, F1
alone does not survive the state replay. It does **not** touch the explicit
user-driven `dsp_clock` setter (`virus_plugin.cpp:2227-2231`), so legitimate user
overrides keep their exact prior semantics — the guard fires only on the
state-restore path. F1 and F2 are complementary belt-and-suspenders: F1 fixes the
boot gap, F2 fixes the replay.

---

## Bug #14 — stuck on a weird patch after a reset; preset loads ignored

### Root cause (code-verified)

The self-contained restore branch (`virus_plugin.cpp:2017-2047`):

- injects the saved 512-byte single into the EditBuffer (via `pending_single` +
  `pending_single_req_gen`, `:2030-2032`),
- sets `shm->current_bank` / `shm->current_preset` to the saved indices
  (`:2026-2029`),
- **never issues a Program Change** to load that ROM slot, then `return;`s
  (`:2046`).

So `current_bank`/`current_preset` now name a slot that was never actually
loaded — the audio is the injected edit-buffer single, not that ROM preset.

Subsequent loads are then silently dropped by "already current" no-op guards:

- `set_param("preset")` guard: `if (idx == shm->current_preset) { … return; }`
  (`virus_plugin.cpp:2109-2112`).
- `set_param("bank_index")` guard: `if (idx == shm->current_bank) { … return; }`
  (`virus_plugin.cpp:2126-2129`).

Because the requested index equals the (untruthful) current index, every
matching load is a no-op → "stuck, won't change."

### Fix implemented (safe, localized part)

Added a parent-side `int force_next_load;` to `virus_instance_t`
(`virus_plugin.cpp` ~`:826`; the instance is `calloc`'d at `:1810` so it is
zero-initialized). The self-contained restore branch sets `inst->force_next_load
= 1;` after injecting the single. Both no-op guards now read it:

```c
if (idx == shm->current_preset && !inst->force_next_load) { … return; }
inst->force_next_load = 0;
```
(and the analogous `current_bank` guard). The first preset/bank selection after a
self-contained restore therefore always issues a real load; the flag self-clears.

**Why a flag instead of `current_preset = -1`:** `current_preset` is a signed
`volatile int` (`:603`), so `-1` is type-safe, **but** it leaks into several
consumers — `get_param("preset")` returns it raw (`:2534`), `patch_in_bank`
returns `current_preset + 1` (`:2545` → would show `0`), state save serializes it
(`:2613`), and `shm_lookup_preset_name` uses it for name lookup (`:763-765`).
A sentinel `-1` would produce a wrong UI/state/name read. The parent-side
`force_next_load` flag avoids all of that and only affects the two guards.

---

## DEFERRED (do NOT implement without explicit go-ahead)

### H2 — validate the captured single before embedding it (Bug #14 secondary)

The embedded single can be captured mid-"preset upgrade" via
`requestSingle(EditBuffer)`
(`libs/gearmulator/source/virusLib/microcontroller.cpp:950`, async overwrite at
`:1381-1419`), yielding a garbage/partial single that then gets saved and
re-injected — another route to a "weird patch."

Recommended fix (next session): in `child_handle_single_io` (~`virus_plugin.cpp`
`:1063-1075`, where the child dumps the live EditBuffer into `current_single`),
validate the captured buffer with `microcontroller.cpp` `isValid()`
(`:1421`, requires byte 240 ∈ [32,127]) before accepting it; reject/retry on
failure rather than embedding it.

**Deferred because** it touches the vendored submodule `libs/gearmulator` (and/or
needs an `isValid` shim into the child capture path), which is out of scope for
this branch. The submodule pointer is intentionally left untouched.

### F2 variant note

F2 as built lives at the parent restore seam (not in the child clock-application
path the summary originally suggested at `:1490-1502`/`:1620-1626`). The
parent-seam version is preferable: the parent already knows the loaded model via
`rom_model_name`, the guard only touches the restore path, and the child's own
auto-select / dynamic loop are left exactly as-is (no risk to live user
overrides). No additional child-side clamp was added; if device testing still
shows a stale clock landing, a child-side clamp in the dynamic loop
(`:1620-1626`) is the recommended follow-up — but only after confirming it can't
override a deliberate user `dsp_clock`.

---

## UNTESTED — needs device validation

These are code changes only; not built, not deployed, not run through gearmulator.

### Build/deploy

Build the module and deploy per the repo's normal flow, then load the Virus
slot. (Per workspace rules, a fresh `dsp.so` loads on next instantiation; if the
slot is already loaded, swap it out/in or restart.)

### #15 repro / measurement

1. Save state while a Virus **B** ROM is loaded (so saved `dsp_clock` = ~45),
   then restore that state with the slot switching to the **A** ROM (the
   ROM-switch restore branch). Also test plain reload on an A ROM.
2. After load settles, read the **DSP-Clock** value in Settings.
   - Expected after fix: **100** on Virus A (was 45 → dips/drops).
   - Expected: B stays 45, C stays 35.
3. Listen: sustained/poly patch on A should hold without periodic dips.

### #14 repro / measurement

1. Trigger a self-contained state restore (Schwung reset / reload of a saved
   state that embeds a `single`).
2. Note the patch (it'll be the injected edit-buffer single).
3. Select the **same** preset index that `current_preset` reports.
   - Expected after fix: it now reloads and the sound changes (was a silent
     no-op → "stuck").
4. Repeat the same-index test for **bank_index**.
5. Confirm the flag self-clears: a *second* selection of an already-current
   index is once again a correct no-op (no double-load).

### Open questions raised by the investigation

- Confirm on-device that the reported "45%" is the **DSP-Clock** Settings param,
  not a host/CPU meter (the diagnosis assumes the former).
- Confirm a B (or C) ROM was actually selected at some point in the failing
  session (i.e. the stale clock truly came from a different model). If the repro
  only ever uses A, the stale clock may have another origin worth tracing.
- Determine what currently *unsticks* #14 on device today (e.g. selecting a
  different index, switching banks) — useful to confirm the no-op guards are the
  real culprit and the flag fix covers every entry path.
- Verify `rom_model_name` is populated (non-empty, correct letter) at the moment
  the post-ROM-switch replay runs, so the F2 floor lookup resolves to the right
  model rather than defaulting to the C/35 branch.
