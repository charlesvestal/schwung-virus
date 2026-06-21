# Upstream Notes (Osirus fork → upstream prep)

Running log of fork-main commits that are candidates to PR upstream
(`charlesvestal`'s Osirus/Virus module), with what/why and an upstream-readiness
flag, so we don't re-derive each change when opening PRs.

Legend: ✅ ready to PR · 🟡 portable but needs care · 📤 already PR'd upstream.

Most recent first.

---

## v4 remote UI + ROM auto-skin + robustness hardening — ✅ (2026-06-20)

- `221016e` **feat(osirus): port v4 signal-flow remote UI** — replaces the absolute-position
  arranger with a scrolling signal-flow editor (live filter Bode plot, drag-editable ADSR graphs,
  mod-matrix routing list, per-ROM A/B/C skins). Self-contained single-file, no build step.
- `0deb39f` **fix(osirus): expose rom_model to the remote UI** — the manager's getParam is cache-only;
  the state seed now carries `rom_model` so the UI can A/B/C-auto-skin. One-line addition to `get_state`.
- `3989694` **fix(osirus): DSP-clock % sticks + audit hardening** — get_param/get_state report
  `dsp_clock_percent` (intent) not the lagging `dsp_clock_applied`; plus a 2-round static audit's HIGH+safe
  batch: child-crash watchdog + auto-respawn (CR-1), octave-transpose held-note table (no stuck voices),
  cross-process memory barriers (MIDI FIFO / audio ring / current_single — ARM weak ordering), robust panic
  (CC64+CC120+CC123), get_state buffer guard, stale-single reset. All device-verified. Report: `docs/2026-06-20-osirus-static-audit.md`.
- `274df23` **fix(osirus): un-gate osc_fm_mode** — REGRESSION fix: a bulk model-filter pass had gated FM
  Mode to B/C with no justification, removing a working Virus-A control. Reverted to `MODEL_ALL` (matches
  upstream + the reference Osirus plugin).
- **Upstream-regression sweep (fan-out, diff vs `upstream/main` == fork point).** Param set 185↔185 (none
  removed/added); only 2 model-flag changes total (osc_fm_mode above + delay_reverb_mode, evidence-justified).
  Two further regressions found in modified behavioral paths and **closed**:
  - **R1 — empty `get_state` dropped pre-first-preset param edits** (side-effect of the #14 empty-bail).
    Fixed: a `params_user_dirty` flag lets pre-preset edits serialize (referentially, like upstream) while
    still protecting a good self-contained slot during the boot/restart window.
  - **R2 — Virus-A sub-100 `dsp_clock` snapped to 100 on restore** (#15 floor). NOT a regression — A
    genuinely needs 100% (sub-100 starves it); the floor is correct. Made the live setter floor A at 100 too
    (consistency) so a sub-100 A value can't be set then lost.
  **Net after these: the fork→upstream diff is regression-free.** The audit hardening fixes several *latent
  upstream* bugs (crash silence, transpose stuck-notes, IPC races) — portable improvements, not regressions.

---

## Self-contained module presets + preset-load latch fix — ✅ (v0.6.0)

- `405aebd` **feat(osirus): self-contained presets + fix silent preset-load latch**
  — Two-part fix in `src/dsp/virus_plugin.cpp`:
    - **Tier 2 (self-contained state):** `get_param("state")` embeds the full
      512-byte EditBuffer single as hex; `set_param("state")` injects it straight
      to the EditBuffer via a `pending_single` shm handshake, bypassing the fragile
      `(bank, preset)` reference. The child keeps `current_single` continuously
      fresh (throttled `requestSingle(EditBuffer)`), so state reads — including the
      ~10s slot autosave — never block. Slot autosave is now self-contained too.
      Backward compatible: state without an embedded single → referential fallback.
    - **Tier 1 (stop the silent latch):** a Program Change whose
      `child_get_single_preset()` failed used to skip `writeSingle()` but still
      mark the sync done → stuck on the old sound while live param edits still
      worked. Now bounds the preset index by the current bank's *actual* count
      (user banks hold fewer than the global `preset_count`), reports per-bank
      count to the UI, and falls back to preset 0 + logs on a failed lookup.
  — **Why it matters / pairs with host:** this is what makes the host's per-component
    **User Presets** feature (and its live audition/preview) actually work for Osirus
    — Virus patches load referentially, so without the embedded single they broke on
    bank renumbering and didn't preview. Triggered by adding internal preset banks
    (renumbering invalidated saved indices).

---

## Cubic output resampler — ✅

- `f8f71fb` **feat(osirus): 4-point cubic output resampler (replaces linear)**
  — Free output-quality win, A/B-confirmed. The same approach is reusable for the
  jv880/obxd ports.

---

## Virus-A FX gating (delay/reverb) — ✅

The Virus A has only a delay (no reverb / no Dly/Rev mode selector); B/C have both.
These model-gate the FX-mode param and match the remote UI.

- `7d25484` feat(osirus): tidy Delay/Reverb block layout
- `72b1b26` fix(osirus): match remote UI to Virus-A FX gating
- `7ddf417` fix(osirus): hide Dly/Rev Mode on Virus A (model-gate FX mode param)

---

## Preset/bank name sync — ✅

- `d28750e` **fix(osirus): re-sync preset/bank names after a program change**
  — Keeps the displayed preset/bank name in lockstep with the loaded single.

---

## Graphical remote UI — 📤 (submitted upstream as lean PR #2)

Hardware-style web panel (`web_ui.html`), shipped upstream as a **lean** PR that
depends on host remote-UI support (host PR #118). The PR is a consolidated/lean
version; the fork-main history below is the granular set of commits behind it, so
they show as "not in upstream/main" even though the feature is PR'd. Don't re-PR
these individually — track them under PR #2.

- `011bd6b` feat(osirus): compact remote UI + hardware-style controls
- `9c3697c` fix(osirus): preset-follow, bank browser metadata, osc_fm_mode gating
- `5325877` docs(osirus): add remote UI screenshot
- `68a92fd` feat(osirus): graphical remote UI (web_ui.html)
- `0ff880e` feat: compact the remote UI layout
- `2aec564` fix: faster remote-UI load and self-contained jump navigation
- `4c4d371` feat: Virus-hardware visual theme for the remote UI
- `250c299` feat: subtle per-section color coding on remote UI blocks
- `189de34` feat: render bipolar params centered on 0 in the remote UI
- `e48b331` fix: deliver bank/preset browser metadata via state blob
- `d55e96a` feat: graphical remote UI for Osirus with preset-follow fix

---

## Local docs / plans — ⛔ not for upstream

Internal planning/analysis docs, not code; listed for completeness.

- `9c36aa4` **docs(osirus): CPU-optimization opportunities + delay/reverb live-edit bug writeup**
  — `docs/plans/2026-06-17-cpu-optimization-opportunities.md` (prioritized CPU wins
  ported from jv880: FTZ/DAZ denormal flush, `-mcpu`/LTO/visibility, thread pin +
  SCHED_FIFO, B/C clock dial; JIT + resampler already optimal) and
  `2026-06-17-delay-reverb-mode-bug.md`. Note: the delay/reverb-mode issue was
  later resolved by model-gating (Virus A simply has no reverb/mode selector — see
  the `7ddf417`/`72b1b26` gating above), so that bug writeup is largely superseded;
  the CPU-opt notes remain a useful backlog.

---

_Coverage: this file documents all non-merge fork-main commits not in
`upstream/main` from 2026-06-16 onward. Re-check with
`git log --no-merges upstream/main..main` and append new work here._
