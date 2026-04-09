/*
 * Virus DSP Plugin for Move Anything
 *
 * Runs the DSP56300 JIT emulator in a CHILD PROCESS to avoid sharing
 * MoveOriginal's mmap_lock, heap allocator, and other kernel resources.
 * Communication between parent (plugin API) and child (DSP) uses a
 * shared memory region with an audio ring buffer and MIDI FIFO.
 *
 * Architecture:
 *   Parent (MoveOriginal process):
 *     - Plugin API v2 (create/destroy/render_block/on_midi/get_param/set_param)
 *     - render_block reads audio from shared memory ring buffer
 *     - on_midi writes to shared memory MIDI FIFO
 *     - get_param/set_param read/write shared memory status
 *
 *   Child (forked process):
 *     - Owns all gearmulator objects (DspSingle, Microcontroller, ROMFile)
 *     - DSP thread runs JIT-compiled DSP56300 code
 *     - Emu thread reads audio from DSP, resamples 46875→44100, writes to shared ring
 *     - MIDI consumer thread reads from shared MIDI FIFO, sends to Microcontroller
 *
 * GPL-3.0 License
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>

/* Gearmulator headers */
#include "virusLib/device.h"
#include "virusLib/dspSingle.h"
#include "virusLib/microcontroller.h"
#include "virusLib/romfile.h"
#include "virusLib/romloader.h"
#include "virusLib/deviceModel.h"
#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/semaphore.h"
#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"
#include "program_selection.h"

/* Plugin API v2 (inline definitions to avoid path issues) */
extern "C" {

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

} /* extern "C" */

/* =====================================================================
 * Constants
 * ===================================================================== */

#define DEVICE_RATE         46875.0f
#define AUDIO_RING_SIZE     8192
#define EMU_CHUNK           64
#define OUTPUT_GAIN         0.7f
#define MIDI_FIFO_SIZE      4096  /* bytes */
#define RING_TARGET_FILL    768   /* ~17ms at 44100 Hz — extra headroom for note-on bursts */
#define VIRUS_STATE_VERSION 3

static const host_api_v1_t *g_host = nullptr;

/* Microsecond clock */
static int64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Logging helper */
static void plugin_log(const char *fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    char msg[560];
    snprintf(msg, sizeof(msg), "[virus] %s", buf);
    g_host->log(msg);
}

/* Crash-safe log: writes to a dedicated file with immediate flush. */
static FILE *g_vlog = nullptr;
static void vlog(const char *fmt, ...) {
    if (!g_vlog)
        g_vlog = fopen("/data/UserData/schwung/virus_debug.log", "a");
    if (!g_vlog) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_vlog, fmt, args);
    va_end(args);
    fputc('\n', g_vlog);
    fflush(g_vlog);
}

/* =====================================================================
 * Virus CC parameter mapping
 * ===================================================================== */

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
    {"input_mode",           "Input Mode",       VIRUS_PAGE_A,101, 0,   3, VIRUS_MODEL_ALL},
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
static constexpr int VIRUS_MAX_BANKS = 32;
static constexpr int VIRUS_MAX_PRESETS_PER_BANK = 128;
static constexpr int VIRUS_PRESET_NAME_BYTES = 24;

/* =====================================================================
 * Shared memory structure (parent <-> child process)
 * ===================================================================== */

struct virus_shm_t {
    /* Audio ring buffer (child writes, parent reads) */
    int16_t audio_ring[AUDIO_RING_SIZE * 2];
    volatile int ring_read;
    volatile int ring_write;

    /* MIDI input FIFO (parent writes, child reads)
     * Format: [len, byte0, byte1, byte2, ...] per message, len=1..8 */
    uint8_t midi_buf[MIDI_FIFO_SIZE];
    volatile int midi_read;
    volatile int midi_write;

    /* Control flags */
    volatile int child_ready;       /* child sets when boot complete */
    volatile int child_shutdown;    /* parent sets to request shutdown */
    volatile int child_alive;       /* child increments periodically */

    /* Status (child writes, parent reads) */
    volatile int initialized;
    volatile int loading_complete;
    char loading_status[128];
    char load_error[256];
    char preset_name[64];
    char bank_name[32];
    volatile int current_bank;
    volatile int current_preset;
    volatile int bank_count;
    volatile int preset_count;
    volatile int preset_name_cache_ready;
    char preset_name_cache[VIRUS_MAX_BANKS][VIRUS_MAX_PRESETS_PER_BANK][VIRUS_PRESET_NAME_BYTES];
    volatile int octave_transpose;
    volatile int cc_values[128];
    uint8_t cc_seen[128];
    volatile int cc_values_b[128];  /* Page B parameter values */
    uint8_t cc_seen_b[128];         /* Page B parameter seen flags */

    /* Profiling */
    volatile int underrun_count;
    volatile int emu_blocks;
    volatile int render_count;
    volatile int64_t prof_process_us_total;
    volatile int prof_process_max_us;
    volatile float prof_peak_level;
    volatile int prof_ring_min;
    volatile int64_t prof_start_us;

    /* Module directory (set by parent before fork) */
    char module_dir[256];

    /* DSP clock percent (parent writes, child reads and applies) */
    volatile int dsp_clock_percent;  /* 0 = auto (100 for A, 50 for B/C) */
    volatile int dsp_clock_applied;  /* last value applied by child */
    char rom_model_name[16];         /* "A", "B", "C", etc. */

    /* Output gain (parent writes, child reads — applied during resample) */
    volatile int gain_percent;       /* 0 = auto (70), else 1..100 */

    /* ROM selection (child enumerates, parent selects) */
    volatile int rom_index;          /* which ROM to load (0-based) */
    volatile int rom_count;          /* how many ROMs found (child writes) */
    char rom_names[8][64];           /* ROM display names (child writes) */
};

/* =====================================================================
 * Shared memory ring buffer helpers
 * ===================================================================== */

static int shm_ring_available(virus_shm_t *shm) {
    int avail = shm->ring_write - shm->ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int shm_ring_free(virus_shm_t *shm) {
    return AUDIO_RING_SIZE - 1 - shm_ring_available(shm);
}

/* MIDI FIFO helpers */
static int midi_fifo_available(virus_shm_t *shm) {
    int avail = shm->midi_write - shm->midi_read;
    if (avail < 0) avail += MIDI_FIFO_SIZE;
    return avail;
}

static int midi_fifo_free(virus_shm_t *shm) {
    return MIDI_FIFO_SIZE - 1 - midi_fifo_available(shm);
}

static void clear_param_overrides(virus_shm_t *shm) {
    if (!shm) return;
    memset(shm->cc_seen, 0, sizeof(shm->cc_seen));
    memset(shm->cc_seen_b, 0, sizeof(shm->cc_seen_b));
    memset((void*)shm->cc_values, 0, sizeof(shm->cc_values));
    memset((void*)shm->cc_values_b, 0, sizeof(shm->cc_values_b));
}

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

static uint8_t bank_index_to_midi_lsb(int bank_index, int bank_count) {
    int idx = bank_index;
    int max_idx = bank_count > 0 ? bank_count - 1 : 0;
    if (idx < 0) idx = 0;
    if (idx > max_idx) idx = max_idx;
    int midi = idx + 1; /* Access uses 1-based bank numbers: A=1..H=8. */
    if (midi < 1) midi = 1;
    if (midi > 127) midi = 127;
    return (uint8_t)midi;
}

static const char *shm_lookup_preset_name(virus_shm_t *shm, int bank, int preset) {
    if (!shm || !shm->preset_name_cache_ready) return nullptr;
    if (bank < 0 || preset < 0) return nullptr;
    if (bank >= shm->bank_count || preset >= shm->preset_count) return nullptr;
    if (bank >= VIRUS_MAX_BANKS || preset >= VIRUS_MAX_PRESETS_PER_BANK) return nullptr;
    const char *name = shm->preset_name_cache[bank][preset];
    if (!name[0]) return nullptr;
    return name;
}

static void shm_refresh_current_preset_name(virus_shm_t *shm) {
    if (!shm) return;
    const char *name = shm_lookup_preset_name(shm, shm->current_bank, shm->current_preset);
    if (name) {
        snprintf((char*)shm->preset_name, sizeof(shm->preset_name), "%s", name);
    } else {
        snprintf((char*)shm->preset_name, sizeof(shm->preset_name), "---");
    }
}

static void midi_fifo_push(virus_shm_t *shm, const uint8_t *msg, int len) {
    if (len < 1 || len > 16) return;
    if (midi_fifo_free(shm) < len + 1) return; /* drop if full */
    int wr = shm->midi_write;
    shm->midi_buf[wr] = (uint8_t)len;
    wr = (wr + 1) % MIDI_FIFO_SIZE;
    for (int i = 0; i < len; i++) {
        shm->midi_buf[wr] = msg[i];
        wr = (wr + 1) % MIDI_FIFO_SIZE;
    }
    shm->midi_write = wr;
}

static void send_param_midi(virus_shm_t *shm, const virus_param_t *p, int value) {
    if (p->page == VIRUS_PAGE_A) {
        shm->cc_values[p->cc] = value;
        shm->cc_seen[p->cc] = 1;
        uint8_t msg[3] = { 0xB0, (uint8_t)p->cc, (uint8_t)value };
        midi_fifo_push(shm, msg, 3);
    } else {
        shm->cc_values_b[p->cc] = value;
        shm->cc_seen_b[p->cc] = 1;
        /* Send Page B params as SysEx parameter change instead of polypressure.
         * This avoids needing MIDI_CONTROL_HIGH_PAGE which would hijack
         * polypressure (aftertouch) from the pads. SysEx format:
         * F0 00 20 33 01 10 71 <part> <param> <value> F7 */
        uint8_t sysex[11] = {
            0xF0, 0x00, 0x20, 0x33, 0x01, 0x10,  /* header + OMNI device */
            0x71,                                  /* PAGE_B */
            0x40,                                  /* SINGLE part */
            (uint8_t)p->cc,                        /* param index */
            (uint8_t)value,                        /* value */
            0xF7                                   /* end of sysex */
        };
        midi_fifo_push(shm, sysex, 11);
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

/* =====================================================================
 * Instance structure (parent process only)
 * ===================================================================== */

struct virus_instance_t {
    virus_shm_t *shm;
    pid_t child_pid;
    pthread_t boot_thread;
    volatile int boot_thread_running;
    char *pending_state;
    int pending_state_valid;

    /* Per-note pressure tracking for polypressure → channel pressure.
     * The Virus only has mono aftertouch (ChanPres), so we convert
     * poly aftertouch by taking the max pressure across active notes. */
    uint8_t note_pressure[128];
};

/* =====================================================================
 * Child process — all DSP work happens here
 * ===================================================================== */

/* Resample ratio: 46875 Hz source → 44100 Hz output */
static constexpr double RESAMPLE_RATIO = 46875.0 / 44100.0;
#define RESAMPLE_MAX_OUT (EMU_CHUNK + 4)

static void child_send_midi(virusLib::Microcontroller *mc, const uint8_t *msg, int len) {
    if (!mc || len < 1) return;
    synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host,
                            msg[0],
                            len > 1 ? msg[1] : 0,
                            len > 2 ? msg[2] : 0);
    mc->sendMIDI(ev);
}

static bool child_use_mc_preset_map(virusLib::ROMFile *rom) {
    return rom && rom->getModel() == virusLib::DeviceModel::A;
}

static int child_map_browser_bank_to_mc_bank(virusLib::ROMFile *rom, int bank) {
    if (bank < 0) return bank;
    if (!child_use_mc_preset_map(rom)) return bank;
    /* Virus A MC single-bank table starts with 2 RAM banks, then ROM A..H.
     * Browser bank 0..7 should map to ROM banks, not RAM mirror slots. */
    return bank + 2;
}

static bool child_get_single_preset(virusLib::Microcontroller *mc,
                                    virusLib::ROMFile *rom,
                                    int bank,
                                    int preset,
                                    virusLib::ROMFile::TPreset *out) {
    if (!out || bank < 0 || preset < 0) return false;

    if (child_use_mc_preset_map(rom) && mc) {
        const int mapped_bank = child_map_browser_bank_to_mc_bank(rom, bank);
        if (mapped_bank < 0 || mapped_bank >= VIRUS_MAX_BANKS || preset >= VIRUS_MAX_PRESETS_PER_BANK)
            return false;
        return mc->requestSingle(virusLib::fromArrayIndex((uint8_t)mapped_bank), (uint8_t)preset, *out);
    }

    if (!rom) return false;
    return rom->getSingle(bank, preset, *out);
}

static bool child_validate_virus_a_bank(virusLib::Microcontroller *mc,
                                        virusLib::ROMFile *rom,
                                        int bank,
                                        int preset_count) {
    if (!child_use_mc_preset_map(rom)) return true;
    if (!mc || !rom || bank < 0 || preset_count <= 0) return false;

    int p0 = 0;
    int p1 = preset_count > 1 ? 1 : 0;
    int plast = preset_count - 1;
    const int probes[3] = {p0, p1, plast};

    for (int i = 0; i < 3; ++i) {
        int p = probes[i];
        virusLib::ROMFile::TPreset pd{};
        if (!child_get_single_preset(mc, rom, bank, p, &pd)) return false;

        if (pd[2] != (uint8_t)bank) return false;
        if (pd[3] != (uint8_t)p) return false;
        if (virusLib::ROMFile::getSingleName(pd).size() != 10) return false;
    }

    return true;
}

static void child_build_preset_name_cache(virus_shm_t *shm,
                                          virusLib::Microcontroller *mc,
                                          virusLib::ROMFile *rom);
static void child_update_preset_name(virus_shm_t *shm,
                                     virusLib::Microcontroller *mc,
                                     virusLib::ROMFile *rom);
static int child_detect_valid_bank_count(virusLib::Microcontroller *mc,
                                         virusLib::ROMFile *rom,
                                         int fallback_bank_count);

/* Populate cc_values / cc_values_b from preset data so the UI
 * reflects the actual preset values after a program change.
 * Preset layout: Page A = bytes 0-127, Page B = bytes 128-255. */
static void child_sync_params_from_preset(virus_shm_t *shm,
                                          const virusLib::ROMFile::TPreset &single) {
    for (int i = 0; i < NUM_PARAMS; i++) {
        int offset = g_params[i].page == VIRUS_PAGE_A
            ? g_params[i].cc
            : g_params[i].cc + 128;
        int val = single[offset];
        if (val < g_params[i].min_val) val = g_params[i].min_val;
        if (val > g_params[i].max_val) val = g_params[i].max_val;
        if (g_params[i].page == VIRUS_PAGE_A)
            shm->cc_values[g_params[i].cc] = val;
        else
            shm->cc_values_b[g_params[i].cc] = val;
    }
}

/* Process at most max_msgs MIDI messages per call.
 * Rate-limiting spreads note-on voice allocation across emu blocks,
 * preventing DSP cycle bursts that cause audio dropouts. */
static void child_process_midi_fifo(virus_shm_t *shm,
                                    virusLib::Microcontroller *mc,
                                    virusLib::ROMFile *rom,
                                    int max_msgs = 2) {
    int processed = 0;
    while (midi_fifo_available(shm) > 0 && processed < max_msgs) {
        int rd = shm->midi_read;
        int len = shm->midi_buf[rd];
        rd = (rd + 1) % MIDI_FIFO_SIZE;
        if (len < 1 || len > 16 || midi_fifo_available(shm) < len + 1) break;
        uint8_t msg[16];
        for (int i = 0; i < len; i++) {
            msg[i] = shm->midi_buf[rd];
            rd = (rd + 1) % MIDI_FIFO_SIZE;
        }
        shm->midi_read = rd;

        /* Track CC values in shared memory */
        uint8_t status = msg[0] & 0xF0;

        if (status == 0xB0 && len >= 3) {
            shm->cc_values[msg[1] & 0x7F] = msg[2] & 0x7F;
            shm->cc_seen[msg[1] & 0x7F] = 1;
        }
        /* Page B params now arrive as SysEx, tracked in send_param_midi */

        int bank = shm->current_bank;
        int preset = shm->current_preset;
        const int change_mask = apply_program_selection_midi(msg, len, shm->bank_count, shm->preset_count, &bank, &preset);
        if (change_mask != PROGRAM_SELECTION_NONE) {
            shm->current_bank = bank;
            shm->current_preset = preset;
            if ((change_mask & PROGRAM_SELECTION_BANK_CHANGED) != 0)
                snprintf((char*)shm->bank_name, sizeof(shm->bank_name), "Bank %c", 'A' + bank);
        }

        const bool is_bank_select = (status == 0xB0 && len >= 3 && (msg[1] == 0 || msg[1] == 32));
        const bool is_program_change = (status == 0xC0 && len >= 2);

        if (is_program_change) {
            /* Load from the same source we use for the browser cache so names
             * and audible patch content stay in lockstep. */
            virusLib::ROMFile::TPreset single{};
            if (child_get_single_preset(mc, rom, shm->current_bank, shm->current_preset, &single)) {
                mc->writeSingle(virusLib::BankNumber::EditBuffer, virusLib::SINGLE, single);
                child_sync_params_from_preset(shm, single);
            }
            child_update_preset_name(shm, mc, rom);
            processed++;
            continue;
        }

        if (is_bank_select) {
            /* Bank select is pending until Program Change commit. */
            if ((change_mask & PROGRAM_SELECTION_BANK_CHANGED) != 0)
                child_update_preset_name(shm, mc, rom);
            processed++;
            continue;
        }

        /* Route SysEx (e.g. Page B param changes) through sendSysex */
        if (status == 0xF0 && len >= 7 && msg[len-1] == 0xF7) {
            synthLib::SysexBuffer sysex(msg, msg + len);
            std::vector<synthLib::SMidiEvent> responses;
            mc->sendSysex(sysex, responses, synthLib::MidiEventSource::Host);
            /* Don't count param SysEx against rate limit — they're lightweight
             * and shouldn't delay note-on or aftertouch processing. */
            continue;
        }

        child_send_midi(mc, msg, len);

        if ((change_mask & (PROGRAM_SELECTION_BANK_CHANGED | PROGRAM_SELECTION_PRESET_CHANGED)) != 0) {
            child_update_preset_name(shm, mc, rom);
        }

        processed++;
    }
}

static int child_detect_valid_bank_count(virusLib::Microcontroller *mc,
                                         virusLib::ROMFile *rom,
                                         int fallback_bank_count) {
    if (!rom) return fallback_bank_count > 0 ? fallback_bank_count : 1;

    int preset_count = rom->getPresetsPerBank();
    if (preset_count <= 0) preset_count = VIRUS_MAX_PRESETS_PER_BANK;
    if (preset_count > VIRUS_MAX_PRESETS_PER_BANK) preset_count = VIRUS_MAX_PRESETS_PER_BANK;

    int detected = 0;
    virusLib::ROMFile::TPreset pd{};
    for (int bank = 0; bank < VIRUS_MAX_BANKS; ++bank) {
        if (!child_get_single_preset(mc, rom, bank, 0, &pd)) break;

        if (child_use_mc_preset_map(rom)) {
            if (!child_validate_virus_a_bank(mc, rom, bank, preset_count)) break;
        }

        detected++;
    }

    if (detected > 0) return detected;
    return fallback_bank_count > 0 ? fallback_bank_count : 1;
}

static void child_build_preset_name_cache(virus_shm_t *shm,
                                          virusLib::Microcontroller *mc,
                                          virusLib::ROMFile *rom) {
    if (!shm) return;
    shm->preset_name_cache_ready = 0;
    memset((void*)shm->preset_name_cache, 0, sizeof(shm->preset_name_cache));
    if (!rom || !mc) return;

    int bank_count = shm->bank_count;
    if (bank_count < 0) bank_count = 0;
    if (bank_count > VIRUS_MAX_BANKS) bank_count = VIRUS_MAX_BANKS;
    int preset_count = shm->preset_count;
    if (preset_count < 0) preset_count = 0;
    if (preset_count > VIRUS_MAX_PRESETS_PER_BANK) preset_count = VIRUS_MAX_PRESETS_PER_BANK;

    virusLib::ROMFile::TPreset pd{};
    for (int bank = 0; bank < bank_count; ++bank) {
        for (int preset = 0; preset < preset_count; ++preset) {
            if (!child_get_single_preset(mc, rom, bank, preset, &pd)) continue;
            const std::string name = virusLib::ROMFile::getSingleName(pd);
            snprintf(shm->preset_name_cache[bank][preset],
                     sizeof(shm->preset_name_cache[bank][preset]),
                     "%s",
                     name.empty() ? "---" : name.c_str());
        }
    }

    shm->preset_name_cache_ready = 1;
}

static void child_update_preset_name(virus_shm_t *shm,
                                     virusLib::Microcontroller *mc,
                                     virusLib::ROMFile *rom) {
    const char *cached = shm_lookup_preset_name(shm, shm->current_bank, shm->current_preset);
    if (cached) {
        snprintf((char*)shm->preset_name, sizeof(shm->preset_name), "%s", cached);
        return;
    }

    virusLib::ROMFile::TPreset pd{};
    if (child_get_single_preset(mc, rom, shm->current_bank, shm->current_preset, &pd))
        snprintf((char*)shm->preset_name, sizeof(shm->preset_name), "%s",
                 virusLib::ROMFile::getSingleName(pd).c_str());
    else
        snprintf((char*)shm->preset_name, sizeof(shm->preset_name), "---");
}

static virus_shm_t *g_child_shm = nullptr;

static void child_crash_handler(int sig) {
    /* Write crash info to debug log before dying */
    FILE *f = fopen("/data/UserData/schwung/virus_debug.log", "a");
    if (f) {
        fprintf(f, "[child] CRASHED signal=%d (%s) pid=%d\n",
                sig, sig == 11 ? "SIGSEGV" : sig == 7 ? "SIGBUS" : sig == 6 ? "SIGABRT" : "?",
                (int)getpid());
        fflush(f);
        fclose(f);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void child_main(virus_shm_t *shm) {
    g_child_shm = shm;

    /* Install crash handler so we get diagnostics */
    signal(SIGSEGV, child_crash_handler);
    signal(SIGBUS, child_crash_handler);
    signal(SIGABRT, child_crash_handler);

    /* After fork in a multithreaded process, avoid stdio teardown inherited
     * from parent; just drop the pointer and reopen lazily in child. */
    g_vlog = nullptr;

    vlog("[child] started, pid=%d", (int)getpid());
    fprintf(stderr, "Virus child: started pid=%d\n", (int)getpid());

    /* 1. Load ROM */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Loading ROM...");
    char roms_dir[512];
    snprintf(roms_dir, sizeof(roms_dir), "%s/roms", shm->module_dir);
    auto roms = virusLib::ROMLoader::findROMs(std::string(roms_dir));
    if (roms.empty()) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error),
                 "No Virus ROM found in roms/ directory.");
        shm->initialized = 1; shm->loading_complete = 1;
        vlog("[child] no ROM found, exiting");
        return;
    }

    /* Sort ROMs by model (A=0, B=1, C=2, ...) so enum goes A → B → C */
    std::sort(roms.begin(), roms.end(), [](const virusLib::ROMFile &a, const virusLib::ROMFile &b) {
        return static_cast<int>(a.getModel()) < static_cast<int>(b.getModel());
    });

    /* Enumerate all ROMs for the UI */
    shm->rom_count = (int)roms.size();
    if (shm->rom_count > 8) shm->rom_count = 8;
    for (int i = 0; i < shm->rom_count; i++) {
        virusLib::ROMFile tmp{roms[i]};
        snprintf(shm->rom_names[i], sizeof(shm->rom_names[i]), "Virus %s",
                 tmp.getModelName().c_str());
        vlog("[child] ROM[%d]: %s", i, shm->rom_names[i]);
    }

    /* Pick the selected ROM */
    int idx = shm->rom_index;
    if (idx < 0 || idx >= shm->rom_count) idx = 0;
    virusLib::ROMFile *rom = new virusLib::ROMFile(std::move(roms[idx]));
    vlog("[child] ROM loaded: %s model=%s (index %d/%d)",
         rom->getFilename().c_str(), rom->getModelName().c_str(), idx, shm->rom_count);

    /* 2. Create DSP instances */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Creating DSP...");
    vlog("[child] creating DspSingle...");
    virusLib::DspSingle *dsp1 = nullptr;
    virusLib::DspSingle *dsp2 = nullptr;
    try {
        virusLib::Device::createDspInstances(dsp1, dsp2, *rom, DEVICE_RATE);
    } catch (const std::exception& e) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error), "DSP creation failed: %s", e.what());
        shm->initialized = 1; shm->loading_complete = 1;
        vlog("[child] DSP creation failed: %s", e.what());
        delete rom;
        return;
    }
    vlog("[child] createDspInstances OK");

    /* 3. Create Microcontroller */
    vlog("[child] creating Microcontroller...");
    virusLib::Microcontroller *mc = new virusLib::Microcontroller(*dsp1, *rom, false);

    /* 4. Set up semaphore and audio callback */
    dsp56k::SpscSemaphore audio_sem(1);
    std::atomic<int32_t> notify_timeout{0};
    std::atomic<uint32_t> callback_count{0};
    std::atomic<bool> sem_active{false};

    auto& audio = dsp1->getAudio();
    audio.setCallback([&](dsp56k::Audio* a) {
        mc->getMidiQueue(0).onAudioWritten();
        uint32_t count = callback_count.fetch_add(1) + 1;
        if ((count & 0x3) == 0) {
            std::vector<synthLib::SMidiEvent> midiOut;
            mc->readMidiOut(midiOut);
            mc->process();
        }
        if (sem_active.load(std::memory_order_relaxed)) {
            const auto avail = a->getAudioOutputs().size();
            int32_t timeout = notify_timeout.load();
            timeout--;
            if (timeout <= 0 && avail >= (EMU_CHUNK - 4)) {
                timeout = (EMU_CHUNK - 4);
                audio_sem.notify();
            }
            notify_timeout.store(timeout);
        }
    }, 0);

    /* 5. Boot DSPs */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Booting DSP...");
    vlog("[child] bootDSPs...");
    virusLib::Device::bootDSPs(dsp1, dsp2, *rom, false);
    vlog("[child] bootDSPs OK");

    /* 6. Boot drain */
    {
        constexpr int BOOT_CHUNK = 8;
        float dummy_l[BOOT_CHUNK], dummy_r[BOOT_CHUNK];
        synthLib::TAudioInputs inputs = {};
        synthLib::TAudioOutputs outputs = {};
        outputs[0] = dummy_l; outputs[1] = dummy_r;

        int retries = 0;
        if (rom->getModel() == virusLib::DeviceModel::A) {
            for (int i = 0; i < 32; i++)
                dsp1->processAudio(inputs, outputs, BOOT_CHUNK, 0);
            dsp1->disableESSI1();
            retries = 32;
        } else {
            while (!mc->dspHasBooted() && retries < 512) {
                if (shm->child_shutdown) goto cleanup;
                dsp1->processAudio(inputs, outputs, BOOT_CHUNK, 0);
                retries++;
            }
        }
        vlog("[child] boot drain: %d cycles (%d frames)", retries, retries * BOOT_CHUNK);

        /* 7. Initialize */
        mc->sendInitControlCommands(127);
        for (int i = 0; i < 8; i++)
            dsp1->processAudio(inputs, outputs, BOOT_CHUNK, 0);
        mc->createDefaultState();
        for (int i = 0; i < 8; i++)
            dsp1->processAudio(inputs, outputs, BOOT_CHUNK, 0);

        /* MIDI_CONTROL_HIGH_PAGE stays disabled (default from
         * sendInitControlCommands). Page B params are sent as SysEx
         * parameter changes, so polypressure passes through to the
         * DSP as aftertouch without corrupting parameters. */
    }
    vlog("[child] DSP initialized");

    /* 7b. Apply DSP clock scaling.
     * Virus A (72 MHz) runs fine at 100%. Virus B/C (108 MHz) need reduction
     * to fit Move's A72 CPU budget. User can override via dsp_clock param. */
    {
        int pct = shm->dsp_clock_percent;
        if (pct <= 0) {
            switch (rom->getModel()) {
                case virusLib::DeviceModel::A: pct = 100; break;
                case virusLib::DeviceModel::B: pct = 45;  break;
                default:                       pct = 35;  break; /* C and others */
            }
        }
        dsp1->getEsxiClock().setSpeedPercent(pct);
        shm->dsp_clock_applied = pct;
        vlog("[child] DSP clock set to %d%% for model %s", pct, rom->getModelName().c_str());
    }
    strncpy((char*)shm->rom_model_name, rom->getModelName().c_str(), sizeof(shm->rom_model_name) - 1);

    /* 8. Set up presets */
    shm->bank_count = child_detect_valid_bank_count(
        mc, rom, (int)virusLib::ROMFile::getRomBankCount(rom->getModel()));
    shm->preset_count = rom->getPresetsPerBank();
    shm->current_bank = 0;
    shm->current_preset = 0;
    snprintf((char*)shm->bank_name, sizeof(shm->bank_name), "Bank A");
    child_build_preset_name_cache(shm, mc, rom);
    child_update_preset_name(shm, mc, rom);

    { /* Initial preset — use writeSingle for all models so the EditBuffer
       * is reliably populated before the emu loop starts producing audio. */
        virusLib::ROMFile::TPreset single{};
        if (child_get_single_preset(mc, rom, shm->current_bank, shm->current_preset, &single)) {
            mc->writeSingle(virusLib::BankNumber::EditBuffer, virusLib::SINGLE, single);
            child_sync_params_from_preset(shm, single);
        }
    }

    /* 9. Pre-fill ring buffer */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Warming up...");
    {
        float wl[EMU_CHUNK], wr_buf[EMU_CHUNK];
        synthLib::TAudioInputs inputs = {};
        synthLib::TAudioOutputs outputs = {};
        outputs[0] = wl; outputs[1] = wr_buf;

        for (int fill = 0; fill < 64 && shm_ring_free(shm) >= EMU_CHUNK; fill++) {
            dsp1->processAudio(inputs, outputs, EMU_CHUNK, 0);
            int wr = shm->ring_write;
            for (int i = 0; i < EMU_CHUNK; i++) {
                int32_t l = (int32_t)(wl[i] * 32767.0f);
                int32_t r = (int32_t)(wr_buf[i] * 32767.0f);
                if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                shm->audio_ring[wr * 2 + 0] = (int16_t)l;
                shm->audio_ring[wr * 2 + 1] = (int16_t)r;
                wr = (wr + 1) % AUDIO_RING_SIZE;
            }
            shm->ring_write = wr;
        }
    }
    vlog("[child] pre-filled %d frames", shm_ring_available(shm));

    /* 10. Signal ready and enter emu loop */
    shm->initialized = 1;
    shm->loading_complete = 1;
    shm->child_ready = 1;
    sem_active.store(true);
    notify_timeout.store(0);
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status),
             "Ready: %d banks, %d presets/bank", shm->bank_count, shm->preset_count);
    vlog("[child] READY! entering emu loop");

    /* === Emu loop (runs until shutdown) === */
    {
        shm->prof_start_us = now_us();
        float proc_l[EMU_CHUNK], proc_r[EMU_CHUNK];
        double resample_phase = 0.0;
        float resample_prev_l = 0.0f, resample_prev_r = 0.0f;

        while (!shm->child_shutdown) {
            /* Process incoming MIDI from parent */
            child_process_midi_fifo(shm, mc, rom);

            /* Throttle: don't let ring fill beyond target (keeps latency low) */
            if (shm_ring_available(shm) >= RING_TARGET_FILL) {
                usleep(500);
                continue;
            }

            /* Wait for DSP to produce enough frames */
            audio_sem.wait();
            if (shm->child_shutdown) break;

            synthLib::TAudioInputs inputs = {};
            synthLib::TAudioOutputs outputs = {};
            outputs[0] = proc_l; outputs[1] = proc_r;

            int64_t t0 = now_us();
            dsp1->processAudio(inputs, outputs, EMU_CHUNK, EMU_CHUNK);
            int64_t dt = now_us() - t0;
            shm->prof_process_us_total += dt;
            if ((int)dt > shm->prof_process_max_us) shm->prof_process_max_us = (int)dt;
            shm->emu_blocks++;
            shm->child_alive++;

            /* Peak tracking */
            for (int i = 0; i < EMU_CHUNK; i++) {
                float peak = fabsf(proc_l[i]);
                float pr = fabsf(proc_r[i]);
                if (pr > peak) peak = pr;
                if (peak > shm->prof_peak_level) shm->prof_peak_level = peak;
            }

            /* Check for dynamic clock change */
            {
                int want = shm->dsp_clock_percent;
                if (want > 0 && want != shm->dsp_clock_applied) {
                    dsp1->getEsxiClock().setSpeedPercent(want);
                    shm->dsp_clock_applied = want;
                    vlog("[child] DSP clock changed to %d%%", want);
                }
            }

            /* Periodic stats */
            if ((shm->emu_blocks % 1465) == 1) {
                int64_t now = now_us();
                double elapsed_s = (now - shm->prof_start_us) / 1000000.0;
                double mips = 0;
                if (elapsed_s > 0.1)
                    mips = (double)shm->emu_blocks * EMU_CHUNK * 2304.0 / elapsed_s / 1000000.0;
                vlog("[child] blk=%d buf=%d ur=%d mips=%.1f peak=%.3f",
                     shm->emu_blocks, shm_ring_available(shm), shm->underrun_count, mips,
                     (double)shm->prof_peak_level);
                shm->prof_peak_level = 0.0f;
            }

            /* Resample 46875→44100 */
            float ext_l[EMU_CHUNK + 1], ext_r[EMU_CHUNK + 1];
            ext_l[0] = resample_prev_l; ext_r[0] = resample_prev_r;
            memcpy(ext_l + 1, proc_l, EMU_CHUNK * sizeof(float));
            memcpy(ext_r + 1, proc_r, EMU_CHUNK * sizeof(float));

            float gain = (shm->gain_percent > 0) ? (shm->gain_percent / 100.0f) : OUTPUT_GAIN;

            int16_t resampled[RESAMPLE_MAX_OUT * 2];
            int out_count = 0;
            while (resample_phase < (double)EMU_CHUNK && out_count < RESAMPLE_MAX_OUT) {
                double ext_pos = resample_phase + 1.0;
                int idx = (int)ext_pos;
                double frac = ext_pos - idx;
                if (idx >= EMU_CHUNK) break;
                float l = ext_l[idx] * (float)(1.0 - frac) + ext_l[idx + 1] * (float)frac;
                float r = ext_r[idx] * (float)(1.0 - frac) + ext_r[idx + 1] * (float)frac;
                int32_t li = (int32_t)(l * gain * 32767.0f);
                int32_t ri = (int32_t)(r * gain * 32767.0f);
                if (li > 32767) li = 32767; if (li < -32768) li = -32768;
                if (ri > 32767) ri = 32767; if (ri < -32768) ri = -32768;
                resampled[out_count * 2 + 0] = (int16_t)li;
                resampled[out_count * 2 + 1] = (int16_t)ri;
                out_count++;
                resample_phase += RESAMPLE_RATIO;
            }
            resample_phase -= (double)EMU_CHUNK;
            resample_prev_l = proc_l[EMU_CHUNK - 1];
            resample_prev_r = proc_r[EMU_CHUNK - 1];

            /* Write to shared ring buffer */
            if (shm_ring_free(shm) < out_count) continue;
            int wr = shm->ring_write;
            for (int i = 0; i < out_count; i++) {
                shm->audio_ring[wr * 2 + 0] = resampled[i * 2 + 0];
                shm->audio_ring[wr * 2 + 1] = resampled[i * 2 + 1];
                wr = (wr + 1) % AUDIO_RING_SIZE;
            }
            shm->ring_write = wr;
        }
    }

cleanup:
    vlog("[child] shutting down");
    audio.setCallback(nullptr, 0);
    audio.terminate();
    delete mc;
    delete dsp1;
    delete rom;
    vlog("[child] exiting");
}

/* =====================================================================
 * Boot / restart helpers (parent process)
 * ===================================================================== */

/* Fork child and wait for it to be ready. Returns 0 on success. */
static int fork_and_wait_child(virus_instance_t *inst) {
    virus_shm_t *shm = inst->shm;

    vlog("fork_and_wait: forking child process for DSP...");

    pid_t pid = fork();
    if (pid < 0) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error), "fork() failed: %s", strerror(errno));
        shm->initialized = 1; shm->loading_complete = 1;
        return -1;
    }

    if (pid == 0) {
        /* === CHILD PROCESS === */
        g_vlog = nullptr;
        child_main(shm);
        _exit(0);
    }

    /* === PARENT PROCESS === */
    inst->child_pid = pid;
    vlog("fork_and_wait: child forked, pid=%d", (int)pid);

    for (int i = 0; i < 600 && !shm->child_ready; i++) {
        int status;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            snprintf((char*)shm->load_error, sizeof(shm->load_error),
                     "DSP process exited unexpectedly (status=%d)", status);
            shm->initialized = 1; shm->loading_complete = 1;
            inst->child_pid = 0;
            vlog("fork_and_wait: child died during boot, status=%d", status);
            return -1;
        }
        usleep(100000);
    }

    if (!shm->child_ready) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error), "DSP boot timed out (60s)");
        shm->initialized = 1; shm->loading_complete = 1;
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        inst->child_pid = 0;
        return -1;
    }

    vlog("fork_and_wait: child ready");
    return 0;
}

/* Kill existing child and reset shm state for a fresh boot.
 * Preserves: module_dir, rom_index, dsp_clock_percent, gain_percent */
static void kill_child_and_reset(virus_instance_t *inst) {
    virus_shm_t *shm = inst->shm;
    if (inst->child_pid > 0) {
        shm->child_shutdown = 1;
        for (int i = 0; i < 30; i++) {
            int status;
            if (waitpid(inst->child_pid, &status, WNOHANG) == inst->child_pid) break;
            usleep(100000);
        }
        kill(inst->child_pid, SIGKILL);
        waitpid(inst->child_pid, nullptr, 0);
        inst->child_pid = 0;
    }

    /* Reset transient state but keep config */
    shm->ring_read = 0;
    shm->ring_write = 0;
    shm->midi_read = 0;
    shm->midi_write = 0;
    shm->child_ready = 0;
    shm->child_shutdown = 0;
    shm->child_alive = 0;
    shm->initialized = 0;
    shm->loading_complete = 0;
    shm->load_error[0] = '\0';
    shm->underrun_count = 0;
    shm->emu_blocks = 0;
    shm->render_count = 0;
    shm->prof_process_us_total = 0;
    shm->prof_process_max_us = 0;
    shm->prof_peak_level = 0.0f;
    shm->prof_ring_min = 0;
    shm->dsp_clock_applied = 0;
}

static void* boot_thread_func(void *arg) {
    virus_instance_t *inst = (virus_instance_t*)arg;
    virus_shm_t *shm = inst->shm;

    vlog("boot thread: waiting 3s for system to stabilize...");
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Waiting...");
    for (int i = 0; i < 3; i++) sleep(1);

    fork_and_wait_child(inst);

    vlog("boot thread: boot complete");
    inst->boot_thread_running = 0;
    return nullptr;
}

/* Restart thread — kills existing child and boots a new one with different ROM */
static void* restart_thread_func(void *arg) {
    virus_instance_t *inst = (virus_instance_t*)arg;
    virus_shm_t *shm = inst->shm;

    vlog("restart thread: killing child for ROM switch...");
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Switching ROM...");

    kill_child_and_reset(inst);

    vlog("restart thread: waiting 1s...");
    sleep(1);

    fork_and_wait_child(inst);

    vlog("restart thread: restart complete");
    inst->boot_thread_running = 0;
    return nullptr;
}

/* =====================================================================
 * Plugin API v2
 * ===================================================================== */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    virus_instance_t *inst = (virus_instance_t*)calloc(1, sizeof(virus_instance_t));
    if (!inst) return nullptr;

    /* Allocate shared memory (MAP_SHARED so it persists across fork) */
    inst->shm = (virus_shm_t*)mmap(nullptr, sizeof(virus_shm_t),
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (inst->shm == MAP_FAILED) {
        fprintf(stderr, "Virus: mmap failed: %s\n", strerror(errno));
        free(inst);
        return nullptr;
    }
    memset(inst->shm, 0, sizeof(virus_shm_t));
    strncpy((char*)inst->shm->module_dir, module_dir, sizeof(inst->shm->module_dir) - 1);

    /* Set default CC values (Virus Page A indices) */
    inst->shm->cc_values[40] = 127;  /* Cutoff */
    inst->shm->cc_values[42] = 0;    /* Resonance */
    inst->shm->cc_values[91] = 100;  /* Patch Volume */

    fprintf(stderr, "Virus: creating instance from %s (fork mode)\n", module_dir);
    snprintf((char*)inst->shm->loading_status, sizeof(inst->shm->loading_status), "Initializing...");

    inst->boot_thread_running = 1;
    pthread_create(&inst->boot_thread, nullptr, boot_thread_func, inst);
    return inst;
}

static void v2_destroy_instance(void *instance) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst) return;
    fprintf(stderr, "Virus: destroying\n");

    /* Signal child to shutdown */
    if (inst->shm) inst->shm->child_shutdown = 1;

    /* Wait for boot thread */
    if (inst->boot_thread_running)
        pthread_join(inst->boot_thread, nullptr);

    /* Kill child process */
    if (inst->child_pid > 0) {
        kill(inst->child_pid, SIGTERM);
        /* Wait up to 3 seconds */
        for (int i = 0; i < 30; i++) {
            int status;
            if (waitpid(inst->child_pid, &status, WNOHANG) == inst->child_pid) break;
            usleep(100000);
        }
        kill(inst->child_pid, SIGKILL);
        waitpid(inst->child_pid, nullptr, 0);
    }

    /* Unmap shared memory */
    if (inst->shm && inst->shm != MAP_FAILED)
        munmap(inst->shm, sizeof(virus_shm_t));

    if (inst->pending_state) free(inst->pending_state);
    free(inst);
    fprintf(stderr, "Virus: destroyed\n");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst || !inst->shm || !inst->shm->initialized || len < 1) return;
    (void)source;

    uint8_t modified[8];
    int n = len > 8 ? 8 : len;
    memcpy(modified, msg, n);

    uint8_t status = msg[0] & 0xF0;

    /* Log non-note MIDI */
    if (status != 0x90 && status != 0x80 && len >= 2)
        vlog("[midi-in] raw=0x%02X status=0x%02X d1=%d d2=%d len=%d",
             msg[0], status, msg[1], len > 2 ? msg[2] : 0, len);

    /* Convert polypressure (0xA0) to channel pressure (0xD0).
     * The Virus uses polypressure exclusively for Page B parameter access
     * (see manual: "KCB: accessible by MIDI Polyphonic Pressure").
     * The mod matrix "ChanPres" source only responds to channel pressure.
     * Since channel pressure is mono, we track per-note pressure and
     * send the max across all active notes. */
    if (status == 0xA0 && len >= 3) {
        uint8_t note = msg[1] & 0x7F;
        inst->note_pressure[note] = msg[2] & 0x7F;
        /* Find max pressure across all notes */
        uint8_t max_p = 0;
        for (int i = 0; i < 128; i++) {
            if (inst->note_pressure[i] > max_p)
                max_p = inst->note_pressure[i];
        }
        uint8_t ch = msg[0] & 0x0F;
        uint8_t cp[2] = { (uint8_t)(0xD0 | ch), max_p };
        midi_fifo_push(inst->shm, cp, 2);
        return;
    }

    /* Clear note pressure on note-off */
    if (status == 0x80 || (status == 0x90 && len >= 3 && msg[2] == 0)) {
        uint8_t note = msg[1] & 0x7F;
        inst->note_pressure[note] = 0;
    }

    /* Apply octave transpose to notes */
    if ((status == 0x90 || status == 0x80) && len >= 2) {
        int note = msg[1] + inst->shm->octave_transpose * 12;
        if (note < 0) note = 0; if (note > 127) note = 127;
        modified[1] = (uint8_t)note;
    }

    /* Track CC values locally too */
    if (status == 0xB0 && len >= 3) {
        inst->shm->cc_values[msg[1] & 0x7F] = msg[2] & 0x7F;
        inst->shm->cc_seen[msg[1] & 0x7F] = 1;
    }

    if (status == 0xC0 && len >= 2) {
        clear_param_overrides(inst->shm);
    } else if (status == 0xB0 && len >= 3 && (msg[1] == 0 || msg[1] == 32)) {
        clear_param_overrides(inst->shm);
    }

    /* Push to shared MIDI FIFO for child to consume */
    midi_fifo_push(inst->shm, modified, n);
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = atoi(pos);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst || !inst->shm) return;
    virus_shm_t *shm = inst->shm;

    if (strcmp(key, "state") == 0) {
        if (!shm->loading_complete || !shm->child_ready) {
            if (inst->pending_state) free(inst->pending_state);
            inst->pending_state = strdup(val);
            inst->pending_state_valid = 1;
            return;
        }
        int ival;
        int state_version = 1;
        if (json_get_int(val, "state_version", &ival) == 0)
            state_version = ival;

        /* Check ROM first: if it's changing, the child will restart which
         * resets the MIDI FIFO — so bank/preset/CC MIDI would be lost.
         * Instead, save the full state as pending and let it re-apply
         * after the new child boots with the correct ROM. */
        if (json_get_int(val, "rom_index", &ival) == 0) {
            if (ival >= 0 && (shm->rom_count == 0 || ival < shm->rom_count) && ival != shm->rom_index) {
                shm->rom_index = ival;
                if (inst->pending_state) free(inst->pending_state);
                inst->pending_state = strdup(val);
                inst->pending_state_valid = 1;
                if (shm->child_ready && !inst->boot_thread_running) {
                    inst->boot_thread_running = 1;
                    pthread_t t;
                    pthread_create(&t, nullptr, restart_thread_func, inst);
                    pthread_detach(t);
                }
                return;
            }
        }

        bool has_preset = false;
        int preset_from_state = 0;
        if (json_get_int(val, "bank", &ival) == 0 && ival >= 0 && ival < shm->bank_count) {
            shm->current_bank = ival;
            snprintf((char*)shm->bank_name, sizeof(shm->bank_name), "Bank %c", 'A' + ival);
        }
        if (json_get_int(val, "preset", &ival) == 0 && ival >= 0 && ival < shm->preset_count) {
            has_preset = true;
            preset_from_state = ival;
        }
        if (has_preset) {
            clear_param_overrides(shm);
            shm->current_preset = preset_from_state;
            uint8_t cc32[3] = { 0xB0, 32, bank_index_to_midi_lsb(shm->current_bank, shm->bank_count) };
            midi_fifo_push(shm, cc32, 3);
            uint8_t pc[2] = { 0xC0, (uint8_t)shm->current_preset };
            midi_fifo_push(shm, pc, 2);
        }
        if (json_get_int(val, "octave_transpose", &ival) == 0) {
            if (ival < -4) ival = -4; if (ival > 4) ival = 4;
            shm->octave_transpose = ival;
        }
        if (json_get_int(val, "dsp_clock", &ival) == 0) {
            if (ival < 10) ival = 10; if (ival > 100) ival = 100;
            shm->dsp_clock_percent = ival;
        }
        if (json_get_int(val, "gain", &ival) == 0) {
            if (ival < 1) ival = 1; if (ival > 100) ival = 100;
            shm->gain_percent = ival;
        }
        if (state_version >= VIRUS_STATE_VERSION) {
            for (int i = 0; i < NUM_PARAMS; i++) {
                if (json_get_int(val, g_params[i].key, &ival) == 0) {
                    if (ival < g_params[i].min_val) ival = g_params[i].min_val;
                    if (ival > g_params[i].max_val) ival = g_params[i].max_val;
                    send_param_midi(shm, &g_params[i], ival);
                }
            }
        } else {
            vlog("[parent] legacy state detected; skipping per-parameter restore");
        }
        shm_refresh_current_preset_name(shm);
        return;
    }
    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < shm->preset_count) {
            if (idx == shm->current_preset) {
                shm_refresh_current_preset_name(shm);
                return;
            }
            clear_param_overrides(shm);
            shm->current_preset = idx;
            uint8_t pc[2] = { 0xC0, (uint8_t)idx };
            midi_fifo_push(shm, pc, 2);
            shm_refresh_current_preset_name(shm);
        }
        return;
    }
    if (strcmp(key, "bank_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < shm->bank_count) {
            if (idx == shm->current_bank) {
                shm_refresh_current_preset_name(shm);
                return;
            }
            clear_param_overrides(shm);
            shm->current_bank = idx;
            shm->current_preset = 0;
            snprintf((char*)shm->bank_name, sizeof(shm->bank_name), "Bank %c", 'A' + idx);
            uint8_t cc32[3] = { 0xB0, 32, bank_index_to_midi_lsb(idx, shm->bank_count) };
            midi_fifo_push(shm, cc32, 3);
            uint8_t pc[2] = { 0xC0, 0 };
            midi_fifo_push(shm, pc, 2);
            shm_refresh_current_preset_name(shm);
        }
        return;
    }
    if (strcmp(key, "octave_transpose") == 0) {
        int v = atoi(val);
        if (v < -4) v = -4; if (v > 4) v = 4;
        shm->octave_transpose = v;
        return;
    }
    if (strcmp(key, "rom_index") == 0) {
        /* Accept either a numeric index or a ROM name string */
        int v = -1;
        for (int i = 0; i < shm->rom_count; i++) {
            if (strcmp(val, shm->rom_names[i]) == 0) { v = i; break; }
        }
        if (v < 0) v = atoi(val);  /* fall back to numeric */
        if (v < 0) v = 0;
        if (shm->rom_count > 0 && v >= shm->rom_count) v = shm->rom_count - 1;
        if (v == shm->rom_index && shm->child_ready) return; /* no change */
        shm->rom_index = v;
        /* Reset clock to auto so new ROM gets its appropriate default */
        shm->dsp_clock_percent = 0;
        /* Restart child with new ROM (in background thread) */
        if (!inst->boot_thread_running) {
            inst->boot_thread_running = 1;
            pthread_t t;
            pthread_create(&t, nullptr, restart_thread_func, inst);
            pthread_detach(t);
        }
        return;
    }
    if (strcmp(key, "dsp_clock") == 0) {
        int v = atoi(val);
        if (v < 10) v = 10; if (v > 100) v = 100;
        shm->dsp_clock_percent = v;
        return;
    }
    if (strcmp(key, "gain") == 0) {
        int v = atoi(val);
        if (v < 1) v = 1; if (v > 100) v = 100;
        shm->gain_percent = v;
        return;
    }
    if (strcmp(key, "all_notes_off") == 0) {
        uint8_t msg[3] = { 0xB0, 123, 0 };
        midi_fifo_push(shm, msg, 3);
        return;
    }
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(key, g_params[i].key) == 0) {
            int ival = atoi(val);
            if (ival < g_params[i].min_val) ival = g_params[i].min_val;
            if (ival > g_params[i].max_val) ival = g_params[i].max_val;
            send_param_midi(shm, &g_params[i], ival);
            return;
        }
    }
}

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
    H_PARAM("input_mode", "Input Mode");
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

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst || !inst->shm) return -1;
    virus_shm_t *shm = inst->shm;

    if (strcmp(key, "preset") == 0) return snprintf(buf, buf_len, "%d", shm->current_preset);
    if (strcmp(key, "preset_count") == 0) return snprintf(buf, buf_len, "%d", shm->preset_count);
    if (strcmp(key, "preset_name") == 0) {
        shm_refresh_current_preset_name(shm);
        return snprintf(buf, buf_len, "%s", (const char*)shm->preset_name);
    }
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Virus");
    if (strcmp(key, "bank_index") == 0) return snprintf(buf, buf_len, "%d", shm->current_bank);
    if (strcmp(key, "bank_count") == 0) return snprintf(buf, buf_len, "%d", shm->bank_count);
    if (strcmp(key, "bank_name") == 0) return snprintf(buf, buf_len, "%s", (const char*)shm->bank_name);
    if (strcmp(key, "patch_in_bank") == 0) return snprintf(buf, buf_len, "%d", shm->current_preset + 1);
    if (strcmp(key, "octave_transpose") == 0) return snprintf(buf, buf_len, "%d", shm->octave_transpose);
    if (strcmp(key, "dsp_clock") == 0) return snprintf(buf, buf_len, "%d", shm->dsp_clock_applied > 0 ? shm->dsp_clock_applied : (shm->dsp_clock_percent > 0 ? shm->dsp_clock_percent : 40));
    if (strcmp(key, "rom_model") == 0) return snprintf(buf, buf_len, "%s", shm->rom_model_name[0] ? (const char*)shm->rom_model_name : "?");
    if (strcmp(key, "rom_index") == 0) {
        int idx = shm->rom_index;
        if (idx >= 0 && idx < shm->rom_count && shm->rom_names[idx][0])
            return snprintf(buf, buf_len, "%s", shm->rom_names[idx]);
        return snprintf(buf, buf_len, "%d", idx);
    }
    if (strcmp(key, "rom_count") == 0) return snprintf(buf, buf_len, "%d", shm->rom_count);
    if (strcmp(key, "rom_name") == 0) {
        int idx = shm->rom_index;
        if (idx >= 0 && idx < shm->rom_count && shm->rom_names[idx][0])
            return snprintf(buf, buf_len, "%s", shm->rom_names[idx]);
        return snprintf(buf, buf_len, "---");
    }
    if (strcmp(key, "rom_list") == 0) {
        int off = 0;
        for (int i = 0; i < shm->rom_count && off < buf_len - 80; i++) {
            if (i > 0) off += snprintf(buf+off, buf_len-off, ",");
            off += snprintf(buf+off, buf_len-off, "%s", shm->rom_names[i]);
        }
        return off;
    }
    if (strcmp(key, "gain") == 0) return snprintf(buf, buf_len, "%d", shm->gain_percent > 0 ? shm->gain_percent : 70);
    if (strcmp(key, "loading_status") == 0) return snprintf(buf, buf_len, "%s", (const char*)shm->loading_status);
    if (strcmp(key, "debug_info") == 0) {
        int avail = shm_ring_available(shm);
        int blocks = shm->emu_blocks > 0 ? shm->emu_blocks : 1;
        return snprintf(buf, buf_len,
            "buf=%d min=%d ur=%d blk=%d proc_avg=%lld proc_max=%d peak=%.2f pid=%d",
            avail, shm->prof_ring_min, shm->underrun_count, shm->emu_blocks,
            (long long)(shm->prof_process_us_total / blocks),
            shm->prof_process_max_us,
            (double)shm->prof_peak_level,
            (int)inst->child_pid);
    }
    if (strcmp(key, "prof_reset") == 0) {
        shm->prof_process_us_total = 0;
        shm->prof_ring_min = AUDIO_RING_SIZE;
        shm->prof_process_max_us = 0;
        shm->prof_peak_level = 0.0f;
        shm->underrun_count = 0;
        shm->emu_blocks = 0;
        return snprintf(buf, buf_len, "reset");
    }
    for (int i = 0; i < NUM_PARAMS; i++)
        if (strcmp(key, g_params[i].key) == 0)
            return snprintf(buf, buf_len, "%d", get_param_value(shm, &g_params[i]));

    if (strcmp(key, "state") == 0) {
        int off = 0;
        off += snprintf(buf+off, buf_len-off, "{\"state_version\":%d,\"bank\":%d,\"preset\":%d,\"octave_transpose\":%d",
            VIRUS_STATE_VERSION, shm->current_bank, shm->current_preset, shm->octave_transpose);
        off += snprintf(buf+off, buf_len-off, ",\"dsp_clock\":%d,\"gain\":%d,\"rom_index\":%d",
            shm->dsp_clock_applied > 0 ? shm->dsp_clock_applied : 40,
            shm->gain_percent > 0 ? shm->gain_percent : 70, shm->rom_index);
        for (int i = 0; i < NUM_PARAMS; i++) {
            if (!is_param_seen(shm, &g_params[i])) continue;
            off += snprintf(buf+off, buf_len-off, ",\"%s\":%d",
                g_params[i].key, get_param_value(shm, &g_params[i]));
        }
        off += snprintf(buf+off, buf_len-off, "}");
        return off;
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        return build_ui_hierarchy(buf, buf_len, (const char*)shm->rom_model_name);
    }
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
    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst || !inst->shm || inst->shm->load_error[0] == '\0') return 0;
    return snprintf(buf, buf_len, "%s", (const char*)inst->shm->load_error);
}

static void v2_render_block(void *instance, int16_t *out, int frames) {
    virus_instance_t *inst = (virus_instance_t*)instance;
    if (!inst || !inst->shm || !inst->shm->loading_complete || !inst->shm->child_ready) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Apply pending state that arrived before child was ready */
    if (inst->pending_state_valid && inst->pending_state) {
        inst->pending_state_valid = 0;
        char *state = inst->pending_state;
        inst->pending_state = nullptr;
        vlog("[parent] applying pending state after boot");
        v2_set_param(instance, "state", state);
        free(state);
    }

    virus_shm_t *shm = inst->shm;

    int avail = shm_ring_available(shm);
    if (shm->prof_ring_min == 0 || avail < shm->prof_ring_min)
        shm->prof_ring_min = avail;
    int to_read = (avail < frames) ? avail : frames;
    int rd = shm->ring_read;
    for (int i = 0; i < to_read; i++) {
        out[i*2+0] = shm->audio_ring[rd*2+0];
        out[i*2+1] = shm->audio_ring[rd*2+1];
        rd = (rd + 1) % AUDIO_RING_SIZE;
    }
    shm->ring_read = rd;

    if (to_read < frames) {
        shm->underrun_count++;
        memset(out + to_read * 2, 0, (frames - to_read) * 2 * sizeof(int16_t));
    }
    shm->render_count++;
}

/* =====================================================================
 * Entry point
 * ===================================================================== */

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;
    return &g_plugin_api_v2;
}
