# Full Virus Parameter Coverage Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Expose all ~100 sound design parameters from the Access Virus DSP engine, with model-aware filtering (A vs B/C), enabling full sound design from the Move UI.

**Architecture:** Expand the `g_params[]` table to include all public parameters from pages A and B of `parameterDescriptions_C.json`. Add a `page` field so page-A params send MIDI CC (`0xB0`) and page-B params send MIDI Polypressure (`0xA0`). Enable Page B polypressure in the Microcontroller during child init. Add a `model_min` field to filter params by detected ROM model. Expand shared memory to track 256 CC values (128 page A + 128 page B). Rebuild the `ui_hierarchy` JSON to organize params into 12 logical groups.

**Tech Stack:** C++ (virus_plugin.cpp), JSON (module.json), Gearmulator DSP56300 engine

---

## Key Reference Files

| File | Role |
|------|------|
| `src/dsp/virus_plugin.cpp` | Core plugin — params, shared memory, MIDI, state, UI hierarchy |
| `src/module.json` | Module metadata, chain_params |
| `libs/gearmulator/source/osirusJucePlugin/parameterDescriptions_C.json` | Canonical param definitions (page, index, name, min, max, isPublic) |
| `libs/gearmulator/source/virusLib/microcontroller.cpp:232` | Where Page B polypressure is disabled by default |
| `libs/gearmulator/source/virusLib/microcontroller.cpp:396-399` | `isPolyPressureForPageBEnabled()` check |
| `libs/gearmulator/source/virusLib/microcontrollerTypes.h:84` | `MIDI_CONTROL_HIGH_PAGE = 95` enum |

## Key Technical Details

- **Page A** params are sent as MIDI CC: `{0xB0, index, value}` (already works)
- **Page B** params are sent as MIDI Polypressure: `{0xA0, index, value}` (new)
- Page B is gated by `MIDI_CONTROL_HIGH_PAGE` global setting — must be set to `1` during init
- Preset data stores page B values at offset +128 in the single buffer (see `microcontroller.cpp:682`)
- Model detection: `shm->rom_model_name` is "A", "B", or "C" — set during child boot
- Model A = DeviceModel::A, Model B/C have extra features (osc3, phaser, distortion, EQ, mod slots 4-6, vocoder)

## Parameter Inventory

### Page A (CC 0xB0) — All Models

| Key | CC | Name | Min | Max | Notes |
|-----|-----|------|-----|-----|-------|
| osc1_shape | 17 | Osc1 Shape | 0 | 127 | bipolar |
| osc1_pulsewidth | 18 | Osc1 Pulsewidth | 0 | 127 | |
| osc1_wave_select | 19 | Osc1 Wave Select | 0 | 63 | discrete |
| osc1_semitone | 20 | Osc1 Semitone | 16 | 112 | bipolar, maps to -48..+48 |
| osc1_keyfollow | 21 | Osc1 Keyfollow | 0 | 127 | bipolar, default 96 |
| osc2_shape | 22 | Osc2 Shape | 0 | 127 | bipolar |
| osc2_pulsewidth | 23 | Osc2 Pulsewidth | 0 | 127 | |
| osc2_wave_select | 24 | Osc2 Wave Select | 0 | 63 | discrete |
| osc2_semitone | 25 | Osc2 Semitone | 16 | 112 | bipolar |
| osc2_detune | 26 | Osc2 Detune | 0 | 127 | default 32 |
| osc2_fm_amount | 27 | Osc2 FM Amount | 0 | 127 | |
| osc2_sync | 28 | Osc2 Sync | 0 | 1 | bool |
| osc2_filt_env_amt | 29 | Osc2 Filt Env Amt | 0 | 127 | bipolar |
| fm_filt_env_amt | 30 | FM Filt Env Amt | 0 | 127 | bipolar |
| osc2_keyfollow | 31 | Osc2 Keyfollow | 0 | 127 | bipolar, default 96 |
| osc_balance | 33 | Osc Balance | 0 | 127 | bipolar |
| sub_osc_volume | 34 | Suboscillator Volume | 0 | 127 | |
| sub_osc_shape | 35 | Suboscillator Shape | 0 | 1 | bool |
| osc_mainvolume | 36 | Osc Mainvolume | 0 | 127 | default 64 |
| noise_volume | 37 | Noise Volume | 0 | 127 | |
| ringmod_volume | 38 | Ringmodulator Volume | 0 | 127 | |
| noise_color | 39 | Noise Color | 0 | 127 | bipolar |
| cutoff | 40 | Cutoff | 0 | 127 | default 127 |
| cutoff2_offset | 41 | Cutoff2 Offset | 0 | 127 | bipolar, default 64 |
| filter1_resonance | 42 | Filter1 Resonance | 0 | 127 | |
| filter2_resonance | 43 | Filter2 Resonance | 0 | 127 | |
| filter1_env_amt | 44 | Filter1 Env Amt | 0 | 127 | |
| filter2_env_amt | 45 | Filter2 Env Amt | 0 | 127 | |
| filter1_keyfollow | 46 | Filter1 Keyfollow | 0 | 127 | bipolar |
| filter2_keyfollow | 47 | Filter2 Keyfollow | 0 | 127 | bipolar |
| filter_balance | 48 | Filter Balance | 0 | 127 | bipolar |
| saturation_curve | 49 | Saturation Curve | 0 | 14 | discrete |
| filter1_mode | 51 | Filter1 Mode | 0 | 7 | discrete |
| filter2_mode | 52 | Filter2 Mode | 0 | 3 | discrete |
| filter_routing | 53 | Filter Routing | 0 | 3 | discrete |
| flt_attack | 54 | Flt Attack | 0 | 127 | |
| flt_decay | 55 | Flt Decay | 0 | 127 | |
| flt_sustain | 56 | Flt Sustain | 0 | 127 | |
| flt_sustain_time | 57 | Flt Sustain Time | 0 | 127 | bipolar |
| flt_release | 58 | Flt Release | 0 | 127 | |
| amp_attack | 59 | Amp Attack | 0 | 127 | |
| amp_decay | 60 | Amp Decay | 0 | 127 | |
| amp_sustain | 61 | Amp Sustain | 0 | 127 | |
| amp_sustain_time | 62 | Amp Sustain Time | 0 | 127 | bipolar |
| amp_release | 63 | Amp Release | 0 | 127 | |
| lfo1_rate | 67 | LFO1 Rate | 0 | 127 | |
| lfo1_shape | 68 | LFO1 Shape | 0 | 67 | discrete |
| lfo1_env_mode | 69 | LFO1 Env Mode | 0 | 1 | bool |
| lfo1_mode | 70 | LFO1 Mode | 0 | 1 | bool |
| lfo1_symmetry | 71 | LFO1 Symmetry | 0 | 127 | bipolar |
| lfo1_keyfollow | 72 | LFO1 Keyfollow | 0 | 127 | |
| lfo1_keytrigger | 73 | LFO1 Keytrigger | 0 | 127 | |
| osc1_lfo1_amount | 74 | Osc1 LFO1 Amount | 0 | 127 | bipolar |
| osc2_lfo1_amount | 75 | Osc2 LFO1 Amount | 0 | 127 | bipolar |
| pw_lfo1_amount | 76 | PW LFO1 Amount | 0 | 127 | bipolar |
| reso_lfo1_amount | 77 | Reso LFO1 Amount | 0 | 127 | bipolar |
| filtgain_lfo1_amount | 78 | FiltGain LFO1 Amount | 0 | 127 | bipolar |
| lfo2_rate | 79 | LFO2 Rate | 0 | 127 | |
| lfo2_shape | 80 | LFO2 Shape | 0 | 67 | discrete |
| lfo2_env_mode | 81 | LFO2 Env Mode | 0 | 1 | bool |
| lfo2_mode | 82 | LFO2 Mode | 0 | 1 | bool |
| lfo2_symmetry | 83 | LFO2 Symmetry | 0 | 127 | bipolar |
| lfo2_keyfollow | 84 | LFO2 Keyfollow | 0 | 127 | |
| lfo2_keytrigger | 85 | LFO2 Keytrigger | 0 | 127 | |
| shape_lfo2_amount | 86 | Shape LFO2 Amount | 0 | 127 | bipolar |
| fm_lfo2_amount | 87 | FM LFO2 Amount | 0 | 127 | bipolar |
| cutoff1_lfo2_amount | 88 | Cutoff1 LFO2 Amount | 0 | 127 | bipolar |
| cutoff2_lfo2_amount | 89 | Cutoff2 LFO2 Amount | 0 | 127 | bipolar |
| pan_lfo2_amount | 90 | Pan LFO2 Amount | 0 | 127 | bipolar |
| patch_volume | 91 | Patch Volume | 0 | 127 | default 100 |
| transpose | 93 | Transpose | 0 | 127 | bipolar |
| key_mode | 94 | Key Mode | 0 | 5 | discrete |
| unison_mode | 97 | Unison Mode | 0 | 15 | discrete |
| unison_detune | 98 | Unison Detune | 0 | 127 | |
| unison_pan_spread | 99 | Unison Pan Spread | 0 | 127 | |
| unison_lfo_phase | 100 | Unison LFO Phase | 0 | 127 | bipolar |
| input_mode | 101 | Input Mode | 0 | 3 | discrete |
| chorus_mix | 105 | Chorus Mix | 0 | 127 | |
| chorus_rate | 106 | Chorus Rate | 0 | 127 | |
| chorus_depth | 107 | Chorus Depth | 0 | 127 | |
| chorus_delay | 108 | Chorus Delay | 0 | 127 | |
| chorus_feedback | 109 | Chorus Feedback | 0 | 127 | bipolar |
| chorus_lfo_shape | 110 | Chorus LFO Shape | 0 | 67 | discrete |
| delay_reverb_mode | 112 | Delay/Reverb Mode | 0 | 26 | discrete |
| effect_send | 113 | Effect Send | 0 | 127 | |
| delay_time | 114 | Delay Time | 0 | 127 | |
| delay_feedback | 115 | Delay Feedback | 0 | 127 | |
| delay_rate_rev_decay | 116 | Delay Rate/Rev Decay | 0 | 127 | |
| delay_depth | 117 | Delay Depth | 0 | 127 | |
| delay_lfo_shape | 118 | Delay LFO Shape | 0 | 5 | discrete |
| delay_color | 119 | Delay Color | 0 | 127 | bipolar |
| panorama | 10 | Panorama | 0 | 127 | bipolar |
| portamento_time | 5 | Portamento Time | 0 | 127 | |

### Page B (Polypressure 0xA0) — All Models

| Key | Index | Name | Min | Max | Notes |
|-----|-------|------|-----|-----|-------|
| arp_mode | 1 | Arp Mode | 0 | 6 | discrete |
| arp_pattern | 2 | Arp Pattern | 0 | 63 | discrete |
| arp_octave_range | 3 | Arp Octave Range | 0 | 3 | discrete |
| arp_hold | 4 | Arp Hold Enable | 0 | 1 | bool |
| arp_note_length | 5 | Arp Note Length | 0 | 127 | bipolar |
| arp_swing | 6 | Arp Swing | 0 | 127 | |
| lfo3_rate | 7 | LFO3 Rate | 0 | 127 | |
| lfo3_shape | 8 | LFO3 Shape | 0 | 67 | discrete |
| lfo3_mode | 9 | LFO3 Mode | 0 | 1 | bool |
| lfo3_keyfollow | 10 | LFO3 Keyfollow | 0 | 127 | |
| lfo3_destination | 11 | LFO3 Destination | 0 | 5 | discrete |
| osc_lfo3_amount | 12 | Osc LFO3 Amount | 0 | 127 | |
| lfo3_fadein_time | 13 | LFO3 Fade-In Time | 0 | 127 | |
| clock_tempo | 16 | Clock Tempo | 0 | 127 | maps to BPM |
| arp_clock | 17 | Arp Clock | 0 | 17 | discrete |
| lfo1_clock | 18 | LFO1 Clock | 0 | 21 | discrete |
| lfo2_clock | 19 | LFO2 Clock | 0 | 21 | discrete |
| delay_clock | 20 | Delay Clock | 0 | 16 | discrete |
| lfo3_clock | 21 | LFO3 Clock | 0 | 21 | discrete |
| control_smooth_mode | 25 | Control Smooth Mode | 0 | 3 | discrete |
| bender_range_up | 26 | Bender Range Up | 0 | 127 | bipolar |
| bender_range_down | 27 | Bender Range Down | 0 | 127 | bipolar |
| bender_scale | 28 | Bender Scale | 0 | 1 | bool |
| filter1_env_polarity | 30 | Filter1 Env Polarity | 0 | 1 | bool |
| filter2_env_polarity | 31 | Filter2 Env Polarity | 0 | 1 | bool |
| filter2_cutoff_link | 32 | Filter2 Cutoff Link | 0 | 1 | bool |
| filter_keytrack_base | 33 | Filter Keytrack Base | 0 | 127 | discrete |
| osc_fm_mode | 34 | Osc FM Mode | 0 | 12 | discrete |
| osc_init_phase | 35 | Osc Init Phase | 0 | 127 | |
| punch_intensity | 36 | Punch Intensity | 0 | 127 | |
| osc1_shape_velocity | 47 | Osc1 Shape Velocity | 0 | 127 | bipolar |
| osc2_shape_velocity | 48 | Osc2 Shape Velocity | 0 | 127 | bipolar |
| pulsewidth_velocity | 49 | PulseWidth Velocity | 0 | 127 | bipolar |
| fm_amount_velocity | 50 | FM Amount Velocity | 0 | 127 | bipolar |
| flt1_envamt_velocity | 54 | Flt1 EnvAmt Velocity | 0 | 127 | bipolar |
| flt2_envamt_velocity | 55 | Flt2 EnvAmt Velocity | 0 | 127 | bipolar |
| resonance1_velocity | 56 | Resonance1 Velocity | 0 | 127 | bipolar |
| resonance2_velocity | 57 | Resonance2 Velocity | 0 | 127 | bipolar |
| amp_velocity | 60 | Amp Velocity | 0 | 127 | bipolar |
| panorama_velocity | 61 | Panorama Velocity | 0 | 127 | bipolar |
| assign1_source | 64 | Assign1 Source | 0 | 27 | discrete |
| assign1_destination | 65 | Assign1 Destination | 0 | 122 | discrete |
| assign1_amount | 66 | Assign1 Amount | 0 | 127 | bipolar |
| assign2_source | 67 | Assign2 Source | 0 | 27 | discrete |
| assign2_dest1 | 68 | Assign2 Destination1 | 0 | 122 | discrete |
| assign2_amount1 | 69 | Assign2 Amount1 | 0 | 127 | bipolar |
| assign2_dest2 | 70 | Assign2 Destination2 | 0 | 122 | discrete |
| assign2_amount2 | 71 | Assign2 Amount2 | 0 | 127 | bipolar |
| assign3_source | 72 | Assign3 Source | 0 | 27 | discrete |
| assign3_dest1 | 73 | Assign3 Dest1 | 0 | 122 | discrete |
| assign3_amount1 | 74 | Assign3 Amount1 | 0 | 127 | bipolar |
| assign3_dest2 | 75 | Assign3 Dest2 | 0 | 122 | discrete |
| assign3_amount2 | 76 | Assign3 Amount2 | 0 | 127 | bipolar |
| assign3_dest3 | 77 | Assign3 Dest3 | 0 | 122 | discrete |
| assign3_amount3 | 78 | Assign3 Amount3 | 0 | 127 | bipolar |
| lfo1_assign_dest | 79 | LFO1 Assign Dest | 0 | 122 | discrete |
| lfo1_assign_amount | 80 | LFO1 Assign Amount | 0 | 127 | bipolar |
| lfo2_assign_dest | 81 | LFO2 Assign Dest | 0 | 122 | discrete |
| lfo2_assign_amount | 82 | LFO2 Assign Amount | 0 | 127 | bipolar |
| filter_select | 122 | Filter Select | 0 | 2 | discrete |

### Page B — B/C Models Only

| Key | Index | Name | Min | Max | Notes |
|-----|-------|------|-----|-----|-------|
| osc3_mode | 41 | Osc3 Mode | 0 | 67 | discrete |
| osc3_volume | 42 | Osc3 Volume | 0 | 127 | |
| osc3_semitone | 43 | Osc3 Semitone | 16 | 112 | bipolar |
| osc3_detune | 44 | Osc3 Detune | 0 | 127 | |
| low_eq_freq | 45 | LowEQ Frequency | 0 | 127 | bipolar |
| high_eq_freq | 46 | HighEQ Frequency | 0 | 127 | bipolar |
| phaser_mode | 84 | Phaser Mode | 0 | 6 | discrete |
| phaser_mix | 85 | Phaser Mix | 0 | 127 | |
| phaser_rate | 86 | Phaser Rate | 0 | 127 | |
| phaser_depth | 87 | Phaser Depth | 0 | 127 | |
| phaser_frequency | 88 | Phaser Frequency | 0 | 127 | |
| phaser_feedback | 89 | Phaser Feedback | 0 | 127 | bipolar |
| phaser_spread | 90 | Phaser Spread | 0 | 127 | |
| mid_eq_gain | 92 | MidEQ Gain | 0 | 127 | bipolar |
| mid_eq_freq | 93 | MidEQ Frequency | 0 | 127 | bipolar |
| mid_eq_q | 94 | MidEQ Q-Factor | 0 | 127 | bipolar |
| low_eq_gain | 95 | LowEQ Gain | 0 | 127 | bipolar |
| high_eq_gain | 96 | HighEQ Gain | 0 | 127 | bipolar |
| bass_intensity | 97 | Bass Intensity | 0 | 127 | |
| bass_tune | 98 | Bass Tune | 0 | 127 | |
| input_ringmod | 99 | Input Ringmodulator | 0 | 127 | |
| distortion_curve | 100 | Distortion Curve | 0 | 11 | discrete |
| distortion_intensity | 101 | Distortion Intensity | 0 | 127 | |
| assign4_source | 103 | Assign4 Source | 0 | 27 | discrete |
| assign4_destination | 104 | Assign4 Destination | 0 | 122 | discrete |
| assign4_amount | 105 | Assign4 Amount | 0 | 127 | bipolar |
| assign5_source | 106 | Assign5 Source | 0 | 27 | discrete |
| assign5_destination | 107 | Assign5 Destination | 0 | 122 | discrete |
| assign5_amount | 108 | Assign5 Amount | 0 | 127 | bipolar |
| assign6_source | 109 | Assign6 Source | 0 | 27 | discrete |
| assign6_destination | 110 | Assign6 Destination | 0 | 122 | discrete |
| assign6_amount | 111 | Assign6 Amount | 0 | 127 | bipolar |
| input_follower_mode | 38 | Input Follower Mode | 0 | 9 | discrete |
| vocoder_mode | 39 | Vocoder Mode | 0 | 12 | discrete (C only really) |

---

## Tasks

### Task 1: Enable Page B Polypressure in Child Init

**Files:**
- Modify: `src/dsp/virus_plugin.cpp:698-704` (after sendInitControlCommands)

**Step 1: Add polypressure enable after init commands**

In `child_main()`, after line 701 (`mc->createDefaultState();`) and the subsequent processAudio drain, add:

```cpp
    /* Enable Page B parameter control via MIDI polypressure.
     * sendInitControlCommands disables this by default; we need it
     * for full parameter access. */
    mc->sendControlCommand(virusLib::MIDI_CONTROL_HIGH_PAGE, 0x1);
```

Insert this after line 703 (after the `for (int i = 0; i < 8; i++) dsp1->processAudio(...)` drain loop), before the `}` closing brace at line 704.

**Step 2: Build and verify**

Run: `cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-virus && ./scripts/build.sh`
Expected: Build succeeds with no errors.

**Step 3: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: enable Page B polypressure for full parameter access"
```

---

### Task 2: Expand Parameter Table with Page Support

**Files:**
- Modify: `src/dsp/virus_plugin.cpp:145-173` (g_params struct and table)

**Step 1: Update the param struct to include page and model info**

Replace the current `virus_param_t` struct and `g_params[]` table:

```cpp
/* Page identifiers matching gearmulator's internal pages */
#define VIRUS_PAGE_A 0  /* Standard CC (0xB0) */
#define VIRUS_PAGE_B 1  /* Polypressure (0xA0) */

/* Minimum model required: 0 = all models, 1 = B or C, 2 = C only */
#define VIRUS_MODEL_ALL 0
#define VIRUS_MODEL_BC  1
#define VIRUS_MODEL_C   2

struct virus_param_t {
    const char *key;
    const char *name;
    int page;       /* VIRUS_PAGE_A or VIRUS_PAGE_B */
    int cc;         /* parameter index within page */
    int min_val;
    int max_val;
    int model_min;  /* VIRUS_MODEL_ALL, VIRUS_MODEL_BC, or VIRUS_MODEL_C */
};

static const virus_param_t g_params[] = {
    /* ── Page A: Oscillators ── */
    {"osc1_shape",           "Osc1 Shape",       VIRUS_PAGE_A, 17, 0, 127, VIRUS_MODEL_ALL},
    {"osc1_pulsewidth",      "Osc1 PW",          VIRUS_PAGE_A, 18, 0, 127, VIRUS_MODEL_ALL},
    {"osc1_wave_select",     "Osc1 Wave",        VIRUS_PAGE_A, 19, 0,  63, VIRUS_MODEL_ALL},
    {"osc1_semitone",        "Osc1 Semi",        VIRUS_PAGE_A, 20, 16,112, VIRUS_MODEL_ALL},
    {"osc1_keyfollow",       "Osc1 KeyFlw",      VIRUS_PAGE_A, 21, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_shape",           "Osc2 Shape",       VIRUS_PAGE_A, 22, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_pulsewidth",      "Osc2 PW",          VIRUS_PAGE_A, 23, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_wave_select",     "Osc2 Wave",        VIRUS_PAGE_A, 24, 0,  63, VIRUS_MODEL_ALL},
    {"osc2_semitone",        "Osc2 Semi",        VIRUS_PAGE_A, 25, 16,112, VIRUS_MODEL_ALL},
    {"osc2_detune",          "Osc2 Detune",      VIRUS_PAGE_A, 26, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_fm_amount",       "Osc2 FM Amt",      VIRUS_PAGE_A, 27, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_sync",            "Osc2 Sync",        VIRUS_PAGE_A, 28, 0,   1, VIRUS_MODEL_ALL},
    {"osc2_filt_env_amt",    "Osc2 FiltEnv",     VIRUS_PAGE_A, 29, 0, 127, VIRUS_MODEL_ALL},
    {"fm_filt_env_amt",      "FM FiltEnv",       VIRUS_PAGE_A, 30, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_keyfollow",       "Osc2 KeyFlw",      VIRUS_PAGE_A, 31, 0, 127, VIRUS_MODEL_ALL},
    {"osc_balance",          "Osc Balance",      VIRUS_PAGE_A, 33, 0, 127, VIRUS_MODEL_ALL},
    {"sub_osc_volume",       "Sub Volume",       VIRUS_PAGE_A, 34, 0, 127, VIRUS_MODEL_ALL},
    {"sub_osc_shape",        "Sub Shape",        VIRUS_PAGE_A, 35, 0,   1, VIRUS_MODEL_ALL},
    {"osc_mainvolume",       "Osc Volume",       VIRUS_PAGE_A, 36, 0, 127, VIRUS_MODEL_ALL},
    {"noise_volume",         "Noise Vol",        VIRUS_PAGE_A, 37, 0, 127, VIRUS_MODEL_ALL},
    {"ringmod_volume",       "Ring Mod Vol",     VIRUS_PAGE_A, 38, 0, 127, VIRUS_MODEL_ALL},
    {"noise_color",          "Noise Color",      VIRUS_PAGE_A, 39, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: Filters ── */
    {"cutoff",               "Cutoff",           VIRUS_PAGE_A, 40, 0, 127, VIRUS_MODEL_ALL},
    {"cutoff2_offset",       "Cutoff2 Ofs",      VIRUS_PAGE_A, 41, 0, 127, VIRUS_MODEL_ALL},
    {"filter1_resonance",    "Filt1 Reso",       VIRUS_PAGE_A, 42, 0, 127, VIRUS_MODEL_ALL},
    {"filter2_resonance",    "Filt2 Reso",       VIRUS_PAGE_A, 43, 0, 127, VIRUS_MODEL_ALL},
    {"filter1_env_amt",      "Filt1 EnvAmt",     VIRUS_PAGE_A, 44, 0, 127, VIRUS_MODEL_ALL},
    {"filter2_env_amt",      "Filt2 EnvAmt",     VIRUS_PAGE_A, 45, 0, 127, VIRUS_MODEL_ALL},
    {"filter1_keyfollow",    "Filt1 KeyFlw",     VIRUS_PAGE_A, 46, 0, 127, VIRUS_MODEL_ALL},
    {"filter2_keyfollow",    "Filt2 KeyFlw",     VIRUS_PAGE_A, 47, 0, 127, VIRUS_MODEL_ALL},
    {"filter_balance",       "Filt Balance",     VIRUS_PAGE_A, 48, 0, 127, VIRUS_MODEL_ALL},
    {"saturation_curve",     "Saturation",       VIRUS_PAGE_A, 49, 0,  14, VIRUS_MODEL_ALL},
    {"filter1_mode",         "Filt1 Mode",       VIRUS_PAGE_A, 51, 0,   7, VIRUS_MODEL_ALL},
    {"filter2_mode",         "Filt2 Mode",       VIRUS_PAGE_A, 52, 0,   3, VIRUS_MODEL_ALL},
    {"filter_routing",       "Filt Routing",     VIRUS_PAGE_A, 53, 0,   3, VIRUS_MODEL_ALL},

    /* ── Page A: Filter Envelope ── */
    {"flt_attack",           "Flt Attack",       VIRUS_PAGE_A, 54, 0, 127, VIRUS_MODEL_ALL},
    {"flt_decay",            "Flt Decay",        VIRUS_PAGE_A, 55, 0, 127, VIRUS_MODEL_ALL},
    {"flt_sustain",          "Flt Sustain",      VIRUS_PAGE_A, 56, 0, 127, VIRUS_MODEL_ALL},
    {"flt_sustain_time",     "Flt Sus Time",     VIRUS_PAGE_A, 57, 0, 127, VIRUS_MODEL_ALL},
    {"flt_release",          "Flt Release",      VIRUS_PAGE_A, 58, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: Amp Envelope ── */
    {"amp_attack",           "Amp Attack",       VIRUS_PAGE_A, 59, 0, 127, VIRUS_MODEL_ALL},
    {"amp_decay",            "Amp Decay",        VIRUS_PAGE_A, 60, 0, 127, VIRUS_MODEL_ALL},
    {"amp_sustain",          "Amp Sustain",      VIRUS_PAGE_A, 61, 0, 127, VIRUS_MODEL_ALL},
    {"amp_sustain_time",     "Amp Sus Time",     VIRUS_PAGE_A, 62, 0, 127, VIRUS_MODEL_ALL},
    {"amp_release",          "Amp Release",      VIRUS_PAGE_A, 63, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: LFO 1 ── */
    {"lfo1_rate",            "LFO1 Rate",        VIRUS_PAGE_A, 67, 0, 127, VIRUS_MODEL_ALL},
    {"lfo1_shape",           "LFO1 Shape",       VIRUS_PAGE_A, 68, 0,  67, VIRUS_MODEL_ALL},
    {"lfo1_env_mode",        "LFO1 Env Mode",    VIRUS_PAGE_A, 69, 0,   1, VIRUS_MODEL_ALL},
    {"lfo1_mode",            "LFO1 Mode",        VIRUS_PAGE_A, 70, 0,   1, VIRUS_MODEL_ALL},
    {"lfo1_symmetry",        "LFO1 Symmetry",    VIRUS_PAGE_A, 71, 0, 127, VIRUS_MODEL_ALL},
    {"lfo1_keyfollow",       "LFO1 KeyFlw",      VIRUS_PAGE_A, 72, 0, 127, VIRUS_MODEL_ALL},
    {"lfo1_keytrigger",      "LFO1 KeyTrig",     VIRUS_PAGE_A, 73, 0, 127, VIRUS_MODEL_ALL},
    {"osc1_lfo1_amount",     "Osc1 LFO1",        VIRUS_PAGE_A, 74, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_lfo1_amount",     "Osc2 LFO1",        VIRUS_PAGE_A, 75, 0, 127, VIRUS_MODEL_ALL},
    {"pw_lfo1_amount",       "PW LFO1",          VIRUS_PAGE_A, 76, 0, 127, VIRUS_MODEL_ALL},
    {"reso_lfo1_amount",     "Reso LFO1",        VIRUS_PAGE_A, 77, 0, 127, VIRUS_MODEL_ALL},
    {"filtgain_lfo1_amount", "FiltGain LFO1",    VIRUS_PAGE_A, 78, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: LFO 2 ── */
    {"lfo2_rate",            "LFO2 Rate",        VIRUS_PAGE_A, 79, 0, 127, VIRUS_MODEL_ALL},
    {"lfo2_shape",           "LFO2 Shape",       VIRUS_PAGE_A, 80, 0,  67, VIRUS_MODEL_ALL},
    {"lfo2_env_mode",        "LFO2 Env Mode",    VIRUS_PAGE_A, 81, 0,   1, VIRUS_MODEL_ALL},
    {"lfo2_mode",            "LFO2 Mode",        VIRUS_PAGE_A, 82, 0,   1, VIRUS_MODEL_ALL},
    {"lfo2_symmetry",        "LFO2 Symmetry",    VIRUS_PAGE_A, 83, 0, 127, VIRUS_MODEL_ALL},
    {"lfo2_keyfollow",       "LFO2 KeyFlw",      VIRUS_PAGE_A, 84, 0, 127, VIRUS_MODEL_ALL},
    {"lfo2_keytrigger",      "LFO2 KeyTrig",     VIRUS_PAGE_A, 85, 0, 127, VIRUS_MODEL_ALL},
    {"shape_lfo2_amount",    "Shape LFO2",       VIRUS_PAGE_A, 86, 0, 127, VIRUS_MODEL_ALL},
    {"fm_lfo2_amount",       "FM LFO2",          VIRUS_PAGE_A, 87, 0, 127, VIRUS_MODEL_ALL},
    {"cutoff1_lfo2_amount",  "Cut1 LFO2",        VIRUS_PAGE_A, 88, 0, 127, VIRUS_MODEL_ALL},
    {"cutoff2_lfo2_amount",  "Cut2 LFO2",        VIRUS_PAGE_A, 89, 0, 127, VIRUS_MODEL_ALL},
    {"pan_lfo2_amount",      "Pan LFO2",         VIRUS_PAGE_A, 90, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: Output & Performance ── */
    {"patch_volume",         "Volume",           VIRUS_PAGE_A, 91, 0, 127, VIRUS_MODEL_ALL},
    {"panorama",             "Panorama",         VIRUS_PAGE_A, 10, 0, 127, VIRUS_MODEL_ALL},
    {"transpose",            "Transpose",        VIRUS_PAGE_A, 93, 0, 127, VIRUS_MODEL_ALL},
    {"key_mode",             "Key Mode",         VIRUS_PAGE_A, 94, 0,   5, VIRUS_MODEL_ALL},
    {"unison_mode",          "Unison Mode",      VIRUS_PAGE_A, 97, 0,  15, VIRUS_MODEL_ALL},
    {"unison_detune",        "Unison Detune",    VIRUS_PAGE_A, 98, 0, 127, VIRUS_MODEL_ALL},
    {"unison_pan_spread",    "Unison Pan",       VIRUS_PAGE_A, 99, 0, 127, VIRUS_MODEL_ALL},
    {"unison_lfo_phase",     "Unison LFO Ph",    VIRUS_PAGE_A,100, 0, 127, VIRUS_MODEL_ALL},
    {"portamento_time",      "Portamento",       VIRUS_PAGE_A,  5, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page A: Chorus ── */
    {"chorus_mix",           "Chorus Mix",       VIRUS_PAGE_A,105, 0, 127, VIRUS_MODEL_ALL},
    {"chorus_rate",          "Chorus Rate",      VIRUS_PAGE_A,106, 0, 127, VIRUS_MODEL_ALL},
    {"chorus_depth",         "Chorus Depth",     VIRUS_PAGE_A,107, 0, 127, VIRUS_MODEL_ALL},
    {"chorus_delay",         "Chorus Delay",     VIRUS_PAGE_A,108, 0, 127, VIRUS_MODEL_ALL},
    {"chorus_feedback",      "Chorus Fdbk",      VIRUS_PAGE_A,109, 0, 127, VIRUS_MODEL_ALL},
    {"chorus_lfo_shape",     "Chorus LFO",       VIRUS_PAGE_A,110, 0,  67, VIRUS_MODEL_ALL},

    /* ── Page A: Delay / Reverb ── */
    {"delay_reverb_mode",    "Dly/Rev Mode",     VIRUS_PAGE_A,112, 0,  26, VIRUS_MODEL_ALL},
    {"effect_send",          "Effect Send",      VIRUS_PAGE_A,113, 0, 127, VIRUS_MODEL_ALL},
    {"delay_time",           "Delay Time",       VIRUS_PAGE_A,114, 0, 127, VIRUS_MODEL_ALL},
    {"delay_feedback",       "Delay Fdbk",       VIRUS_PAGE_A,115, 0, 127, VIRUS_MODEL_ALL},
    {"delay_rate_rev_decay", "Dly Rate/Decay",   VIRUS_PAGE_A,116, 0, 127, VIRUS_MODEL_ALL},
    {"delay_depth",          "Delay Depth",      VIRUS_PAGE_A,117, 0, 127, VIRUS_MODEL_ALL},
    {"delay_lfo_shape",      "Delay LFO",        VIRUS_PAGE_A,118, 0,   5, VIRUS_MODEL_ALL},
    {"delay_color",          "Delay Color",      VIRUS_PAGE_A,119, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page B: Arpeggiator ── */
    {"arp_mode",             "Arp Mode",         VIRUS_PAGE_B,  1, 0,   6, VIRUS_MODEL_ALL},
    {"arp_pattern",          "Arp Pattern",      VIRUS_PAGE_B,  2, 0,  63, VIRUS_MODEL_ALL},
    {"arp_octave_range",     "Arp Octaves",      VIRUS_PAGE_B,  3, 0,   3, VIRUS_MODEL_ALL},
    {"arp_hold",             "Arp Hold",         VIRUS_PAGE_B,  4, 0,   1, VIRUS_MODEL_ALL},
    {"arp_note_length",      "Arp Length",       VIRUS_PAGE_B,  5, 0, 127, VIRUS_MODEL_ALL},
    {"arp_swing",            "Arp Swing",        VIRUS_PAGE_B,  6, 0, 127, VIRUS_MODEL_ALL},
    {"arp_clock",            "Arp Clock",        VIRUS_PAGE_B, 17, 0,  17, VIRUS_MODEL_ALL},

    /* ── Page B: LFO 3 ── */
    {"lfo3_rate",            "LFO3 Rate",        VIRUS_PAGE_B,  7, 0, 127, VIRUS_MODEL_ALL},
    {"lfo3_shape",           "LFO3 Shape",       VIRUS_PAGE_B,  8, 0,  67, VIRUS_MODEL_ALL},
    {"lfo3_mode",            "LFO3 Mode",        VIRUS_PAGE_B,  9, 0,   1, VIRUS_MODEL_ALL},
    {"lfo3_keyfollow",       "LFO3 KeyFlw",      VIRUS_PAGE_B, 10, 0, 127, VIRUS_MODEL_ALL},
    {"lfo3_destination",     "LFO3 Dest",        VIRUS_PAGE_B, 11, 0,   5, VIRUS_MODEL_ALL},
    {"osc_lfo3_amount",      "Osc LFO3 Amt",    VIRUS_PAGE_B, 12, 0, 127, VIRUS_MODEL_ALL},
    {"lfo3_fadein_time",     "LFO3 FadeIn",      VIRUS_PAGE_B, 13, 0, 127, VIRUS_MODEL_ALL},
    {"lfo3_clock",           "LFO3 Clock",       VIRUS_PAGE_B, 21, 0,  21, VIRUS_MODEL_ALL},

    /* ── Page B: Clock / Sync ── */
    {"clock_tempo",          "Clock Tempo",      VIRUS_PAGE_B, 16, 0, 127, VIRUS_MODEL_ALL},
    {"lfo1_clock",           "LFO1 Clock",       VIRUS_PAGE_B, 18, 0,  21, VIRUS_MODEL_ALL},
    {"lfo2_clock",           "LFO2 Clock",       VIRUS_PAGE_B, 19, 0,  21, VIRUS_MODEL_ALL},
    {"delay_clock",          "Delay Clock",      VIRUS_PAGE_B, 20, 0,  16, VIRUS_MODEL_ALL},

    /* ── Page B: Performance / Misc ── */
    {"filter1_env_polarity", "Flt1 Env Pol",     VIRUS_PAGE_B, 30, 0,   1, VIRUS_MODEL_ALL},
    {"filter2_env_polarity", "Flt2 Env Pol",     VIRUS_PAGE_B, 31, 0,   1, VIRUS_MODEL_ALL},
    {"filter2_cutoff_link",  "Flt2 Cut Link",    VIRUS_PAGE_B, 32, 0,   1, VIRUS_MODEL_ALL},
    {"filter_keytrack_base", "Flt KeyTrk Base",  VIRUS_PAGE_B, 33, 0, 127, VIRUS_MODEL_ALL},
    {"osc_fm_mode",          "Osc FM Mode",      VIRUS_PAGE_B, 34, 0,  12, VIRUS_MODEL_ALL},
    {"osc_init_phase",       "Osc Init Phase",   VIRUS_PAGE_B, 35, 0, 127, VIRUS_MODEL_ALL},
    {"punch_intensity",      "Punch",            VIRUS_PAGE_B, 36, 0, 127, VIRUS_MODEL_ALL},
    {"bender_range_up",      "Bend Up",          VIRUS_PAGE_B, 26, 0, 127, VIRUS_MODEL_ALL},
    {"bender_range_down",    "Bend Down",        VIRUS_PAGE_B, 27, 0, 127, VIRUS_MODEL_ALL},
    {"filter_select",        "Filter Select",    VIRUS_PAGE_B,122, 0,   2, VIRUS_MODEL_ALL},

    /* ── Page B: Velocity Sensitivity ── */
    {"osc1_shape_velocity",  "Osc1 Shp Vel",    VIRUS_PAGE_B, 47, 0, 127, VIRUS_MODEL_ALL},
    {"osc2_shape_velocity",  "Osc2 Shp Vel",    VIRUS_PAGE_B, 48, 0, 127, VIRUS_MODEL_ALL},
    {"pulsewidth_velocity",  "PW Velocity",      VIRUS_PAGE_B, 49, 0, 127, VIRUS_MODEL_ALL},
    {"fm_amount_velocity",   "FM Amt Vel",       VIRUS_PAGE_B, 50, 0, 127, VIRUS_MODEL_ALL},
    {"flt1_envamt_velocity", "Flt1 Env Vel",     VIRUS_PAGE_B, 54, 0, 127, VIRUS_MODEL_ALL},
    {"flt2_envamt_velocity", "Flt2 Env Vel",     VIRUS_PAGE_B, 55, 0, 127, VIRUS_MODEL_ALL},
    {"resonance1_velocity",  "Reso1 Vel",        VIRUS_PAGE_B, 56, 0, 127, VIRUS_MODEL_ALL},
    {"resonance2_velocity",  "Reso2 Vel",        VIRUS_PAGE_B, 57, 0, 127, VIRUS_MODEL_ALL},
    {"amp_velocity",         "Amp Velocity",     VIRUS_PAGE_B, 60, 0, 127, VIRUS_MODEL_ALL},
    {"panorama_velocity",    "Pan Velocity",     VIRUS_PAGE_B, 61, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page B: Mod Matrix (slots 1-3, all models) ── */
    {"assign1_source",       "Asgn1 Src",        VIRUS_PAGE_B, 64, 0,  27, VIRUS_MODEL_ALL},
    {"assign1_destination",  "Asgn1 Dst",        VIRUS_PAGE_B, 65, 0, 122, VIRUS_MODEL_ALL},
    {"assign1_amount",       "Asgn1 Amt",        VIRUS_PAGE_B, 66, 0, 127, VIRUS_MODEL_ALL},
    {"assign2_source",       "Asgn2 Src",        VIRUS_PAGE_B, 67, 0,  27, VIRUS_MODEL_ALL},
    {"assign2_dest1",        "Asgn2 Dst1",       VIRUS_PAGE_B, 68, 0, 122, VIRUS_MODEL_ALL},
    {"assign2_amount1",      "Asgn2 Amt1",       VIRUS_PAGE_B, 69, 0, 127, VIRUS_MODEL_ALL},
    {"assign2_dest2",        "Asgn2 Dst2",       VIRUS_PAGE_B, 70, 0, 122, VIRUS_MODEL_ALL},
    {"assign2_amount2",      "Asgn2 Amt2",       VIRUS_PAGE_B, 71, 0, 127, VIRUS_MODEL_ALL},
    {"assign3_source",       "Asgn3 Src",        VIRUS_PAGE_B, 72, 0,  27, VIRUS_MODEL_ALL},
    {"assign3_dest1",        "Asgn3 Dst1",       VIRUS_PAGE_B, 73, 0, 122, VIRUS_MODEL_ALL},
    {"assign3_amount1",      "Asgn3 Amt1",       VIRUS_PAGE_B, 74, 0, 127, VIRUS_MODEL_ALL},
    {"assign3_dest2",        "Asgn3 Dst2",       VIRUS_PAGE_B, 75, 0, 122, VIRUS_MODEL_ALL},
    {"assign3_amount2",      "Asgn3 Amt2",       VIRUS_PAGE_B, 76, 0, 127, VIRUS_MODEL_ALL},
    {"assign3_dest3",        "Asgn3 Dst3",       VIRUS_PAGE_B, 77, 0, 122, VIRUS_MODEL_ALL},
    {"assign3_amount3",      "Asgn3 Amt3",       VIRUS_PAGE_B, 78, 0, 127, VIRUS_MODEL_ALL},
    {"lfo1_assign_dest",     "LFO1 Asgn Dst",   VIRUS_PAGE_B, 79, 0, 122, VIRUS_MODEL_ALL},
    {"lfo1_assign_amount",   "LFO1 Asgn Amt",   VIRUS_PAGE_B, 80, 0, 127, VIRUS_MODEL_ALL},
    {"lfo2_assign_dest",     "LFO2 Asgn Dst",   VIRUS_PAGE_B, 81, 0, 122, VIRUS_MODEL_ALL},
    {"lfo2_assign_amount",   "LFO2 Asgn Amt",   VIRUS_PAGE_B, 82, 0, 127, VIRUS_MODEL_ALL},

    /* ── Page B: Osc 3 (B/C only) ── */
    {"osc3_mode",            "Osc3 Mode",        VIRUS_PAGE_B, 41, 0,  67, VIRUS_MODEL_BC},
    {"osc3_volume",          "Osc3 Volume",      VIRUS_PAGE_B, 42, 0, 127, VIRUS_MODEL_BC},
    {"osc3_semitone",        "Osc3 Semi",        VIRUS_PAGE_B, 43, 16,112, VIRUS_MODEL_BC},
    {"osc3_detune",          "Osc3 Detune",      VIRUS_PAGE_B, 44, 0, 127, VIRUS_MODEL_BC},

    /* ── Page B: Phaser (B/C only) ── */
    {"phaser_mode",          "Phaser Mode",      VIRUS_PAGE_B, 84, 0,   6, VIRUS_MODEL_BC},
    {"phaser_mix",           "Phaser Mix",       VIRUS_PAGE_B, 85, 0, 127, VIRUS_MODEL_BC},
    {"phaser_rate",          "Phaser Rate",      VIRUS_PAGE_B, 86, 0, 127, VIRUS_MODEL_BC},
    {"phaser_depth",         "Phaser Depth",     VIRUS_PAGE_B, 87, 0, 127, VIRUS_MODEL_BC},
    {"phaser_frequency",     "Phaser Freq",      VIRUS_PAGE_B, 88, 0, 127, VIRUS_MODEL_BC},
    {"phaser_feedback",      "Phaser Fdbk",      VIRUS_PAGE_B, 89, 0, 127, VIRUS_MODEL_BC},
    {"phaser_spread",        "Phaser Spread",    VIRUS_PAGE_B, 90, 0, 127, VIRUS_MODEL_BC},

    /* ── Page B: EQ (B/C only) ── */
    {"low_eq_freq",          "Lo EQ Freq",       VIRUS_PAGE_B, 45, 0, 127, VIRUS_MODEL_BC},
    {"high_eq_freq",         "Hi EQ Freq",       VIRUS_PAGE_B, 46, 0, 127, VIRUS_MODEL_BC},
    {"mid_eq_gain",          "Mid EQ Gain",      VIRUS_PAGE_B, 92, 0, 127, VIRUS_MODEL_BC},
    {"mid_eq_freq",          "Mid EQ Freq",      VIRUS_PAGE_B, 93, 0, 127, VIRUS_MODEL_BC},
    {"mid_eq_q",             "Mid EQ Q",         VIRUS_PAGE_B, 94, 0, 127, VIRUS_MODEL_BC},
    {"low_eq_gain",          "Lo EQ Gain",       VIRUS_PAGE_B, 95, 0, 127, VIRUS_MODEL_BC},
    {"high_eq_gain",         "Hi EQ Gain",       VIRUS_PAGE_B, 96, 0, 127, VIRUS_MODEL_BC},

    /* ── Page B: Distortion (B/C only) ── */
    {"distortion_curve",     "Dist Curve",       VIRUS_PAGE_B,100, 0,  11, VIRUS_MODEL_BC},
    {"distortion_intensity", "Dist Intensity",   VIRUS_PAGE_B,101, 0, 127, VIRUS_MODEL_BC},
    {"bass_intensity",       "Analog Boost",     VIRUS_PAGE_B, 97, 0, 127, VIRUS_MODEL_BC},
    {"bass_tune",            "Boost Tune",       VIRUS_PAGE_B, 98, 0, 127, VIRUS_MODEL_BC},

    /* ── Page B: Mod Matrix Slots 4-6 (B/C only) ── */
    {"assign4_source",       "Asgn4 Src",        VIRUS_PAGE_B,103, 0,  27, VIRUS_MODEL_BC},
    {"assign4_destination",  "Asgn4 Dst",        VIRUS_PAGE_B,104, 0, 122, VIRUS_MODEL_BC},
    {"assign4_amount",       "Asgn4 Amt",        VIRUS_PAGE_B,105, 0, 127, VIRUS_MODEL_BC},
    {"assign5_source",       "Asgn5 Src",        VIRUS_PAGE_B,106, 0,  27, VIRUS_MODEL_BC},
    {"assign5_destination",  "Asgn5 Dst",        VIRUS_PAGE_B,107, 0, 122, VIRUS_MODEL_BC},
    {"assign5_amount",       "Asgn5 Amt",        VIRUS_PAGE_B,108, 0, 127, VIRUS_MODEL_BC},
    {"assign6_source",       "Asgn6 Src",        VIRUS_PAGE_B,109, 0,  27, VIRUS_MODEL_BC},
    {"assign6_destination",  "Asgn6 Dst",        VIRUS_PAGE_B,110, 0, 122, VIRUS_MODEL_BC},
    {"assign6_amount",       "Asgn6 Amt",        VIRUS_PAGE_B,111, 0, 127, VIRUS_MODEL_BC},

    /* ── Page B: Vocoder / Input (C only) ── */
    {"input_follower_mode",  "Input Follower",   VIRUS_PAGE_B, 38, 0,   9, VIRUS_MODEL_C},
    {"vocoder_mode",         "Vocoder Mode",     VIRUS_PAGE_B, 39, 0,  12, VIRUS_MODEL_C},
    {"input_ringmod",        "Input RingMod",    VIRUS_PAGE_B, 99, 0, 127, VIRUS_MODEL_BC},
};
static const int NUM_PARAMS = sizeof(g_params) / sizeof(g_params[0]);
```

**Step 2: Build and verify**

Run: `cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-virus && ./scripts/build.sh`
Expected: Build succeeds.

**Step 3: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: expand parameter table to ~140 params with page and model info"
```

---

### Task 3: Expand Shared Memory for Page B Tracking

**Files:**
- Modify: `src/dsp/virus_plugin.cpp` — shared memory struct and helpers

**Step 1: Add page B arrays to shared memory**

In `virus_shm_t`, after `cc_seen[128]`, add:

```cpp
    volatile int cc_values_b[128];  /* Page B parameter values */
    uint8_t cc_seen_b[128];         /* Page B parameter seen flags */
```

**Step 2: Update `clear_param_overrides` to clear both pages**

```cpp
static void clear_param_overrides(virus_shm_t *shm) {
    if (!shm) return;
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (g_params[i].page == VIRUS_PAGE_A)
            shm->cc_seen[g_params[i].cc] = 0;
        else
            shm->cc_seen_b[g_params[i].cc] = 0;
    }
}
```

**Step 3: Add model check helper**

After the `g_params` table, add:

```cpp
static int model_name_to_level(const char *name) {
    if (!name || !name[0]) return 0;
    if (name[0] == 'C' || name[0] == 'c') return 2;
    if (name[0] == 'B' || name[0] == 'b') return 1;
    return 0; /* A or unknown */
}

static bool param_available_for_model(const virus_param_t *p, const char *model_name) {
    int level = model_name_to_level(model_name);
    return p->model_min <= level;
}
```

**Step 4: Build and verify**

Run: `./scripts/build.sh`

**Step 5: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: expand shared memory for page B parameter tracking"
```

---

### Task 4: Update MIDI Send Logic for Page B

**Files:**
- Modify: `src/dsp/virus_plugin.cpp` — `v2_set_param`, `child_process_midi_fifo`

**Step 1: Create helper to send a param value as the correct MIDI message type**

Add after the model helpers:

```cpp
static void send_param_midi(virus_shm_t *shm, const virus_param_t *p, int value) {
    if (p->page == VIRUS_PAGE_A) {
        shm->cc_values[p->cc] = value;
        shm->cc_seen[p->cc] = 1;
        uint8_t msg[3] = { 0xB0, (uint8_t)p->cc, (uint8_t)value };
        midi_fifo_push(shm, msg, 3);
    } else {
        shm->cc_values_b[p->cc] = value;
        shm->cc_seen_b[p->cc] = 1;
        uint8_t msg[3] = { 0xA0, (uint8_t)p->cc, (uint8_t)value };
        midi_fifo_push(shm, msg, 3);
    }
}

static int get_param_value(virus_shm_t *shm, const virus_param_t *p) {
    if (p->page == VIRUS_PAGE_A)
        return shm->cc_values[p->cc];
    else
        return shm->cc_values_b[p->cc];
}

static bool is_param_seen(virus_shm_t *shm, const virus_param_t *p) {
    if (p->page == VIRUS_PAGE_A)
        return shm->cc_seen[p->cc] != 0;
    else
        return shm->cc_seen_b[p->cc] != 0;
}
```

**Step 2: Update `v2_set_param` param loop**

Replace the param-matching loop at the end of `v2_set_param` (the `for (int i = 0; i < NUM_PARAMS; i++)` block near line 1297):

```cpp
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(key, g_params[i].key) == 0) {
            int ival = atoi(val);
            if (ival < g_params[i].min_val) ival = g_params[i].min_val;
            if (ival > g_params[i].max_val) ival = g_params[i].max_val;
            send_param_midi(shm, &g_params[i], ival);
            return;
        }
    }
```

**Step 3: Update state restore loop in `v2_set_param`**

In the `if (strcmp(key, "state") == 0)` handler, replace the param restore loop (around line 1202):

```cpp
        if (state_version >= VIRUS_STATE_VERSION) {
            for (int i = 0; i < NUM_PARAMS; i++) {
                if (json_get_int(val, g_params[i].key, &ival) == 0) {
                    if (ival < g_params[i].min_val) ival = g_params[i].min_val;
                    if (ival > g_params[i].max_val) ival = g_params[i].max_val;
                    send_param_midi(shm, &g_params[i], ival);
                }
            }
        }
```

**Step 4: Update `child_process_midi_fifo` to track page B values**

In `child_process_midi_fifo`, after the existing CC tracking block (around line 434), add polypressure tracking:

```cpp
        if (status == 0xA0 && len >= 3) {
            shm->cc_values_b[msg[1] & 0x7F] = msg[2] & 0x7F;
            shm->cc_seen_b[msg[1] & 0x7F] = 1;
        }
```

**Step 5: Update `v2_get_param` param loop**

Replace the param-matching loop in `v2_get_param` (around line 1373):

```cpp
    for (int i = 0; i < NUM_PARAMS; i++)
        if (strcmp(key, g_params[i].key) == 0)
            return snprintf(buf, buf_len, "%d", get_param_value(shm, &g_params[i]));
```

**Step 6: Build and verify**

Run: `./scripts/build.sh`

**Step 7: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: add page-aware MIDI send/receive for all parameters"
```

---

### Task 5: Update State Serialization

**Files:**
- Modify: `src/dsp/virus_plugin.cpp` — state get/set, state version bump

**Step 1: Bump state version**

Find `VIRUS_STATE_VERSION` (should be `2`) and change to `3`:

```cpp
#define VIRUS_STATE_VERSION 3
```

**Step 2: Update state serialization in `v2_get_param "state"`**

Replace the param serialization loop in the state handler (around line 1384):

```cpp
        for (int i = 0; i < NUM_PARAMS; i++) {
            if (!is_param_seen(shm, &g_params[i])) continue;
            off += snprintf(buf+off, buf_len-off, ",\"%s\":%d",
                g_params[i].key, get_param_value(shm, &g_params[i]));
        }
```

**Step 3: Build and verify**

Run: `./scripts/build.sh`

**Step 4: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: update state serialization for expanded params (v3)"
```

---

### Task 6: Update chain_params Dynamic Output

**Files:**
- Modify: `src/dsp/virus_plugin.cpp` — `v2_get_param "chain_params"`

**Step 1: Add model filtering to chain_params output**

Replace the chain_params handler (around line 1397). The key change is filtering by model and using `get_param_value`/`send_param_midi`:

```cpp
    if (strcmp(key, "chain_params") == 0) {
        int off = 0;
        int bank_max = shm->bank_count > 0 ? shm->bank_count - 1 : 0;
        off += snprintf(buf+off, buf_len-off,
            "[{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"bank_index\",\"name\":\"Bank\",\"type\":\"int\",\"min\":0,\"max\":%d},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-4,\"max\":4},"
            "{\"key\":\"rom_index\",\"name\":\"ROM\",\"type\":\"enum\",\"options\":[", bank_max);
        for (int i = 0; i < shm->rom_count; i++) {
            if (i > 0) off += snprintf(buf+off, buf_len-off, ",");
            off += snprintf(buf+off, buf_len-off, "\"%s\"", shm->rom_names[i]);
        }
        if (shm->rom_count == 0)
            off += snprintf(buf+off, buf_len-off, "\"(loading)\"");
        off += snprintf(buf+off, buf_len-off, "]},"
            "{\"key\":\"dsp_clock\",\"name\":\"DSP Clock %%\",\"type\":\"int\",\"min\":10,\"max\":100,\"step\":5},"
            "{\"key\":\"gain\",\"name\":\"Gain %%\",\"type\":\"int\",\"min\":1,\"max\":100}");
        for (int i = 0; i < NUM_PARAMS && off < buf_len - 100; i++) {
            if (!param_available_for_model(&g_params[i], (const char*)shm->rom_model_name))
                continue;
            off += snprintf(buf+off, buf_len-off,
                ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"int\",\"min\":%d,\"max\":%d}",
                g_params[i].key, g_params[i].name, g_params[i].min_val, g_params[i].max_val);
        }
        off += snprintf(buf+off, buf_len-off, "]");
        return off;
    }
```

**Step 2: Build and verify**

Run: `./scripts/build.sh`

**Step 3: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: filter chain_params by ROM model"
```

---

### Task 7: Rebuild UI Hierarchy with Full Parameter Organization

**Files:**
- Modify: `src/dsp/virus_plugin.cpp` — `v2_get_param "ui_hierarchy"`

**Step 1: Replace the static ui_hierarchy JSON**

This is the largest change. Replace the `ui_hierarchy` handler with a dynamic builder that filters by model. The hierarchy has 12 groups as designed:

Replace the entire `if (strcmp(key, "ui_hierarchy") == 0)` block with a function that builds the JSON dynamically based on `shm->rom_model_name`. The full JSON structure:

```
root (preset browser)
  ├── osc (Oscillators) — knobs: osc1_shape, osc2_shape, osc_balance, osc_mainvolume
  │     All osc1/osc2 params, sub, noise, ringmod
  │     If B/C: osc3 params
  ├── filter (Filters) — knobs: cutoff, filter1_resonance, filter1_mode, filter_routing
  │     All filter params, saturation, filter select
  ├── flt_env (Filter Env) — knobs: flt_attack, flt_decay, flt_sustain, flt_release
  │     + sustain time, polarity, velocity params
  ├── amp_env (Amp Env) — knobs: amp_attack, amp_decay, amp_sustain, amp_release
  │     + sustain time, velocity, punch
  ├── lfo1 (LFO 1) — knobs: lfo1_rate, lfo1_shape, lfo1_symmetry, osc1_lfo1_amount
  │     All LFO1 params + mod targets
  ├── lfo2 (LFO 2) — knobs: lfo2_rate, lfo2_shape, lfo2_symmetry, shape_lfo2_amount
  │     All LFO2 params + mod targets
  ├── lfo3 (LFO 3) — knobs: lfo3_rate, lfo3_shape, lfo3_destination, osc_lfo3_amount
  │     All LFO3 params
  ├── arp (Arpeggiator) — knobs: arp_mode, arp_pattern, arp_octave_range, arp_swing
  │     All arp params + clock
  ├── fx (Effects) — knobs: chorus_mix, effect_send, delay_time, delay_feedback
  │     Chorus, delay/reverb, phaser (B/C), distortion (B/C), EQ (B/C)
  ├── mod (Mod Matrix) — knobs: assign1_amount, assign2_amount1, assign3_amount1
  │     3 slots all models, +3 slots B/C, LFO assign
  ├── perf (Performance) — knobs: patch_volume, panorama, unison_mode, portamento_time
  │     Volume, pan, unison, key mode, transpose, portamento, bender
  └── settings (Settings) — knobs: dsp_clock, gain
        ROM, DSP clock, gain
```

Since this is a large static string, implement it as a function that builds the JSON into the buffer, skipping B/C-only sections when model is A. See implementation guidance below.

**Implementation approach:** Rather than one massive string literal, use a helper function:

```cpp
static int build_ui_hierarchy(char *buf, int buf_len, const char *model_name) {
    int off = 0;
    int model_level = model_name_to_level(model_name);

    #define H_APPEND(...) do { off += snprintf(buf+off, buf_len-off, __VA_ARGS__); } while(0)
    #define H_PARAM(k, l) H_APPEND("{\"key\":\"%s\",\"label\":\"%s\"},", k, l)
    #define H_LEVEL(k, l) H_APPEND("{\"level\":\"%s\",\"label\":\"%s\"},", k, l)

    H_APPEND("{\"modes\":null,\"levels\":{");

    /* Root level */
    H_APPEND("\"root\":{\"list_param\":\"preset\",\"count_param\":\"preset_count\","
             "\"name_param\":\"preset_name\",\"children\":null,"
             "\"knobs\":[\"cutoff\",\"filter1_resonance\",\"filter1_env_amt\","
             "\"flt_attack\",\"flt_decay\",\"flt_sustain\",\"flt_release\",\"octave_transpose\"],"
             "\"params\":[{\"key\":\"bank_index\",\"label\":\"Bank\"},");
    H_LEVEL("osc", "Oscillators");
    H_LEVEL("filter", "Filters");
    H_LEVEL("flt_env", "Filter Env");
    H_LEVEL("amp_env", "Amp Env");
    H_LEVEL("lfo1", "LFO 1");
    H_LEVEL("lfo2", "LFO 2");
    H_LEVEL("lfo3", "LFO 3");
    H_LEVEL("arp", "Arpeggiator");
    H_LEVEL("fx", "Effects");
    H_LEVEL("mod", "Mod Matrix");
    H_LEVEL("perf", "Performance");
    H_LEVEL("settings", "Settings");
    /* Remove trailing comma from last entry - back up one char */
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Oscillators */
    H_APPEND("\"osc\":{\"children\":null,"
             "\"knobs\":[\"osc1_shape\",\"osc2_shape\",\"osc_balance\",\"osc_mainvolume\"],"
             "\"params\":[");
    H_PARAM("osc1_shape", "Osc1 Shape");
    H_PARAM("osc1_pulsewidth", "Osc1 PW");
    H_PARAM("osc1_wave_select", "Osc1 Wave");
    H_PARAM("osc1_semitone", "Osc1 Semi");
    H_PARAM("osc1_keyfollow", "Osc1 KeyFlw");
    H_PARAM("osc2_shape", "Osc2 Shape");
    H_PARAM("osc2_pulsewidth", "Osc2 PW");
    H_PARAM("osc2_wave_select", "Osc2 Wave");
    H_PARAM("osc2_semitone", "Osc2 Semi");
    H_PARAM("osc2_detune", "Osc2 Detune");
    H_PARAM("osc2_fm_amount", "Osc2 FM Amt");
    H_PARAM("osc2_sync", "Osc2 Sync");
    H_PARAM("osc2_filt_env_amt", "Osc2 FiltEnv");
    H_PARAM("fm_filt_env_amt", "FM FiltEnv");
    H_PARAM("osc2_keyfollow", "Osc2 KeyFlw");
    H_PARAM("osc_fm_mode", "FM Mode");
    H_PARAM("osc_init_phase", "Init Phase");
    H_PARAM("osc_balance", "Osc Balance");
    H_PARAM("sub_osc_volume", "Sub Volume");
    H_PARAM("sub_osc_shape", "Sub Shape");
    H_PARAM("osc_mainvolume", "Osc Volume");
    H_PARAM("noise_volume", "Noise Vol");
    H_PARAM("ringmod_volume", "Ring Mod Vol");
    H_PARAM("noise_color", "Noise Color");
    if (model_level >= 1) {
        H_PARAM("osc3_mode", "Osc3 Mode");
        H_PARAM("osc3_volume", "Osc3 Volume");
        H_PARAM("osc3_semitone", "Osc3 Semi");
        H_PARAM("osc3_detune", "Osc3 Detune");
    }
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Filters */
    H_APPEND("\"filter\":{\"children\":null,"
             "\"knobs\":[\"cutoff\",\"filter1_resonance\",\"filter1_mode\",\"filter_routing\"],"
             "\"params\":[");
    H_PARAM("cutoff", "Cutoff");
    H_PARAM("cutoff2_offset", "Cutoff2 Offset");
    H_PARAM("filter1_resonance", "Filt1 Reso");
    H_PARAM("filter2_resonance", "Filt2 Reso");
    H_PARAM("filter1_env_amt", "Filt1 EnvAmt");
    H_PARAM("filter2_env_amt", "Filt2 EnvAmt");
    H_PARAM("filter1_keyfollow", "Filt1 KeyFlw");
    H_PARAM("filter2_keyfollow", "Filt2 KeyFlw");
    H_PARAM("filter_balance", "Filt Balance");
    H_PARAM("saturation_curve", "Saturation");
    H_PARAM("filter1_mode", "Filt1 Mode");
    H_PARAM("filter2_mode", "Filt2 Mode");
    H_PARAM("filter_routing", "Filt Routing");
    H_PARAM("filter_select", "Filter Select");
    H_PARAM("filter2_cutoff_link", "Filt2 Link");
    H_PARAM("filter_keytrack_base", "KeyTrk Base");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Filter Envelope */
    H_APPEND("\"flt_env\":{\"children\":null,"
             "\"knobs\":[\"flt_attack\",\"flt_decay\",\"flt_sustain\",\"flt_release\"],"
             "\"params\":[");
    H_PARAM("flt_attack", "Attack");
    H_PARAM("flt_decay", "Decay");
    H_PARAM("flt_sustain", "Sustain");
    H_PARAM("flt_sustain_time", "Sus Time");
    H_PARAM("flt_release", "Release");
    H_PARAM("filter1_env_polarity", "Flt1 Polarity");
    H_PARAM("filter2_env_polarity", "Flt2 Polarity");
    H_PARAM("flt1_envamt_velocity", "Flt1 Vel");
    H_PARAM("flt2_envamt_velocity", "Flt2 Vel");
    H_PARAM("resonance1_velocity", "Reso1 Vel");
    H_PARAM("resonance2_velocity", "Reso2 Vel");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Amp Envelope */
    H_APPEND("\"amp_env\":{\"children\":null,"
             "\"knobs\":[\"amp_attack\",\"amp_decay\",\"amp_sustain\",\"amp_release\"],"
             "\"params\":[");
    H_PARAM("amp_attack", "Attack");
    H_PARAM("amp_decay", "Decay");
    H_PARAM("amp_sustain", "Sustain");
    H_PARAM("amp_sustain_time", "Sus Time");
    H_PARAM("amp_release", "Release");
    H_PARAM("amp_velocity", "Velocity");
    H_PARAM("punch_intensity", "Punch");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* LFO 1 */
    H_APPEND("\"lfo1\":{\"children\":null,"
             "\"knobs\":[\"lfo1_rate\",\"lfo1_shape\",\"lfo1_symmetry\",\"osc1_lfo1_amount\"],"
             "\"params\":[");
    H_PARAM("lfo1_rate", "Rate");
    H_PARAM("lfo1_shape", "Shape");
    H_PARAM("lfo1_env_mode", "Env Mode");
    H_PARAM("lfo1_mode", "Mode");
    H_PARAM("lfo1_symmetry", "Symmetry");
    H_PARAM("lfo1_keyfollow", "KeyFlw");
    H_PARAM("lfo1_keytrigger", "KeyTrig");
    H_PARAM("lfo1_clock", "Clock");
    H_PARAM("osc1_lfo1_amount", "-> Osc1");
    H_PARAM("osc2_lfo1_amount", "-> Osc2");
    H_PARAM("pw_lfo1_amount", "-> PW");
    H_PARAM("reso_lfo1_amount", "-> Reso");
    H_PARAM("filtgain_lfo1_amount", "-> FiltGain");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* LFO 2 */
    H_APPEND("\"lfo2\":{\"children\":null,"
             "\"knobs\":[\"lfo2_rate\",\"lfo2_shape\",\"lfo2_symmetry\",\"shape_lfo2_amount\"],"
             "\"params\":[");
    H_PARAM("lfo2_rate", "Rate");
    H_PARAM("lfo2_shape", "Shape");
    H_PARAM("lfo2_env_mode", "Env Mode");
    H_PARAM("lfo2_mode", "Mode");
    H_PARAM("lfo2_symmetry", "Symmetry");
    H_PARAM("lfo2_keyfollow", "KeyFlw");
    H_PARAM("lfo2_keytrigger", "KeyTrig");
    H_PARAM("lfo2_clock", "Clock");
    H_PARAM("shape_lfo2_amount", "-> Shape");
    H_PARAM("fm_lfo2_amount", "-> FM");
    H_PARAM("cutoff1_lfo2_amount", "-> Cut1");
    H_PARAM("cutoff2_lfo2_amount", "-> Cut2");
    H_PARAM("pan_lfo2_amount", "-> Pan");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* LFO 3 */
    H_APPEND("\"lfo3\":{\"children\":null,"
             "\"knobs\":[\"lfo3_rate\",\"lfo3_shape\",\"lfo3_destination\",\"osc_lfo3_amount\"],"
             "\"params\":[");
    H_PARAM("lfo3_rate", "Rate");
    H_PARAM("lfo3_shape", "Shape");
    H_PARAM("lfo3_mode", "Mode");
    H_PARAM("lfo3_keyfollow", "KeyFlw");
    H_PARAM("lfo3_destination", "Destination");
    H_PARAM("osc_lfo3_amount", "Osc Amount");
    H_PARAM("lfo3_fadein_time", "Fade-In");
    H_PARAM("lfo3_clock", "Clock");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Arpeggiator */
    H_APPEND("\"arp\":{\"children\":null,"
             "\"knobs\":[\"arp_mode\",\"arp_pattern\",\"arp_octave_range\",\"arp_swing\"],"
             "\"params\":[");
    H_PARAM("arp_mode", "Mode");
    H_PARAM("arp_pattern", "Pattern");
    H_PARAM("arp_octave_range", "Octaves");
    H_PARAM("arp_hold", "Hold");
    H_PARAM("arp_note_length", "Note Length");
    H_PARAM("arp_swing", "Swing");
    H_PARAM("arp_clock", "Clock");
    H_PARAM("clock_tempo", "Tempo");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Effects */
    H_APPEND("\"fx\":{\"children\":null,"
             "\"knobs\":[\"chorus_mix\",\"effect_send\",\"delay_time\",\"delay_feedback\"],"
             "\"params\":[");
    H_PARAM("chorus_mix", "Chorus Mix");
    H_PARAM("chorus_rate", "Chorus Rate");
    H_PARAM("chorus_depth", "Chorus Depth");
    H_PARAM("chorus_delay", "Chorus Delay");
    H_PARAM("chorus_feedback", "Chorus Fdbk");
    H_PARAM("chorus_lfo_shape", "Chorus LFO");
    H_PARAM("delay_reverb_mode", "Dly/Rev Mode");
    H_PARAM("effect_send", "Effect Send");
    H_PARAM("delay_time", "Delay Time");
    H_PARAM("delay_feedback", "Delay Fdbk");
    H_PARAM("delay_rate_rev_decay", "Rate/Decay");
    H_PARAM("delay_depth", "Delay Depth");
    H_PARAM("delay_lfo_shape", "Delay LFO");
    H_PARAM("delay_color", "Delay Color");
    H_PARAM("delay_clock", "Delay Clock");
    if (model_level >= 1) {
        H_PARAM("phaser_mode", "Phaser Mode");
        H_PARAM("phaser_mix", "Phaser Mix");
        H_PARAM("phaser_rate", "Phaser Rate");
        H_PARAM("phaser_depth", "Phaser Depth");
        H_PARAM("phaser_frequency", "Phaser Freq");
        H_PARAM("phaser_feedback", "Phaser Fdbk");
        H_PARAM("phaser_spread", "Phaser Spread");
        H_PARAM("distortion_curve", "Dist Curve");
        H_PARAM("distortion_intensity", "Dist Amount");
        H_PARAM("bass_intensity", "Analog Boost");
        H_PARAM("bass_tune", "Boost Tune");
        H_PARAM("low_eq_freq", "Lo EQ Freq");
        H_PARAM("low_eq_gain", "Lo EQ Gain");
        H_PARAM("mid_eq_freq", "Mid EQ Freq");
        H_PARAM("mid_eq_gain", "Mid EQ Gain");
        H_PARAM("mid_eq_q", "Mid EQ Q");
        H_PARAM("high_eq_freq", "Hi EQ Freq");
        H_PARAM("high_eq_gain", "Hi EQ Gain");
    }
    if (model_level >= 2) {
        H_PARAM("vocoder_mode", "Vocoder Mode");
        H_PARAM("input_follower_mode", "Input Follow");
        H_PARAM("input_ringmod", "Input RingMod");
    }
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Mod Matrix */
    H_APPEND("\"mod\":{\"children\":null,"
             "\"knobs\":[\"assign1_amount\",\"assign2_amount1\",\"assign3_amount1\",\"lfo1_assign_amount\"],"
             "\"params\":[");
    H_PARAM("assign1_source", "Slot1 Src");
    H_PARAM("assign1_destination", "Slot1 Dst");
    H_PARAM("assign1_amount", "Slot1 Amt");
    H_PARAM("assign2_source", "Slot2 Src");
    H_PARAM("assign2_dest1", "Slot2 Dst1");
    H_PARAM("assign2_amount1", "Slot2 Amt1");
    H_PARAM("assign2_dest2", "Slot2 Dst2");
    H_PARAM("assign2_amount2", "Slot2 Amt2");
    H_PARAM("assign3_source", "Slot3 Src");
    H_PARAM("assign3_dest1", "Slot3 Dst1");
    H_PARAM("assign3_amount1", "Slot3 Amt1");
    H_PARAM("assign3_dest2", "Slot3 Dst2");
    H_PARAM("assign3_amount2", "Slot3 Amt2");
    H_PARAM("assign3_dest3", "Slot3 Dst3");
    H_PARAM("assign3_amount3", "Slot3 Amt3");
    H_PARAM("lfo1_assign_dest", "LFO1 Dst");
    H_PARAM("lfo1_assign_amount", "LFO1 Amt");
    H_PARAM("lfo2_assign_dest", "LFO2 Dst");
    H_PARAM("lfo2_assign_amount", "LFO2 Amt");
    if (model_level >= 1) {
        H_PARAM("assign4_source", "Slot4 Src");
        H_PARAM("assign4_destination", "Slot4 Dst");
        H_PARAM("assign4_amount", "Slot4 Amt");
        H_PARAM("assign5_source", "Slot5 Src");
        H_PARAM("assign5_destination", "Slot5 Dst");
        H_PARAM("assign5_amount", "Slot5 Amt");
        H_PARAM("assign6_source", "Slot6 Src");
        H_PARAM("assign6_destination", "Slot6 Dst");
        H_PARAM("assign6_amount", "Slot6 Amt");
    }
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Performance */
    H_APPEND("\"perf\":{\"children\":null,"
             "\"knobs\":[\"patch_volume\",\"panorama\",\"unison_mode\",\"portamento_time\"],"
             "\"params\":[");
    H_PARAM("patch_volume", "Volume");
    H_PARAM("panorama", "Panorama");
    H_PARAM("transpose", "Transpose");
    H_PARAM("key_mode", "Key Mode");
    H_PARAM("unison_mode", "Unison Mode");
    H_PARAM("unison_detune", "Unison Detune");
    H_PARAM("unison_pan_spread", "Unison Pan");
    H_PARAM("unison_lfo_phase", "Unison LFO Ph");
    H_PARAM("portamento_time", "Portamento");
    H_PARAM("bender_range_up", "Bend Up");
    H_PARAM("bender_range_down", "Bend Down");
    H_PARAM("osc1_shape_velocity", "Osc1 Shp Vel");
    H_PARAM("osc2_shape_velocity", "Osc2 Shp Vel");
    H_PARAM("pulsewidth_velocity", "PW Velocity");
    H_PARAM("fm_amount_velocity", "FM Amt Vel");
    if (buf[off-1] == ',') off--;
    H_APPEND("]},");

    /* Settings */
    H_APPEND("\"settings\":{\"children\":null,"
             "\"knobs\":[\"dsp_clock\",\"gain\"],"
             "\"params\":[");
    H_PARAM("rom_index", "ROM");
    H_PARAM("dsp_clock", "DSP Clock");
    H_PARAM("gain", "Gain");
    if (buf[off-1] == ',') off--;
    H_APPEND("]}");

    H_APPEND("}}");

    #undef H_APPEND
    #undef H_PARAM
    #undef H_LEVEL

    return off;
}
```

Then replace the ui_hierarchy handler:

```cpp
    if (strcmp(key, "ui_hierarchy") == 0) {
        return build_ui_hierarchy(buf, buf_len, (const char*)shm->rom_model_name);
    }
```

**Step 2: Build and verify**

Run: `./scripts/build.sh`

**Step 3: Commit**

```bash
git add src/dsp/virus_plugin.cpp
git commit -m "feat: rebuild UI hierarchy with full parameter coverage, model-aware"
```

---

### Task 8: Update module.json chain_params

**Files:**
- Modify: `src/module.json`

**Step 1: Update chain_params to include key sound design params**

The chain_params in module.json are the **static fallback** — the dynamic `chain_params` from `v2_get_param` takes precedence at runtime. Keep the static list lean with the most useful params for chain editing:

```json
    "chain_params": [
        {"key": "cutoff", "name": "Cutoff", "type": "int", "min": 0, "max": 127},
        {"key": "filter1_resonance", "name": "Resonance", "type": "int", "min": 0, "max": 127},
        {"key": "filter1_env_amt", "name": "Filt Env Amt", "type": "int", "min": 0, "max": 127},
        {"key": "flt_attack", "name": "Flt Attack", "type": "int", "min": 0, "max": 127},
        {"key": "flt_decay", "name": "Flt Decay", "type": "int", "min": 0, "max": 127},
        {"key": "flt_sustain", "name": "Flt Sustain", "type": "int", "min": 0, "max": 127},
        {"key": "flt_release", "name": "Flt Release", "type": "int", "min": 0, "max": 127},
        {"key": "amp_attack", "name": "Amp Attack", "type": "int", "min": 0, "max": 127},
        {"key": "amp_decay", "name": "Amp Decay", "type": "int", "min": 0, "max": 127},
        {"key": "amp_sustain", "name": "Amp Sustain", "type": "int", "min": 0, "max": 127},
        {"key": "amp_release", "name": "Amp Release", "type": "int", "min": 0, "max": 127},
        {"key": "filter1_mode", "name": "Filter Mode", "type": "int", "min": 0, "max": 7},
        {"key": "osc1_shape", "name": "Osc1 Shape", "type": "int", "min": 0, "max": 127},
        {"key": "osc2_shape", "name": "Osc2 Shape", "type": "int", "min": 0, "max": 127},
        {"key": "osc_balance", "name": "Osc Balance", "type": "int", "min": 0, "max": 127},
        {"key": "patch_volume", "name": "Volume", "type": "int", "min": 0, "max": 127}
    ]
```

Note: The key names have changed for some params (e.g., `resonance` -> `filter1_resonance`, `filter_env` -> `filter1_env_amt`, `filter_mode` -> `filter1_mode`). This is intentional to match the full parameter namespace. The dynamic chain_params handler will override this at runtime anyway.

**Step 2: Commit**

```bash
git add src/module.json
git commit -m "feat: update static chain_params with new parameter keys"
```

---

### Task 9: Build, Test, and Verify

**Step 1: Full build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-virus
./scripts/build.sh
```

**Step 2: Verify binary size is reasonable**

The expanded param table and UI hierarchy builder add ~10-15KB of static data. This should be fine.

**Step 3: Deploy and test on device**

```bash
./scripts/install.sh
```

Test checklist:
- [ ] Module loads with Virus A ROM — should show ~80 params (no B/C sections)
- [ ] Module loads with Virus B/C ROM — should show all ~140 params
- [ ] Page A params (cutoff, resonance, etc.) respond to changes
- [ ] Page B params (arp mode, LFO3, phaser, etc.) respond to changes
- [ ] Preset change clears overrides correctly
- [ ] State save/restore preserves all modified params
- [ ] ROM switching re-filters the UI hierarchy
- [ ] Slot presets capture and restore the full parameter state

**Step 4: Final commit with version bump**

Update version in `src/module.json` from `0.3.5` to `0.4.0`:

```bash
git add -A
git commit -m "feat: full Virus parameter coverage with model-aware filtering

- Expose ~140 sound design parameters (up from 16)
- Page A (CC) and Page B (polypressure) parameter access
- Model-aware filtering: A shows ~80 params, B/C shows all ~140
- 12-section UI hierarchy: Osc, Filter, Filter Env, Amp Env, LFO1/2/3, Arp, Effects, Mod Matrix, Performance, Settings
- B/C-only sections: Osc3, Phaser, Distortion, EQ, Mod Slots 4-6
- State version bumped to v3 for expanded parameter serialization"
```

---

## Migration Notes

- **State version 2 -> 3**: Old states (v2) will load bank/preset but skip per-parameter restore for new params. This is safe — the preset itself provides all default values.
- **Key renames**: `resonance` -> `filter1_resonance`, `filter_env` -> `filter1_env_amt`, `filter_mode` -> `filter1_mode`. Old states using these keys will fail to restore those specific params (they'll get preset defaults instead). This is acceptable.
- **Backward compat**: The old 16-param keys (`cutoff`, `osc1_shape`, etc.) that didn't change names will continue to work in old states.
