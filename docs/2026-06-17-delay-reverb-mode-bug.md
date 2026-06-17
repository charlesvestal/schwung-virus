# Delay/Reverb Mode live-edit bug — diagnosis + proposed fix (resume here)

_Investigated 2026-06-17, on-device confirmed. Fix NOT yet implemented — pick up here._

## Symptom (confirmed on device)
- ROM **Bank 3 / Preset 30 "Out-of RP"** audibly plays **Reverb**, but the UI shows
  `delay_reverb_mode` = **26 ("Pattern X+Y")**.
- Changing `delay_reverb_mode` from the UI (e.g. **Reverb (2) → Delay (1)**) on that loaded patch
  produces **NO audible difference**. So the live FX-mode write is not taking effect.
- By contrast, **presets DO change the delay/reverb audibly**, and ordinary params (cutoff, etc.)
  both display correctly and are audibly live-editable.

## What is NOT the bug (ruled out, source-backed)
- **Addressing is correct.** `delay_reverb_mode` = PAGE_A (0x70), index 112, matches gearmulator
  `DELAY_REVERB_MODE=112` (`libs/gearmulator/source/virusLib/microcontrollerTypes.h:89`). Single-dump
  byte 112 IS the mode (name at bytes 240-249 ⇒ Page A = bytes 0-127; `microcontroller.cpp:1287`
  `applyToSingleEditBuffer` PAGE_A offset = param). Desktop Osirus reads/writes the same page-A 112.
- **Labels are correct** — our `opts_delay_reverb_mode` (`src/dsp/virus_plugin.cpp:192`) matches
  gearmulator's authoritative `delayReverbMode` value list exactly (0 Off,1 Delay,2 Reverb,…,26
  "Pattern X+Y"). The "Pattern X+Y" shown for an old factory reverb preset is the **faithful raw
  byte 112**; the static modern label table just mis-glosses old-OS encodings (desktop Osirus shows
  the same — upstream limitation, not ours).
- **Not global-vs-single.** The page-A→`MD_DELAY_MODE`/page-C mirror (`applyToMultiEditBuffer`) only
  runs when `PLAY_MODE != Single` (`microcontroller.cpp:817`). We run single mode, so the single's
  byte 112 is the source of truth — confirmed.
- **Our send path reaches the DSP.** `v2_set_param` → `send_param_midi` → `pending_params[112]` →
  `child_drain_pending_params` (`src/dsp/virus_plugin.cpp:925`) builds `{F0 00 20 33 01 OMNI 0x70
  0x00 112 val F7}` → `mc->sendSysex` → single-mode branch (`microcontroller.cpp:851-855` part==0)
  → `applyToSingleEditBuffer(SINGLE)` + `send(0x70,0,112,val)` (`:858`). **Cutoff uses this exact
  path (only cc differs) and is audible**, so the page-A 112 write does reach the firmware.

## Leading hypothesis (what to fix)
The emulated firmware appears to **(re)build the delay/reverb engine only on a full single load, not
on a bare FX-param write**. That's why preset loads (which `mc->writeSingle(EditBuffer, SINGLE, …)`,
`virus_plugin.cpp:1001`) change the FX audibly, but a live param-change of byte 112 updates the
edit-buffer mirror without re-rendering the FX.

## Proposed experiment / fix (try first in the new session)
On an **FX-section param change (indices 112-119)** — at minimum `delay_reverb_mode` (112) and
`effect_send` (113) — instead of (or in addition to) the bare `send_param_midi`, **re-push the whole
single edit buffer**:
1. In the child, read back the current edit-buffer single: `mc->requestSingle(EditBuffer, SINGLE, out)`.
2. Patch the changed byte(s) in `out` (page-A → `out[cc]`).
3. `mc->writeSingle(EditBuffer, SINGLE, out)` — the proven-audible path.

If that makes mode changes audible → cause + fix confirmed. If NOT → it's deeper (value encoding /
firmware), instrument `child_drain_pending_params` to log the actual bytes sent and compare to a
preset-load, and check whether the desktop Osirus re-pushes or relies on real-HW live recompute.

Validate by ear on Bank3/Preset30: Reverb(2) vs Delay(1) must become clearly different.

## Key files
- `src/dsp/virus_plugin.cpp`: param table @192/464, `send_param_midi` @757, `child_drain_pending_params`
  @925, preset load + `writeSingle` @996-1003, `child_sync_params_from_preset` (read-back) @901,
  `v2_set_param` FX write @2000-2007.
- `libs/gearmulator/source/virusLib/microcontroller.cpp`: page-A SysEx handler @775-859, `send()`
  @401, `writeSingle`/`sendPreset` @254-343, `requestSingle` (read edit buffer) — grep it.
- See also memory `[[schwung-virus-remote-ui]]` (params/delay-reverb section).
