// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Fullban (Christopher Fuller)
//
// Fullban M-One: the PicoFaceM1 engine (Michi71, GPL-3.0, vendored in src/m1) as an MPC OS VST2 instrument,
// through the open-mpc wrapper's mpc_engine() contract (wrapper/engine.h): 44.1 kHz int16 stereo.
//
// The engine runs at the M1's own 31 250 Hz; a 16-tap windowed-sinc resampler takes it to 44 100 Hz.
// The ROM tables and PCM are read at start-up from the user's own M1 ROM set in the plugin's Content/ROMs folder
// (m1_romload.cpp), so the plugin itself holds nothing from the ROMs. Without them it stays silent and says so.
//
// Each instance keeps its own writable copy of the 100 programs and 100 combinations. The engine reads program
// bytes live, so the eight "quick edit" macros (cutoff, EG, attack ... FX amount) are offsets applied to that
// copy: 0 = the sound as stored, and they work in Program and Combination mode alike.

extern "C" {
#include "engine.h"
}
#include "m1_engine/m1_engine.h"
#include "m1_romload.h"
#include "params.h"                                  // PARAMS (generated): ranges / options for the Q-Link proxies

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <strings.h>


namespace {

// ---------------------------------------------------------------- resampler 31 250 -> 44 100 Hz
constexpr int kTaps = 16, kHalf = 8, kPhases = 512;
constexpr int kRing = 1024;                       // power of two
constexpr double kStep = 31250.0 / 44100.0;       // input samples per output sample

float g_tab[kPhases + 1][kTaps];
bool g_tabReady = false;

double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

void build_table() {
    if (g_tabReady) return;
    const double fc = 0.47, beta = 7.5, i0b = bessel_i0(beta);   // cutoff 14.7 kHz, below the M1's 15.6 kHz Nyquist
    for (int p = 0; p <= kPhases; ++p) {
        const double frac = (double)p / kPhases;
        double sum = 0, h[kTaps];
        for (int j = 0; j < kTaps; ++j) {
            const double d = frac + (kHalf - 1) - j;            // distance from the output point to tap j
            const double x = 2 * fc * d;
            const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            const double r = d / kHalf;
            const double w = fabs(r) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - r * r)) / i0b;
            h[j] = 2 * fc * sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < kTaps; ++j) g_tab[p][j] = (float)(h[j] / sum);
    }
    g_tabReady = true;
}

// ---------------------------------------------------------------- parameters
enum Macro { kCutoff, kEgInt, kAttack, kDecay, kSustain, kRelease, kVibrato, kFxAmt, kMacros };
const char* const kMacroKeys[kMacros] = {"cutoff", "eg_int", "attack", "decay", "sustain", "release", "vibrato", "fx_amt"};

constexpr int kOscBase[2] = {63, 103};          // OSC 1 / OSC 2 parameter blocks inside a 143-byte program

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- Library (1.2.0)
// The owner's own M1 SysEx banks, converted to .m1b files (tools/syx2m1b.py) in the plugin folder's
// Content/Banks/<author>/<bank>.m1b: "M1B1", flags (bit 0 combinations, bit 1 drum kits), 3 reserved bytes,
// 100 x 143 program bytes, [100 x 124 combination bytes], [4 x 210 drum kit bytes]. Scanned once per process.
struct LibBank { char name[48]; char path[640]; };
struct LibAuthor { char name[48]; int first, count; };
constexpr int kLibMaxAuthors = 64, kLibMaxBanks = 1024;
LibAuthor g_auth[kLibMaxAuthors];
LibBank* g_bank = nullptr;
int g_nAuth = 0, g_nBank = 0;
pthread_once_t g_libOnce = PTHREAD_ONCE_INIT;

void lib_anchor() {}
int ci_cmp(const void* a, const void* b) { return strcasecmp(*(const char* const*)a, *(const char* const*)b); }
void lib_scan() {
    char dir[512];
    bool found = false;
    if (const char* env = getenv("MPC_PLUGIN_FOLDER")) {   // tests off the device (the SDK's own override)
        snprintf(dir, sizeof dir, "%s", env); found = true;
    } else {
        Dl_info di;
        if (!dladdr((void*)&lib_anchor, &di) || !di.dli_fname) return;
        snprintf(dir, sizeof dir, "%s", di.dli_fname);
    }
    for (int up = 0; up < 3 && !found; ++up) {         // the .so sits in the plugin folder or in its VST/
        char* sl = strrchr(dir, '/');
        if (!sl) break;
        *sl = 0;
        char t[600];
        snprintf(t, sizeof t, "%s/plugin.json", dir);
        found = access(t, F_OK) == 0;
    }
    if (!found) return;
    char root[600];
    snprintf(root, sizeof root, "%s/Content/Banks", dir);
    DIR* d = opendir(root);
    if (!d) return;
    static char names[kLibMaxAuthors][48];
    const char* order[kLibMaxAuthors];
    int na = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || na >= kLibMaxAuthors) continue;
        char p[700]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(names[na], 48, "%s", e->d_name); order[na] = names[na]; ++na; }
    }
    closedir(d);
    qsort(order, na, sizeof order[0], ci_cmp);
    g_bank = static_cast<LibBank*>(calloc(kLibMaxBanks, sizeof(LibBank)));
    if (!g_bank) return;
    for (int a = 0; a < na; ++a) {
        char ad[700];
        snprintf(ad, sizeof ad, "%s/%s", root, order[a]);
        DIR* bd = opendir(ad);
        if (!bd) continue;
        static char bn[256][48];
        const char* bo[256];
        int nb = 0;
        while (dirent* e = readdir(bd)) {
            const size_t l = strlen(e->d_name);
            if (e->d_name[0] == '.' || l < 5 || strcasecmp(e->d_name + l - 4, ".m1b") || nb >= 256) continue;
            snprintf(bn[nb], 48, "%.*s", (int)(l - 4 < 47 ? l - 4 : 47), e->d_name); bo[nb] = bn[nb]; ++nb;
        }
        closedir(bd);
        if (!nb) continue;
        qsort(bo, nb, sizeof bo[0], ci_cmp);
        LibAuthor& A = g_auth[g_nAuth];
        snprintf(A.name, sizeof A.name, "%s", order[a]);
        A.first = g_nBank; A.count = 0;
        for (int b = 0; b < nb && g_nBank < kLibMaxBanks; ++b, ++A.count, ++g_nBank) {
            snprintf(g_bank[g_nBank].name, sizeof g_bank[0].name, "%s", bo[b]);
            snprintf(g_bank[g_nBank].path, sizeof g_bank[0].path, "%s/%s.m1b", ad, bo[b]);
        }
        if (++g_nAuth >= kLibMaxAuthors) break;
    }
}
void lib_init() { pthread_once(&g_libOnce, lib_scan); }
#ifdef MONE_TRACE_FILE
// Test builds only: a log of every value MPC sets (wrapper WRAP_TRACE) and what the Library does with it.
FILE* g_trace = nullptr;
void trace_open() {
    if (g_trace) return;
    char dir[512]; Dl_info di;
    if (!dladdr((void*)&lib_anchor, &di) || !di.dli_fname) return;
    snprintf(dir, sizeof dir, "%s", di.dli_fname);
    for (int up = 0; up < 2; ++up) { char* sl = strrchr(dir, '/'); if (sl) *sl = 0; char t[600]; snprintf(t, sizeof t, "%s/plugin.json", dir); if (!access(t, F_OK)) break; }
    char p[600]; snprintf(p, sizeof p, "%s/mone-trace.txt", dir);
    g_trace = fopen(p, "a");
}
#define MONE_TRACE(...) do { trace_open(); if (g_trace) { struct timespec t_; clock_gettime(CLOCK_MONOTONIC, &t_); \
    fprintf(g_trace, "%ld.%03ld ", (long)t_.tv_sec, t_.tv_nsec / 1000000); fprintf(g_trace, __VA_ARGS__); fputc('\n', g_trace); fflush(g_trace); } } while (0)
extern "C" void wrap_trace(int kind, int i, float n) { if (kind == 0) MONE_TRACE("host set p%d %.5f", i, n); else if (kind == 2) MONE_TRACE("UPDATE DISPLAY"); else if (kind == 3) MONE_TRACE("  report p%d %.4f", i, n); }
#else
#define MONE_TRACE(...) do {} while (0)
#endif
double now_s() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---------------------------------------------------------------- the user's ROMs (Content/ROMs)
m1load::Rom* g_rom = nullptr;
char g_romErr[160] = "not loaded";
pthread_once_t g_romOnce = PTHREAD_ONCE_INIT;
void rom_scan() {
    char dir[512];
    bool found = false;
    if (const char* env = getenv("MPC_PLUGIN_FOLDER")) { snprintf(dir, sizeof dir, "%s", env); found = true; }
    else {
        Dl_info di;
        if (!dladdr((void*)&lib_anchor, &di) || !di.dli_fname) { snprintf(g_romErr, sizeof g_romErr, "plugin folder not found"); return; }
        snprintf(dir, sizeof dir, "%s", di.dli_fname);
    }
    for (int up = 0; up < 3 && !found; ++up) {
        char* sl = strrchr(dir, '/');
        if (!sl) break;
        *sl = 0;
        char t[600];
        snprintf(t, sizeof t, "%s/plugin.json", dir);
        found = access(t, F_OK) == 0;
    }
    if (!found) { snprintf(g_romErr, sizeof g_romErr, "plugin folder not found"); return; }
    char roms[600];
    snprintf(roms, sizeof roms, "%s/Content/ROMs", dir);
    g_rom = m1load::load(roms, g_romErr, sizeof g_romErr);
    if (!g_rom) fprintf(stderr, "M-One: no ROMs (%s): %s\n", roms, g_romErr);
#ifdef MONE_FAKEROM
    // Test builds only (virtual MPC): with MONE_FAKEROM=1 in the environment and no ROMs, run the full engine on
    // blank sound data, so the Library / program-load path can be tested without anyone's ROMs. Silent.
    if (!g_rom && getenv("MONE_FAKEROM")) {
        m1load::Rom* r = (m1load::Rom*)calloc(1, sizeof(m1load::Rom));
        if (r) { r->pcmWords = 1u << 21; r->pcm = (int16_t*)calloc(r->pcmWords + 64, sizeof(int16_t));
                 r->nSplits = 1; r->nDrums = 1; for (int i = 0; i < 256; ++i) r->velCurve[i] = (uint16_t)(i * 256);
                 if (r->pcm) { g_rom = r; fprintf(stderr, "M-One: MONE_FAKEROM blank sound data\n"); } else free(r); }
    }
#endif
}
void rom_init() { pthread_once(&g_romOnce, rom_scan); }
const uint8_t g_zero[m1::kPrograms * m1::kProgLen] = {0};   // stands in for the factory sounds without ROMs
const uint8_t* fac_progs() { return g_rom ? g_rom->programs : g_zero; }
const uint8_t* fac_combis() { return g_rom ? g_rom->combis : g_zero; }
const uint8_t* fac_kits() { return g_rom ? g_rom->kits : g_zero; }

// ---------------------------------------------------------------- FX page (1.1.0)
// The M1's 33 effect types, grouped for the FX page; 33 = no effect. Names follow the engine (src/m1/src/m1_fx.cpp).
const char* const kFxName[34] = {
    "Hall", "Ensemble Hall", "Concert Hall", "Room", "Large Room", "Live Stage",
    "Early Ref 1", "Early Ref 2", "Early Ref 3", "Stereo Delay", "Cross Delay",
    "Chorus 1", "Chorus 2", "Flanger 1", "Cross Flanger", "Stereo Phaser", "Phaser",
    "Stereo Tremolo", "Tremolo", "Equalizer", "Overdrive", "Distortion", "Exciter",
    "Symphonic Ens", "Rotary Speaker", "Delay/Hall", "Delay/Room", "Delay/Early Ref",
    "Delay/Delay", "Delay/Chorus", "Delay/Flanger", "Delay/Phaser", "Delay/Tremolo", "No Effect"};
enum FxCat { kCatReverb, kCatEarly, kCatDelay, kCatMod, kCatDrive, kCatEq, kCatDual, kCats };
const char* const kCatName[kCats] = {"Reverb", "Early", "Delay", "Mod", "Drive", "EQ", "Dual"};
const uint8_t kFxCat[34] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 5, 4, 4, 4, 3, 3,
                            6, 6, 6, 6, 6, 6, 6, 6, 0};

// One FX-page knob: which byte of the effect's 8 it edits, and how. The page knob runs 0..100.
enum Kind : uint8_t { kNone, kU8, kS8, kU16, kT10, kFLo, kFHi, kSw };
struct Kn { const char* name; Kind kind; uint8_t byte; int16_t lo, hi; const char* unit; };
#define K_(n, k, b, lo, hi, u) {n, k, b, lo, hi, u}
#define NONE_ {"", kNone, 0, 0, 0, ""}
#define EQ_ K_("EQ High", kS8, 6, -12, 12, "dB"), K_("EQ Low", kS8, 7, -12, 12, "dB")
#define DLY_ K_("Delay", kU16, 0, 0, 500, "ms"), K_("Dly Fdbk", kS8, 2, -99, 99, "")
const Kn kKn[33][6] = {
#define REV_ {K_("Time", kU8, 0, 0, 97, ""), K_("Pre-delay", kU8, 3, 0, 200, "ms"), K_("Hi Damp", kU8, 2, 0, 99, ""), K_("Early", kU8, 4, 0, 99, ""), EQ_}
    REV_, REV_, REV_, REV_, REV_, REV_,
#define ER_ {K_("Time", kT10, 0, 0, 70, ""), K_("Pre-delay", kU8, 1, 0, 200, "ms"), EQ_, NONE_, NONE_}
    ER_, ER_, ER_,
#define DL_ {K_("Time L", kU16, 0, 0, 500, "ms"), K_("Time R", kU16, 4, 0, 500, "ms"), K_("Feedback", kS8, 2, -99, 99, ""), K_("Hi Damp", kU8, 3, 0, 99, ""), EQ_}
    DL_, DL_,
#define CH_ {K_("Depth", kU8, 0, 0, 99, ""), K_("Speed", kU8, 1, 0, 216, ""), K_("Wave", kU8, 2, 0, 3, ""), K_("Delay", kU8, 4, 0, 200, "ms"), EQ_}
    CH_, CH_,
#define FL_ {K_("Depth", kU8, 0, 0, 99, ""), K_("Speed", kU8, 1, 0, 216, ""), K_("Feedback", kS8, 3, -99, 99, ""), K_("Delay", kU8, 4, 0, 50, "ms"), EQ_}
    FL_, FL_,
#define PH_ {K_("Depth", kU8, 0, 0, 99, ""), K_("Speed", kU8, 1, 0, 216, ""), K_("Feedback", kS8, 3, -99, 99, ""), K_("Manual", kU8, 4, 0, 99, ""), K_("Wave", kU8, 2, 0, 3, ""), NONE_}
    PH_, PH_,
#define TR_ {K_("Depth", kU8, 0, 0, 99, ""), K_("Speed", kU8, 1, 0, 216, ""), K_("Wave", kU8, 2, 0, 3, ""), K_("Shape", kS8, 3, -99, 99, ""), EQ_}
    TR_, TR_,
    {K_("High Freq", kFHi, 4, 0, 2, ""), K_("Low Freq", kFLo, 5, 0, 2, ""), K_("High Gain", kS8, 6, -12, 12, "dB"), K_("Low Gain", kS8, 7, -12, 12, "dB"), NONE_, NONE_},
    {K_("Drive", kU8, 2, 0, 99, ""), K_("Level", kU8, 3, 0, 99, ""), EQ_, NONE_, NONE_},
    {K_("Drive", kU8, 2, 0, 99, ""), K_("Level", kU8, 3, 0, 99, ""), K_("EQ Low", kS8, 7, -12, 12, "dB"), NONE_, NONE_, NONE_},
    {K_("Blend", kS8, 0, -99, 99, ""), K_("Emphasis", kU8, 1, 0, 9, ""), EQ_, NONE_, NONE_},
    {K_("Depth", kU8, 0, 0, 99, ""), EQ_, NONE_, NONE_, NONE_},
    {K_("Depth", kU8, 0, 0, 99, ""), K_("Speed", kS8, 2, -10, 10, ""), K_("Fast", kSw, 7, 0, 1, ""), NONE_, NONE_, NONE_},
    {DLY_, K_("Dly Damp", kU8, 3, 0, 99, ""), K_("Rev Time", kU8, 4, 0, 97, ""), K_("Rev Damp", kU8, 6, 0, 99, ""), K_("Pre-delay", kU8, 7, 0, 150, "ms")},
    {DLY_, K_("Dly Damp", kU8, 3, 0, 99, ""), K_("Rev Time", kU8, 4, 0, 47, ""), K_("Rev Damp", kU8, 6, 0, 99, ""), K_("Pre-delay", kU8, 7, 0, 150, "ms")},
    {DLY_, K_("Dly Damp", kU8, 3, 0, 99, ""), K_("ER Time", kT10, 4, 0, 30, ""), K_("ER Pre-dly", kU8, 5, 0, 100, "ms"), NONE_},
    {K_("Delay 1", kU16, 0, 0, 500, "ms"), K_("Fdbk 1", kS8, 2, -99, 99, ""), K_("Damp 1", kU8, 3, 0, 99, ""),
     K_("Delay 2", kU16, 4, 0, 500, "ms"), K_("Fdbk 2", kS8, 6, -99, 99, ""), K_("Damp 2", kU8, 7, 0, 99, "")},
    {DLY_, K_("Dly Damp", kU8, 3, 0, 99, ""), K_("Depth", kU8, 4, 0, 99, ""), K_("Speed", kU8, 5, 0, 216, ""), K_("Wave", kU8, 6, 0, 3, "")},
    {DLY_, K_("Depth", kU8, 4, 0, 99, ""), K_("Speed", kU8, 5, 0, 216, ""), K_("Feedback", kS8, 7, -99, 99, ""), K_("Dly Damp", kU8, 3, 0, 99, "")},
    {DLY_, K_("Depth", kU8, 4, 0, 99, ""), K_("Speed", kU8, 5, 0, 216, ""), K_("Feedback", kS8, 6, -99, 99, ""), K_("Manual", kU8, 7, 0, 99, "")},
    {DLY_, K_("Dly Damp", kU8, 3, 0, 99, ""), K_("Depth", kU8, 4, 0, 99, ""), K_("Speed", kU8, 5, 0, 216, ""), K_("Shape", kS8, 7, -99, 99, "")},
};
#undef K_
#undef NONE_
#undef EQ_
#undef DLY_
// The keys the engine reports when it moves them itself (a new sound with Preset FX on, Reset, a type change):
// bit 2 + index in MOne::changed.
const char* const kFxKeys[] = {"fx_route", "fx1_on", "fx2_on",
    "fx1_cat", "fx1_type", "fx1_p1", "fx1_p2", "fx1_p3", "fx1_p4", "fx1_p5", "fx1_p6", "fx1_p7", "fx1_p8",
    "fx2_cat", "fx2_type", "fx2_p1", "fx2_p2", "fx2_p3", "fx2_p4", "fx2_p5", "fx2_p6", "fx2_p7", "fx2_p8",
    "rev_amt", "dly_amt"};
constexpr int kFxKeyCount = (int)(sizeof kFxKeys / sizeof kFxKeys[0]);
// MOne::changed: bit 0 program, bit 1 combi, bits 2..24 kFxKeys, then these.
constexpr int kEncTicks = 4;                      // Q-Link events per Library step in a fast turn
constexpr unsigned kChgPage = 1u << 31;          // the browser page (00-19 ...) followed the sound
constexpr unsigned kChgLibSrc = 1u << 27, kChgLibAuthor = 1u << 28, kChgLibBank = 1u << 29, kChgSound = 1u << 30;
static_assert(2 + sizeof kFxKeys / sizeof kFxKeys[0] <= 27, "MOne::changed bits");

struct MOne {
    m1::Engine eng;
    m1::RomTables rom{};
    int16_t fxMem[65536];
    uint8_t kernel[32768];
    bool noRom = false;                           // no ROMs: silent, the display says so
    int hlAuthor = 0, hlBank = 0, hlReport = 0;    // hlReport bits: 1 author outline, 2 bank outline, 4 lib_aidx, 8 lib_bidx    // the outline lit on the author / bank box while it changes
    double hlTime = 0;
    int rawAuthor = 50, rawBank = 50;              // the Library knobs: endless, put back to 50 after every move
    int encAcc[2] = {0, 0}; double encLast[2] = {0, 0};   // their click counting (author, bank)                // the author / bank knobs as MPC has them (0..99, loop round the list)
    bool fxDirty = false; double fxDirtyAt = 0;   // FX values to report (see fx_mark)
    char fxSnap[32][24] = {};                     // what MPC was last told for each kFxKeys entry
    int qSel[2] = {0, 0};                         // Q-Link section: [0] Play (Top / Bottom), [1] FX (FX 1 / FX 2)
    double qLast[3][8] = {};
    float qSent[3][8] = {}, qRead[3][8] = {};      // the proxy value MPC last sent / last read back
    double qSentAt[3][8] = {};
    int pickAcc[3] = {};                          // quick-turn clicks counted towards the next sound
    bool fxLate = false;                          // the sound changed during a pick; its effects come when the knob stops
    double lastPick = 0;                          // when a Q-Link last picked a sound (program / combi reports wait)                         // the proxy value MPC holds (last sent or read back)                      // last proxy move per knob (fast turn -> bigger steps)
    int pend[48]; int nPend = 0;                  // PARAMS indices to report to MPC (proxies, their targets, selectors)
    double bornS = 0;                             // when create() ran (the Library auto-switch waits 3 s)
    uint8_t global[21];
    uint8_t progs[m1::kPrograms * m1::kProgLen];
    uint8_t combis[m1::kCombis * m1::kCombiLen];

    int mode = 0, program = 0, combi = 0, bank = 0;
    int macro[kMacros] = {0};
    float volDb = -4.0f;
    bool fxOn = true;
    unsigned changed = 0;                         // bit 0 program, bit 1 combi, 2.. kFxKeys (mpc_engine_changed)
    uint8_t fxUser[m1::kFxBlockLen] = {33, 33};   // the FX page's effect block (FX AMOUNT is laid over it, fx_apply)
    uint8_t fxDef[33][8] = {{0}};                 // per type: the settings a newly picked type starts from
    bool fxFollow = true;                         // Preset FX: a new sound brings its own effects
    // Library: source (0 Factory, 1 Library), the author and bank picked, the bank loaded (index into g_bank, -1 none)
    int libSrc = 0, libAuthor = 0, libBank = 0, libLoaded = -1;
    uint8_t libFlags = 0;
    uint8_t libProgs[m1::kPrograms * m1::kProgLen], libCombis[m1::kCombis * m1::kCombiLen], libKits[840];
    // The FX page's block last used per mode and for which sound: MPC nudges parameter 0 (mode) on every prepare,
    // which must not throw away the page's edits (select_sound restores them when the sound is the same).
    struct { int idx = -1; uint8_t b[m1::kFxBlockLen]; } fxCache[2];

    float inL[kRing], inR[kRing];
    long long have = 0;                           // input samples produced so far
    double pos = kHalf;                           // input position of the next output sample
    float blkL[m1::kRenderBlock], blkR[m1::kRenderBlock];
};




// ---------------------------------------------------------------- FX page
// Where the sounds come from: the Library bank when one is loaded and picked, else the factory preload. A bank of
// programs only plays the factory combinations (with the factory programs they were made from) in Combi mode.
bool use_lib(const MOne* m) { return m->libSrc == 1 && m->libLoaded >= 0; }
const uint8_t* src_progs(const MOne* m) { return use_lib(m) && !(m->mode && !(m->libFlags & 1)) ? m->libProgs : fac_progs(); }
const uint8_t* src_combis(const MOne* m) { return use_lib(m) && (m->libFlags & 1) ? m->libCombis : fac_combis(); }
const uint8_t* src_kits(const MOne* m) { return use_lib(m) && (m->libFlags & 2) && src_progs(m) == m->libProgs ? m->libKits : fac_kits(); }
int lib_index(const MOne* m) {
    if (!g_nAuth) return -1;
    const LibAuthor& A = g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)];
    return A.first + clampi(m->libBank, 0, A.count - 1);
}
bool lib_load(MOne* m) {                          // the picked bank into libProgs / libCombis / libKits
    const int idx = lib_index(m);
    if (idx < 0) { m->libLoaded = -1; return false; }
    if (idx == m->libLoaded) return true;
    m->libLoaded = -1;
    FILE* f = fopen(g_bank[idx].path, "rb");
    if (!f) return false;
    uint8_t h[8];
    bool ok = fread(h, 1, 8, f) == 8 && !memcmp(h, "M1B1", 4) && fread(m->libProgs, 1, sizeof m->libProgs, f) == sizeof m->libProgs;
    if (ok && (h[4] & 1)) ok = fread(m->libCombis, 1, sizeof m->libCombis, f) == sizeof m->libCombis;
    if (ok && (h[4] & 2)) ok = fread(m->libKits, 1, sizeof m->libKits, f) == sizeof m->libKits;
    fclose(f);
    if (!ok) return false;
    m->libFlags = h[4];
    m->libLoaded = idx;
    return true;
}

const uint8_t* stored_fx(const MOne* m) {         // the current sound's own effect block, as in ROM
    return m->mode ? src_combis(m) + m->combi * m1::kCombiLen + 11 : src_progs(m) + m->program * m1::kProgLen + 38;
}
// The FX page's values changed (a new sound, a reset). Not reported at once: a Q-Link or wheel picking sounds would
// make MPC re-read ~26 values and rebuild its screen (the effect types are option lists) on every click, which made
// the Q-Link screens flicker and the turning knob stall. mpc_engine_changed reports, 0.35 s after the last change,
// only the values that differ from what MPC was last told.
void fx_mark(MOne* m) { m->fxDirtyAt = now_s(); m->fxDirty = true; }
// What the effects run: the FX page's block with the FX AMOUNT offset on the four balances.
void fx_apply(MOne* m, bool rebuild) {
    m->fxCache[m->mode].idx = m->mode ? m->combi : m->program;
    memcpy(m->fxCache[m->mode].b, m->fxUser, sizeof m->fxUser);
    uint8_t e[m1::kFxBlockLen];
    memcpy(e, m->fxUser, sizeof e);
    for (int k = 2; k <= 5; ++k) e[k] = (uint8_t)clampi((int)e[k] + m->macro[kFxAmt] * 2, 0, 100);
    if (rebuild) m->eng.refreshEffects(e); else m->eng.retuneEffects(e);
}
// Defaults for a type: the settings of the first factory program or combination that uses it there.
void fx_defaults(MOne* m) {
    bool have[34] = {false};
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 100; ++i) {
            const uint8_t* b = pass ? fac_combis() + i * m1::kCombiLen + 11 : fac_progs() + i * m1::kProgLen + 38;   // FACTORY
            for (int k = 0; k < 2; ++k)
                if (b[k] < 33 && !have[b[k]]) { have[b[k]] = true; memcpy(m->fxDef[b[k]], b + 9 + 8 * k, 8); }
        }
}
int kn_get(const uint8_t* p, const Kn& d) {        // the effect setting in its own units
    switch (d.kind) {
    case kS8: return (int8_t)p[d.byte];
    case kU16: return p[d.byte] | (p[d.byte + 1] << 8);
    case kNone: return 0;
    default: return p[d.byte];
    }
}
void kn_set(uint8_t* p, const Kn& d, int v) {
    v = clampi(v, d.lo, d.hi);
    if (d.kind == kU16) { p[d.byte] = (uint8_t)(v & 0xFF); p[d.byte + 1] = (uint8_t)(v >> 8); }
    else if (d.kind != kNone) p[d.byte] = (uint8_t)(int8_t)v;
}
int kn_pos(const uint8_t* p, const Kn& d) {        // 0..100 page knob position
    if (d.kind == kNone || d.hi <= d.lo) return 0;
    return (int)lround((kn_get(p, d) - d.lo) * 100.0 / (d.hi - d.lo));
}
void kn_text(const uint8_t* p, const Kn& d, char* buf, int n) {
    const int v = clampi(kn_get(p, d), d.lo, d.hi);
    static const char* const lo[3] = {"250 Hz", "500 Hz", "1 kHz"}, *hi[3] = {"1 kHz", "2 kHz", "4 kHz"};
    switch (d.kind) {
    case kNone: snprintf(buf, n, " "); break;
    case kT10: snprintf(buf, n, "%d", v + 10); break;
    case kFLo: snprintf(buf, n, "%s", lo[v]); break;
    case kFHi: snprintf(buf, n, "%s", hi[v]); break;
    case kSw: snprintf(buf, n, "%s", v ? "On" : "Off"); break;
    case kS8: if (d.unit[0]) snprintf(buf, n, v > 0 ? "+%d %s" : "%d %s", v, d.unit); else snprintf(buf, n, v > 0 ? "+%d" : "%d", v); break;
    default: if (d.unit[0]) snprintf(buf, n, "%d %s", v, d.unit); else snprintf(buf, n, "%d", v); break;
    }
}
int fx_type(const MOne* m, int k) { const int t = m->fxUser[k]; return t < 33 ? t : 33; }
void fx_set_type(MOne* m, int k, int t) {
    t = clampi(t, 0, 32);
    if (t == m->fxUser[k]) return;
    m->fxUser[k] = (uint8_t)t;
    memcpy(m->fxUser + 9 + 8 * k, m->fxDef[t], 8);
    if (!(m->fxUser[8] & (k ? 0x0C : 0x03))) m->fxUser[8] |= k ? 0x0C : 0x03;   // picking a type switches it on
    if (m->fxUser[2 + 2 * k] == 0 && m->fxUser[3 + 2 * k] == 0) m->fxUser[2 + 2 * k] = m->fxUser[3 + 2 * k] = 30;
    fx_mark(m);
}
void fx_step_type(MOne* m, int k, int dir) {       // next / previous type in the same category
    const int t = fx_type(m, k), c = t < 33 ? kFxCat[t] : 0;
    int u = t < 33 ? t : 0;
    for (int i = 0; i < 33; ++i) { u = (u + dir + 33) % 33; if (kFxCat[u] == c) break; }
    fx_set_type(m, k, u);
}
void fx_set_cat(MOne* m, int k, int c) {
    c = clampi(c, 0, kCats - 1);
    const int t = fx_type(m, k);
    if (t < 33 && kFxCat[t] == c) return;
    for (int u = 0; u < 33; ++u) if (kFxCat[u] == c) { fx_set_type(m, k, u); return; }
}
// The Play page's REVERB / DELAY quick knobs: the mix (both balances) of the effect that is a reverb / a delay.
// A dedicated one wins (Reverb or Early group / Delay group), else a Dual type that has one (Delay/Hall, Room,
// Early Ref for reverb; any Dual for delay). -1: this sound has none.
int quick_slot(const MOne* m, bool reverb) {
    int dual = -1;
    for (int k = 0; k < 2; ++k) {
        const int t = fx_type(m, k);
        if (t >= 33) continue;
        const int c = kFxCat[t];
        if (reverb ? (c == kCatReverb || c == kCatEarly) : c == kCatDelay) return k;
        if (c == kCatDual && dual < 0 && (!reverb || t <= 27)) dual = k;
    }
    return dual;
}

// fxN_pK: knobs 1-6 = the type's own settings, 7/8 = its two balances (mix L / R, 0..100).
bool fx_param_set(MOne* m, const char* k, double v) {
    if (!strcmp(k, "rev_amt") || !strcmp(k, "dly_amt")) {
        const int s = quick_slot(m, k[0] == 'r');
        if (s < 0) return true;
        const uint8_t b = (uint8_t)clampi((int)lround(v), 0, 100);
        if (m->fxUser[2 + 2 * s] == b && m->fxUser[3 + 2 * s] == b) return true;
        m->fxUser[2 + 2 * s] = m->fxUser[3 + 2 * s] = b;
        fx_mark(m);   // the FX page's Mix L / R follow
        fx_apply(m, false);
        return true;
    }
    if (strncmp(k, "fx", 2) || (k[2] != '1' && k[2] != '2') || k[3] != '_') return false;
    const int s = k[2] - '1';
    const char* r = k + 4;
    const int iv = (int)lround(v);
    if (!strcmp(r, "on")) { if (iv) m->fxUser[8] |= s ? 0x0C : 0x03; else m->fxUser[8] &= (uint8_t)~(s ? 0x0C : 0x03); }
    else if (!strcmp(r, "cat")) fx_set_cat(m, s, iv);
    else if (!strcmp(r, "type")) fx_set_type(m, s, iv);
    else if (!strcmp(r, "type_prev")) { if (iv) fx_step_type(m, s, -1); }
    else if (!strcmp(r, "type_next")) { if (iv) fx_step_type(m, s, +1); }
    else if (r[0] == 'p' && r[1] >= '1' && r[1] <= '8' && !r[2]) {
        const int j = r[1] - '1';
        if (j >= 6) m->fxUser[2 + 2 * s + (j - 6)] = (uint8_t)clampi(iv, 0, 100);
        else {
            const int t = fx_type(m, s);
            if (t >= 33) return true;
            const Kn& d = kKn[t][j];
            if (d.kind == kNone) return true;
            kn_set(m->fxUser + 9 + 8 * s, d, d.lo + (int)lround(v * (d.hi - d.lo) / 100.0));
        }
    } else return false;
    fx_apply(m, false);
    return true;
}
int fx_param_get(const MOne* m, const char* k, char* buf, int n) {
    if (!strncmp(k, "rev_amt", 7) || !strncmp(k, "dly_amt", 7)) {
        const int s = quick_slot(m, k[0] == 'r');
        const int v = s < 0 ? 0 : (m->fxUser[2 + 2 * s] + m->fxUser[3 + 2 * s] + 1) / 2;
        if (!k[7]) return snprintf(buf, n, "%d", v);
        if (!strcmp(k + 7, "_display")) return s < 0 ? snprintf(buf, n, "None") : snprintf(buf, n, "%d", v);
        return 0;
    }
    if (!strcmp(k, "fx_follow")) return snprintf(buf, n, "%s", m->fxFollow ? "On" : "Off");
    if (!strcmp(k, "fx_reset")) return snprintf(buf, n, "0");
    if (!strcmp(k, "fx_route")) return snprintf(buf, n, "%s", (m->fxUser[8] & 0x10) ? "Serial" : "Parallel");
    if (strncmp(k, "fx", 2) || (k[2] != '1' && k[2] != '2') || k[3] != '_') return 0;
    const int s = k[2] - '1', t = fx_type(m, s);
    const char* r = k + 4;
    if (!strcmp(r, "on")) return snprintf(buf, n, "%s", (m->fxUser[8] & (s ? 0x0C : 0x03)) ? "On" : "Off");
    if (!strcmp(r, "cat")) return snprintf(buf, n, "%s", kCatName[t < 33 ? kFxCat[t] : 0]);
    if (!strcmp(r, "type")) return snprintf(buf, n, "%d", t < 33 ? t : 0);
    if (!strcmp(r, "type_display")) return snprintf(buf, n, "%s", kFxName[t]);
    if (!strcmp(r, "type_prev") || !strcmp(r, "type_next")) return snprintf(buf, n, "0");
    if (r[0] == 'p' && r[1] >= '1' && r[1] <= '8') {
        const int j = r[1] - '1';
        const char* tail = r + 2;
        if (j >= 6) {
            const int b = m->fxUser[2 + 2 * s + (j - 6)];
            if (!*tail) return snprintf(buf, n, "%d", b);
            if (!strcmp(tail, "_name")) return snprintf(buf, n, "%s", j == 6 ? "Mix L" : "Mix R");
            if (!strcmp(tail, "_display")) return snprintf(buf, n, "%d", b);
            return 0;
        }
        const Kn& d = kKn[t < 33 ? t : 0][j];
        const uint8_t* p = m->fxUser + 9 + 8 * s;
        if (!*tail) return snprintf(buf, n, "%d", t < 33 ? kn_pos(p, d) : 0);
        if (!strcmp(tail, "_name")) return snprintf(buf, n, "%s", (t < 33 && d.kind != kNone) ? d.name : "-");
        if (!strcmp(tail, "_display")) { if (t < 33) kn_text(p, d, buf, n); else snprintf(buf, n, " "); return (int)strlen(buf); }
    }
    return 0;
}

// Rebuild the working copies from ROM and lay the macro offsets over the programs in use.
// fx: 0 = leave the effects alone (a knob that doesn't touch them), 1 = new balances only (FX amount knob:
// smooth, keeps the tails), 2 = rebuild the effect units (a new sound). Rebuilding clears the delay RAM, so it
// must not happen on a knob turn (v1.0.0 did: crackle while playing).
void apply_macros(MOne* m, int fx = 2) {
    memcpy(m->progs, src_progs(m), sizeof m->progs);
    memcpy(m->combis, src_combis(m), sizeof m->combis);
    bool use[m1::kPrograms] = {false};
    if (m->mode == 0) {
        use[m->program] = true;
    } else {
        const uint8_t* c = m->combis + m->combi * m1::kCombiLen;
        for (int t = 0; t < m1::kTimbres; ++t) {
            const uint8_t* r = c + 36 + 11 * t;
            if (!(r[10] & 0x10) && r[0] < m1::kPrograms) use[r[0]] = true;
        }
    }
    const int* d = m->macro;
    auto addU = [](uint8_t* p, int delta, int hi) { *p = (uint8_t)clampi((int)*p + delta, 0, hi); };
    for (int i = 0; i < m1::kPrograms; ++i) {
        if (!use[i]) continue;
        uint8_t* p = m->progs + i * m1::kProgLen;
        for (int o = 0; o < 2; ++o) {
            uint8_t* b = p + kOscBase[o];
            addU(b + 8, d[kCutoff] * 2, 99);      // VDF cutoff
            addU(b + 11, d[kEgInt] * 2, 99);      // VDF EG intensity
            addU(b + 29, d[kAttack] * 2, 99);     // VDA EG attack time
            addU(b + 31, d[kDecay] * 2, 99);      // VDA EG decay time
            addU(b + 34, d[kSustain] * 2, 99);    // VDA EG sustain level
            addU(b + 35, d[kRelease] * 2, 99);    // VDA EG release time
        }
        addU(p + 22, d[kVibrato] * 2, 99);        // pitch MG intensity
    }
    if (fx == 2) fx_apply(m, true);
    else if (fx == 1) fx_apply(m, false);
}

void select_sound(MOne* m);
// A new source / author / bank: load it, keep the program and combination numbers, play it.
void lib_changed(MOne* m, unsigned bits) {
    if (m->libSrc == 1) lib_load(m);
    m->changed |= bits | kChgSound | 1u | 2u;
    select_sound(m);
}
void lib_param_set_(MOne* m, const char* r, int iv) {
    const int na = g_nAuth > 0 ? g_nAuth : 1;
    const int nb = g_nAuth > 0 ? g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)].count : 1;
    if (!strcmp(r, "src")) {
        const int s = clampi(iv, 0, 1);
        if (s != m->libSrc) { m->libSrc = s; lib_changed(m, kChgLibSrc); }
        return;
    }
    // Author and bank (knobs, wheel, arrows) only work in the Library; on Factory they do nothing (user's choice,
    // 2026-11-14: switching source is the FACTORY / LIBRARY tabs' job).
    // The author / bank knobs are endless encoders: only the direction of a move counts, and the value goes back to 50
    // after each one (reported, so MPC reads it back and never runs ahead). A slow click (data wheel, a careful
    // Q-Link) moves one item; in a fast Q-Link turn it takes kEncTicks events. A jump (MPC catching up, automation)
    // is ignored. The picked author and bank are kept in lib_aidx / lib_bidx, which projects save and restore.
    if (!strcmp(r, "aidx") || !strcmp(r, "bidx")) {
        const int v = clampi(iv, 0, 255);
        int& cur = r[0] == 'a' ? m->libAuthor : m->libBank;
        if (v != cur && g_nAuth > 0) {
            if (r[0] == 'a') { m->libAuthor = clampi(v, 0, na - 1); }
            else m->libBank = clampi(v, 0, g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)].count - 1);
            lib_changed(m, 0);
        }
        return;
    }
    const bool knob = !strcmp(r, "author") || !strcmp(r, "bank");
    if (knob) {
        const int k = r[0] == 'a' ? 0 : 1;
        int& raw = k ? m->rawBank : m->rawAuthor;
        const int want = clampi(iv, 0, 99), d = want - raw;
        if (d == 0) return;
        raw = 50; m->changed |= k ? kChgLibBank : kChgLibAuthor;          // back to the middle
        if (now_s() - m->bornS <= 3.0 || d > 6 || d < -6) return;           // set-up, or a jump
        if (m->libSrc != 1) return;                                          // Factory: the Library knobs do nothing
        const int dir = d > 0 ? 1 : -1;
        const double now = now_s();
        int step = 0;
        if (now - m->encLast[k] > 0.3) { m->encAcc[k] = 0; step = dir; }
        else {
            if (m->encAcc[k] * dir < 0) m->encAcc[k] = 0;
            m->encAcc[k] += dir;
            if (m->encAcc[k] >= kEncTicks || m->encAcc[k] <= -kEncTicks) { step = dir; m->encAcc[k] = 0; }
        }
        m->encLast[k] = now;
        if (!step) return;
        if (k == 0) { m->libAuthor = (m->libAuthor + step + na) % na; m->libBank = 0; { m->hlReport |= 12; lib_changed(m, 0); } }
        else { m->libBank = (m->libBank + step + nb) % nb; { m->hlReport |= 8; lib_changed(m, 0); } }
        return;
    }
    if (m->libSrc != 1) return;                                              // Factory: the Library arrows do nothing
    if (false) {
    } else if (!strcmp(r, "author_prev") || !strcmp(r, "author_next")) {
        if (!iv) return;
        m->libAuthor = (m->libAuthor + (r[7] == 'n' ? 1 : -1) + na) % na; m->libBank = 0;
        { m->hlReport |= 12; lib_changed(m, 0); }
    } else if (!strcmp(r, "bank_prev") || !strcmp(r, "bank_next")) {   // runs on through the authors
        if (!iv) return;
        if (r[5] == 'n') { if (++m->libBank >= nb) { m->libBank = 0; m->libAuthor = (m->libAuthor + 1) % na; } }
        else if (--m->libBank < 0) { m->libAuthor = (m->libAuthor - 1 + na) % na;
                                     m->libBank = g_nAuth > 0 ? g_auth[m->libAuthor].count - 1 : 0; }
        { m->hlReport |= 12; lib_changed(m, 0); }
    }
}

// Lights the outline of the box that changed (author or bank) for 1.5 s; mpc_engine_changed reports it and puts it out.
void lib_param_set(MOne* m, const char* r, int iv) {
    if (strstr(r, "_hl")) return;                    // the outlines are the plugin's own, MPC only shows them
    MONE_TRACE("set lib_%s %d", r, iv);
    const int a0 = m->libAuthor, b0 = m->libBank;
    lib_param_set_(m, r, iv);
    MONE_TRACE("  -> src %d author %d bank %d raw %d/%d acc %d/%d", m->libSrc, m->libAuthor, m->libBank, m->rawAuthor, m->rawBank, m->encAcc[0], m->encAcc[1]);
    if (m->libSrc != 1) return;
    if (m->libAuthor != a0) m->changed |= kChgLibAuthor | kChgLibBank;   // a new author: its bank box (name, n/N) at once
    if (m->libAuthor != a0) { if (!m->hlAuthor) m->hlReport |= 1; m->hlAuthor = 1; if (m->hlBank) { m->hlBank = 0; m->hlReport |= 2; } m->hlTime = now_s(); }
    else if (m->libBank != b0) { if (!m->hlBank) m->hlReport |= 2; m->hlBank = 1; if (m->hlAuthor) { m->hlAuthor = 0; m->hlReport |= 1; } m->hlTime = now_s(); }
}

// The browser page follows the sound (Preset knob, arrows, mode switch), so the highlighted tile is always in view.
void page_follow(MOne* m) {
    const int pg = (m->mode ? m->combi : m->program) / 20;
    if (pg != m->bank) { m->bank = pg; m->changed |= kChgPage; }
}
void fx_follow_sound(MOne* m) {
    if (!m->fxFollow) return;
    const int idx = m->mode ? m->combi : m->program;
    memcpy(m->fxUser, m->fxCache[m->mode].idx == idx ? m->fxCache[m->mode].b : stored_fx(m), sizeof m->fxUser);
    fx_mark(m);
}
void select_sound(MOne* m) {
    page_follow(m);
    memcpy(m->progs, src_progs(m), sizeof m->progs);
    memcpy(m->combis, src_combis(m), sizeof m->combis);
    if (m->noRom) return;                            // no engine without ROMs: the names are all there is
    m->eng.setKits(src_kits(m));
    // A Q-Link scrolling through sounds: the effects stay as they are until the knob stops (mpc_engine_changed), so
    // each click only loads the voices (rebuilding both effects every click glitched the Force).
    const bool picking = now_s() - m->lastPick < 2.0;
    m->eng.setKeepFx(picking);
    if (m->mode) m->eng.selectCombi(m->combi); else m->eng.selectProgram(m->program);
    m->eng.setKeepFx(false);
    if (picking) { m->fxLate = true; apply_macros(m, 0); return; }
    m->fxLate = false;
    fx_follow_sound(m);
    apply_macros(m);   // rebuilds the effects from the FX page's block
}

void name_of(const uint8_t* rec, char* out) {
    int n = 0;
    for (int i = 0; i < 10; ++i) { char ch = (char)rec[i]; out[n++] = (ch >= 32 && ch < 127) ? ch : ' '; }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
}

void* create(const char*) {
    build_table();
    rom_init();
    MOne* m = new (std::nothrow) MOne();
    if (!m) return nullptr;
    m->bornS = now_s();
    lib_init();
    if (!g_rom) { m->noRom = true; fx_defaults(m); return m; }   // silent until the ROMs are in Content/ROMs
    const m1load::Rom& R = *g_rom;
    memcpy(m->kernel, R.kernel, sizeof m->kernel);
    memcpy(m->global, R.global, sizeof m->global);
    memcpy(m->progs, R.programs, sizeof m->progs);   // FACTORY
    memcpy(m->combis, R.combis, sizeof m->combis);   // FACTORY
    m1::RomTables& r = m->rom;
    r.pcm = R.pcm; r.pcmWords = R.pcmWords;
    r.ms = R.ms; r.splits = R.splits;
    r.drums = R.drums; r.nDrums = R.nDrums;
    r.kernel = m->kernel; r.vdf1k = R.vdf1k; r.vdf2k = R.vdf2k;
    r.vdf1t0 = R.vdf1t0; r.vdf2t0 = R.vdf2t0;
    r.egRise = R.egRise; r.egFall = R.egFall; r.velCurve = R.velCurve;
    r.mgFreq = R.mgFreq; r.mgDelay = R.mgDelay; r.mgIntP = R.mgIntP; r.mgIntF = R.mgIntF;
    r.pitchS = R.pitchS; r.pitchF = R.pitchF; r.panReg = R.panReg; r.pan101 = R.pan101;
    r.delayStart = R.delayStart; r.fx = &R.fx;
    r.programs = m->progs; r.combis = m->combis; r.kits = R.kits;
    r.global = m->global; r.scale = R.scale;
    if (!m->eng.init(r, m->fxMem)) { delete m; return nullptr; }
    fx_defaults(m);
    select_sound(m);
    return m;
}

void destroy(void* i) { delete static_cast<MOne*>(i); }

void step_sound(MOne* m, int delta) {
    if (m->mode) { m->combi = (m->combi + delta + 100) % 100; m->changed |= 2; }
    else { m->program = (m->program + delta + 100) % 100; m->changed |= 1; }
    m->changed |= kChgSound;
    select_sound(m);
}

void midi(void* i, const uint8_t* msg, int len) {
    MOne* m = static_cast<MOne*>(i);
    if (len < 1 || m->noRom) return;
    const uint8_t st = msg[0] & 0xF0;
    const uint8_t a = len > 1 ? msg[1] & 0x7F : 0, b = len > 2 ? msg[2] & 0x7F : 0;
    switch (st) {
    case 0x90: if (b) m->eng.noteOn(0, a, b); else m->eng.noteOff(0, a); break;
    case 0x80: m->eng.noteOff(0, a); break;
    case 0xE0: m->eng.pitchBend(0, (int16_t)(((b << 7) | a) - 8192)); break;
    case 0xD0: m->eng.aftertouch(0, a); break;
    case 0xC0:
        if (m->mode) { m->combi = a % 100; m->changed |= 2; } else { m->program = a % 100; m->changed |= 1; }
        m->changed |= kChgSound;
        select_sound(m);
        break;
    case 0xB0:
        if (a == 1) m->eng.modWheel(0, b);
        else if (a == 2) m->eng.modWheelNeg(0, b);
        else if (a == 64) m->eng.damper(0, b >= 64);
        else if (a == 120) m->eng.allSoundOff();
        else if (a == 123) m->eng.allNotesOff();
        break;
    default: break;
    }
}

// ---- Q-Link sections (2026-11-14): the Force has 8 Q-Links; Play and FX have more controls. A tap on a section (Quick
// Edit / Sound, Effect 1 / Effect 2) points the 8 proxy knobs pk1..8 (Play) / fk1..8 (FX) at it. A proxy is an endless
// knob: it shows its target's name, value and position, and each Q-Link event steps the target (bigger steps in a fast
// turn). The XL keeps its direct 16 Q-Links.
const char* const kQTarget[3][2][8] = {
    {{"cutoff", "eg_int", "attack", "decay", "sustain", "release", "vibrato", "fx_amt"},
     {"volume", "sound", "rev_amt", "dly_amt", nullptr, nullptr, nullptr, nullptr}},
    {{"fx1_p1", "fx1_p2", "fx1_p3", "fx1_p4", "fx1_p5", "fx1_p6", "fx1_p7", "fx1_p8"},
     {"fx2_p1", "fx2_p2", "fx2_p3", "fx2_p4", "fx2_p5", "fx2_p6", "fx2_p7", "fx2_p8"}},
    {{"sound", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},          // Browse: one Preset knob
     {"sound", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}}};
const char* const kQSelKey[2] = {"q_play", "q_fx"};
int pidx(const char* k) { for (int i = 0; i < (int)(sizeof PARAMS / sizeof PARAMS[0]); ++i) if (!strcmp(PARAMS[i].key, k)) return i; return -1; }
void q_push(MOne* m, const char* k) {
    const int i = pidx(k); if (i < 0) return;
    for (int j = 0; j < m->nPend; ++j) if (m->pend[j] == i) return;
    if (m->nPend < (int)(sizeof m->pend / sizeof m->pend[0])) m->pend[m->nPend++] = i;
}
// a proxy key -> {page, knob}, or false
const char kQPre[3] = {'p', 'f', 'b'};
bool q_key(const char* k, int* pg, int* kn) {
    if (k[1] != 'k' || k[2] < '1' || k[2] > '8') return false;
    for (int g = 0; g < 3; ++g) if (k[0] == kQPre[g]) { *pg = g; *kn = k[2] - '1'; return true; }
    return false;
}
const char* q_target(const MOne* m, int pg, int kn) { return kQTarget[pg][pg < 2 ? m->qSel[pg] : 0][kn]; }
int get_param(void* i, const char* k, char* buf, int n);
void set_param(void* i, const char* k, const char* v);
float q_norm(MOne* m, const char* t) {                // a target's position, 0..1
    const int i = pidx(t); if (i < 0) return 0;
    char b[64] = ""; if (get_param(m, t, b, sizeof b) <= 0) return 0;
    const param_t& P = PARAMS[i];
    if (P.nopts > 1) {
        int idx = -1; for (int j = 0; j < P.nopts; ++j) if (!strcasecmp(b, P.opts[j])) idx = j;
        if (idx < 0) idx = atoi(b);
        return clampi(idx, 0, P.nopts - 1) / (float)(P.nopts - 1);
    }
    if (P.max <= P.min) return 0;
    const float v = ((float)atof(b) - P.min) / (P.max - P.min);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
void q_step(MOne* m, const char* t, int dir, int mult) {
    const int i = pidx(t); if (i < 0) return;
    const param_t& P = PARAMS[i];
    char b[64] = ""; get_param(m, t, b, sizeof b);
    char out[32];
    if (P.nopts > 1) {
        int idx = -1; for (int j = 0; j < P.nopts; ++j) if (!strcasecmp(b, P.opts[j])) idx = j;
        if (idx < 0) idx = atoi(b);
        snprintf(out, sizeof out, "%d", clampi(idx + dir, 0, P.nopts - 1));
    } else {
        const bool whole = P.int_display || P.max - P.min > 20;
        const float step = whole ? 1.0f : (P.max - P.min) / 100.0f;   // a value knob: 100 clicks end to end, like the Quick Edit knobs
        float v = (float)atof(b) + dir * mult * step;
        v = v < P.min ? P.min : v > P.max ? P.max : v;
        if (whole) snprintf(out, sizeof out, "%d", (int)lroundf(v)); else snprintf(out, sizeof out, "%.2f", v);
    }
    set_param(m, t, out);
}
// MPC set a proxy (Q-Link) or a section selector; true if it was one
bool q_set(MOne* m, const char* k, const char* v) {
    const int iv = (int)lround(atof(v));
    for (int pg = 0; pg < 2; ++pg)
        if (!strcmp(k, kQSelKey[pg])) {
            const int s = clampi(iv, 0, 1);
            if (s != m->qSel[pg]) {
                m->qSel[pg] = s; q_push(m, kQSelKey[pg]);
                char pk[4] = {kQPre[pg], 'k', '1', 0};
                for (int j = 0; j < 8; ++j) { pk[2] = (char)('1' + j); q_push(m, pk); }
            }
            return true;
        }
    int pg, kn; if (!q_key(k, &pg, &kn) || k[3]) return false;
    const char* t = q_target(m, pg, kn);
    if (!t) { q_push(m, k); return true; }
    // A proxy is a 0..1 knob that MPC moves in small steps (a Q-Link event is ~1/128) along its own track: while it
    // turns MPC keeps going from what it last sent and doesn't take our read-backs, so the direction is measured from its
    // last value; after a pause (0.5 s) from what it last read back from us, which it then starts from.
    const float want = (float)atof(v);
    const double now = now_s();
    const float ref = now - m->qSentAt[pg][kn] < 0.5 ? m->qSent[pg][kn] : m->qRead[pg][kn];
    const float d = want - ref;
    m->qSent[pg][kn] = want; m->qSentAt[pg][kn] = now;
    if (d < 0.001f && d > -0.001f) return true;
    // One Q-Link click moves MPC's copy by 1/128 (0.0078). A bigger move is MPC starting again from a value it read back
    // from us (after a report or a screen rebuild): not a turn. Taking it as one made the knob step backwards now and then,
    // and on a sound knob each of those loaded a sound (and its effects) -- the "glitches".
    if (d > 0.03f || d < -0.03f) { MONE_TRACE("  q %s %.4f ref %.4f RESYNC", k, want, ref); return true; }
    const bool pick = !strcmp(t, "sound");                // a sound pick: one per click, never accelerated
    if (pick) {                       // slow (user, 2026-11-15): the first click after a pause picks, then every 4th (Play's Sound Select: 12th)
        m->lastPick = now;
        if (now - m->qLast[pg][kn] < 0.5 && ++m->pickAcc[pg] < (pg == 0 ? 12 : 4)) { m->qLast[pg][kn] = now; q_push(m, k); return true; }
        m->pickAcc[pg] = 0;
    }
    const int mult = !pick && now - m->qLast[pg][kn] < 0.06 ? 3 : 1;
    m->qLast[pg][kn] = now;
    MONE_TRACE("  q %s %.4f ref %.4f -> %s %+d x%d", k, want, ref, t, d > 0 ? 1 : -1, mult);
    q_step(m, t, d > 0 ? 1 : -1, mult);
    q_push(m, k); q_push(m, t);
    return true;
}
// MPC reads a proxy / selector; true if it was one (n written to *len)
bool q_get(MOne* m, const char* k, char* buf, int n, int* len) {
    for (int pg = 0; pg < 2; ++pg)
        if (!strcmp(k, kQSelKey[pg])) { *len = snprintf(buf, n, "%s", pg ? (m->qSel[pg] ? "FX 2" : "FX 1") : (m->qSel[pg] ? "Bottom" : "Top")); return true; }
    int pg, kn; if (!q_key(k, &pg, &kn)) return false;
    const char* t = q_target(m, pg, kn);
    const char* r = k + 3;
    if (!*r) { const float v = t ? q_norm(m, t) : 0.5f; m->qRead[pg][kn] = v; *len = snprintf(buf, n, "%.4f", v); return true; }
    if (!t) { *len = snprintf(buf, n, " "); return true; }
    const int i = pidx(t); if (i < 0) { *len = 0; return true; }
    const param_t& P = PARAMS[i];
    char k2[40];
    if (!strcmp(r, "_name")) {
        snprintf(k2, sizeof k2, "%s_name", t);
        if (P.dynamic_name && get_param(m, k2, buf, n) > 0) { *len = (int)strlen(buf); return true; }
        *len = snprintf(buf, n, "%s", P.name); return true;
    }
    if (!strcmp(r, "_display")) {
        snprintf(k2, sizeof k2, "%s_display", t);
        if (P.dynamic_display && get_param(m, k2, buf, n) > 0) { *len = (int)strlen(buf); return true; }
        char b[64] = ""; get_param(m, t, b, sizeof b);
        if (P.nopts > 1 || !P.max) { *len = snprintf(buf, n, "%s", b); return true; }
        const bool whole = P.int_display || P.max - P.min > 20;
        *len = whole ? snprintf(buf, n, "%d%s%s", (int)lroundf((float)atof(b)), *P.unit ? " " : "", P.unit)
                     : snprintf(buf, n, "%.1f%s%s", atof(b), *P.unit ? " " : "", P.unit);
        return true;
    }
    *len = 0; return true;
}
// a target moved some other way (the screen, a preset): its proxy follows
void q_follow(MOne* m, const char* k) {
    for (int pg = 0; pg < 3; ++pg) for (int kn = 0; kn < 8; ++kn) {
        const char* t = q_target(m, pg, kn);
        if (t && !strcmp(t, k)) { char pk[4] = {kQPre[pg], 'k', (char)('1' + kn), 0}; q_push(m, pk); }
    }
}

void set_param_(void* i, const char* k, const char* v);
void set_param(void* i, const char* k, const char* v) {
    MOne* m = static_cast<MOne*>(i);
    if (q_set(m, k, v)) return;
    set_param_(i, k, v);
    q_follow(m, k);
    for (int j = 0; j < kFxKeyCount; ++j)          // MPC set an FX value itself: that is what it now shows
        if (!strcmp(k, kFxKeys[j])) { char b[64] = ""; get_param(m, k, b, sizeof b); snprintf(m->fxSnap[j], sizeof m->fxSnap[j], "%s", b); break; }
}
void set_param_(void* i, const char* k, const char* v) {
    MOne* m = static_cast<MOne*>(i);
    const int iv = (int)lround(atof(v));
    if (m->noRom) {                               // no ROMs: keep the settings (knobs, saved projects), make no sound
        int& cur = m->mode ? m->combi : m->program;
        if (!strcmp(k, "mode")) m->mode = clampi(iv, 0, 1);
        else if (!strcmp(k, "program")) m->program = clampi(iv, 0, 99);
        else if (!strcmp(k, "combi")) m->combi = clampi(iv, 0, 99);
        else if (!strcmp(k, "sound")) cur = clampi(iv, 0, 99);
        else if (!strcmp(k, "prev")) { if (iv) cur = (cur + 99) % 100; }
        else if (!strcmp(k, "next")) { if (iv) cur = (cur + 1) % 100; }
        else if (!strcmp(k, "bank")) m->bank = clampi(iv, 0, 4);
        else if (!strncmp(k, "sl_", 3)) cur = m->bank * 20 + clampi(atoi(k + 3), 1, 20) - 1;
        else if (!strcmp(k, "volume")) m->volDb = (float)atof(v);
        else if (!strncmp(k, "lib_", 4)) lib_param_set(m, k + 4, iv);    // the Library browses without ROMs too
        else for (int j = 0; j < kMacros; ++j) if (!strcmp(k, kMacroKeys[j])) m->macro[j] = clampi(iv, -50, 50);
        if (!strcmp(k, "sound") || !strcmp(k, "prev") || !strcmp(k, "next") || !strcmp(k, "mode")) page_follow(m);
        return;
    }
    if (!strcmp(k, "mode")) {
        const int nm = clampi(iv, 0, 1);
        if (nm != m->mode) { m->mode = nm; m->changed |= kChgSound; select_sound(m); }
    } else if (!strcmp(k, "program")) {           // kept for saved projects and the PRESET menu; the screen uses "sound"
        const int np = clampi(iv, 0, 99);
        if (np != m->program) { m->program = np; if (!m->mode) { m->changed |= kChgSound; select_sound(m); } }
    } else if (!strcmp(k, "combi")) {
        const int nc = clampi(iv, 0, 99);
        if (nc != m->combi) { m->combi = nc; if (m->mode) { m->changed |= kChgSound; select_sound(m); } }
    } else if (!strcmp(k, "sound")) {             // the display: step the current mode's list
        const int ns = clampi(iv, 0, 99);
        int& cur = m->mode ? m->combi : m->program;
        if (ns != cur) { cur = ns; m->changed |= m->mode ? 2u : 1u; select_sound(m); }   // reports program/combi too
    } else if (!strcmp(k, "prev")) { if (iv) step_sound(m, -1); }
    else if (!strcmp(k, "next")) { if (iv) step_sound(m, +1); }
    else if (!strcmp(k, "bank")) m->bank = clampi(iv, 0, 4);
    else if (!strncmp(k, "sl_", 3)) {           // a browser tile: pick that program / combination
        const int idx = m->bank * 20 + clampi(atoi(k + 3), 1, 20) - 1;
        if (m->mode) { if (idx != m->combi) { m->combi = idx; m->changed |= 2 | kChgSound; select_sound(m); } }
        else if (idx != m->program) { m->program = idx; m->changed |= 1 | kChgSound; select_sound(m); }
    }
    else if (!strcmp(k, "volume")) m->volDb = (float)atof(v);
    else if (!strcmp(k, "fx")) { m->fxOn = iv != 0; m->eng.setBypassFx(!m->fxOn); }
    else if (!strcmp(k, "fx_follow")) m->fxFollow = iv != 0;
    else if (!strcmp(k, "fx_reset")) { if (iv) { memcpy(m->fxUser, stored_fx(m), sizeof m->fxUser); fx_mark(m); fx_apply(m, true); } }
    else if (!strcmp(k, "fx_route")) {
        const uint8_t old = m->fxUser[8];
        if (iv) m->fxUser[8] |= 0x10; else m->fxUser[8] &= (uint8_t)~0x10;
        if (m->fxUser[8] != old) fx_apply(m, true);   // a new routing: rebuild (both effects' tails restart)
    }
    else if (!strncmp(k, "lib_", 4)) lib_param_set(m, k + 4, iv);
    else if (fx_param_set(m, k, atof(v))) {}
    else {
        for (int j = 0; j < kMacros; ++j)
            if (!strcmp(k, kMacroKeys[j])) {
                const int nv = clampi(iv, -50, 50);
                if (nv != m->macro[j]) { m->macro[j] = nv; apply_macros(m, j == kFxAmt ? 1 : 0); }
                return;
            }
    }
}

int get_param(void* i, const char* k, char* buf, int n) {
    MOne* m = static_cast<MOne*>(i);
    { int len; if (q_get(m, k, buf, n, &len)) return len; }
    char nm[16];
    if (m->noRom) {
        if (!strcmp(k, "sound_display")) {
            if (!use_lib(m)) return snprintf(buf, n, "NO M1 ROMS");
            if (m->mode) name_of(src_combis(m) + m->combi * m1::kCombiLen, nm); else name_of(src_progs(m) + m->program * m1::kProgLen, nm);
            return snprintf(buf, n, "%02d %s (no ROMs)", m->mode ? m->combi : m->program, nm);
        }
        if (!strcmp(k, "program_display") || !strcmp(k, "combi_display")) return snprintf(buf, n, "Put them in Content/ROMs");
    }
    if (!strcmp(k, "mode")) return snprintf(buf, n, "%s", m->mode ? "Combi" : "Program");
    if (!strcmp(k, "program")) return snprintf(buf, n, "%d", m->program);
    if (!strcmp(k, "combi")) return snprintf(buf, n, "%d", m->combi);
    if (!strcmp(k, "sound")) return snprintf(buf, n, "%d", m->mode ? m->combi : m->program);
    if (!strcmp(k, "sound_display")) {
        if (m->mode) name_of(src_combis(m) + m->combi * m1::kCombiLen, nm); else name_of(src_progs(m) + m->program * m1::kProgLen, nm);
        return snprintf(buf, n, "%02d %s", m->mode ? m->combi : m->program, nm);
    }
    if (!strcmp(k, "program_display")) { name_of(src_progs(m) + m->program * m1::kProgLen, nm); return snprintf(buf, n, "%02d %s", m->program, nm); }
    if (!strcmp(k, "combi_display")) { name_of(src_combis(m) + m->combi * m1::kCombiLen, nm); return snprintf(buf, n, "%02d %s", m->combi, nm); }
    if (!strcmp(k, "name")) {
        if (m->mode) name_of(src_combis(m) + m->combi * m1::kCombiLen, nm); else name_of(src_progs(m) + m->program * m1::kProgLen, nm);
        return snprintf(buf, n, "%s", nm);
    }
    if (!strcmp(k, "number")) return snprintf(buf, n, "%s I%02d", m->mode ? "COMBI" : "PROG", m->mode ? m->combi : m->program);   // the M1's own "PROG I00"
    if (!strncmp(k, "program:", 8)) {
        const int p = clampi(atoi(k + 8), 0, 99);
        name_of(src_progs(m) + p * m1::kProgLen, nm);
        return snprintf(buf, n, "%02d %s", p, nm);
    }
    if (!strcmp(k, "prev") || !strcmp(k, "next")) return snprintf(buf, n, "0");
    if (!strcmp(k, "bank")) return snprintf(buf, n, "%d", m->bank);
    if (!strncmp(k, "sl_", 3)) {
        const int slot = clampi(atoi(k + 3), 1, 20), idx = m->bank * 20 + slot - 1;
        const bool on = (m->mode ? m->combi : m->program) == idx;
        if (strstr(k, "_on")) return snprintf(buf, n, "%d", on ? 1 : 0);
        name_of(m->mode ? src_combis(m) + idx * m1::kCombiLen : src_progs(m) + idx * m1::kProgLen, nm);
        return snprintf(buf, n, "%02d %s", idx, nm);
    }
    if (!strcmp(k, "volume")) return snprintf(buf, n, "%.1f", m->volDb);
    if (!strcmp(k, "fx")) return snprintf(buf, n, "%s", m->fxOn ? "On" : "Off");
    if (!strncmp(k, "rev_amt", 7) || !strncmp(k, "dly_amt", 7)) return fx_param_get(m, k, buf, n);
    if (!strncmp(k, "lib_", 4)) {
        const char* r = k + 4;
        const bool lib = m->libSrc == 1;
        if (!strcmp(r, "src")) return snprintf(buf, n, "%s", lib ? "Library" : "Factory");
        if (!strcmp(r, "aidx")) return snprintf(buf, n, "%d", m->libAuthor);
        if (!strcmp(r, "bidx")) return snprintf(buf, n, "%d", m->libBank);
        if (!strcmp(r, "author_hl")) return snprintf(buf, n, "%s", m->hlAuthor ? "On" : "Off");
        if (!strcmp(r, "bank_hl")) return snprintf(buf, n, "%s", m->hlBank ? "On" : "Off");
        if (!strcmp(r, "author")) return snprintf(buf, n, "%d", m->rawAuthor);
        if (!strcmp(r, "bank")) return snprintf(buf, n, "%d", m->rawBank);
        if (!strcmp(r, "author_display")) {
            if (!lib) return snprintf(buf, n, "Factory");
            if (!g_nAuth) return snprintf(buf, n, "No banks found");
            return snprintf(buf, n, "%s", g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)].name);
        }
        if (!strcmp(r, "bank_display")) {
            if (!lib) return snprintf(buf, n, "M1 Preload");
            const int idx = lib_index(m);
            if (idx < 0) return snprintf(buf, n, "-");
            const LibAuthor& A = g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)];
            return snprintf(buf, n, "%s  %d/%d%s", g_bank[idx].name, idx - A.first + 1, A.count,
                            m->libLoaded != idx ? " (not loaded)" : "");
        }
        return snprintf(buf, n, "0");                    // the prev / next triggers
    }
    if (!strncmp(k, "fx_", 3) || !strncmp(k, "fx1_", 4) || !strncmp(k, "fx2_", 4)) {
        if (!strcmp(k, "fx_amt") || !strcmp(k, "fx_amt_display")) {} else return fx_param_get(m, k, buf, n);
    }
    for (int j = 0; j < kMacros; ++j) {
        if (!strcmp(k, kMacroKeys[j])) return snprintf(buf, n, "%d", m->macro[j]);
        const size_t kl = strlen(kMacroKeys[j]);   // "<key>_display": offsets read +8 / 0 / -12
        if (!strncmp(k, kMacroKeys[j], kl) && !strcmp(k + kl, "_display")) return snprintf(buf, n, m->macro[j] > 0 ? "+%d" : "%d", m->macro[j]);
    }
    return 0;
}

// Unity below 0.8 (-1.9 dBFS), then a smooth knee that never passes 1.0: the M1's drums and full combis peak above
// full scale, and a hard clip there crackles.
inline float soft(float x) {
    const float a = fabsf(x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

void render(void* i, int16_t* out, int frames) {
    MOne* m = static_cast<MOne*>(i);
    if (m->noRom) { memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); return; }
    const float gain = powf(10.0f, m->volDb / 20.0f);
    for (int f = 0; f < frames; ++f) {
        long long n0 = (long long)floor(m->pos);
        while (n0 + kHalf >= m->have) {                  // need input up to n0 + 8
            m->eng.render(m->blkL, m->blkR, m1::kRenderBlock);
            for (int j = 0; j < m1::kRenderBlock; ++j) {
                const int w = (int)((m->have + j) & (kRing - 1));
                m->inL[w] = m->blkL[j]; m->inR[w] = m->blkR[j];
            }
            m->have += m1::kRenderBlock;
        }
        const double fr = m->pos - (double)n0;
        const double pf = fr * kPhases;
        const int p = (int)pf;
        const float t = (float)(pf - p);
        float l = 0, r = 0;
        for (int j = 0; j < kTaps; ++j) {
            const float h = g_tab[p][j] + t * (g_tab[p + 1][j] - g_tab[p][j]);
            const int w = (int)((n0 - (kHalf - 1) + j) & (kRing - 1));
            l += h * m->inL[w]; r += h * m->inR[w];
        }
        m->pos += kStep;
        out[2 * f] = (int16_t)lrintf(soft(l * gain) * 32767.0f);
        out[2 * f + 1] = (int16_t)lrintf(soft(r * gain) * 32767.0f);
    }
}

const mpc_engine_t kEngine = {create, destroy, midi, set_param, get_param, render, nullptr, nullptr};

}  // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }

extern "C" int mpc_engine_changed(void* inst, char* key, int len) {
    MOne* m = static_cast<MOne*>(inst);
    if (m->fxDirty && now_s() - m->fxDirtyAt > 0.35) {
        m->fxDirty = false;
        char b[64];
        for (int i = 0; i < kFxKeyCount; ++i) {
            b[0] = 0; get_param(m, kFxKeys[i], b, sizeof b);
            if (strncmp(b, m->fxSnap[i], sizeof m->fxSnap[i] - 1)) { snprintf(m->fxSnap[i], sizeof m->fxSnap[i], "%s", b); m->changed |= 1u << (2 + i); }
        }
    }
    if (m->nPend > 0) { const int i = m->pend[0]; memmove(m->pend, m->pend + 1, sizeof(int) * (size_t)--m->nPend); snprintf(key, len, "%s", PARAMS[i].key); return 1; }
    // "program" is MPC's preset number (PROG_PARAM): telling it about each sound a Q-Link turn passes made MPC treat every
    // click as a preset change (screen and Q-Link screens rebuilt, the knob dropped). Told once the knob has stopped.
    const bool picking = now_s() - m->lastPick < 2.0;
    if (m->fxLate && !picking && !m->noRom) { m->fxLate = false; fx_follow_sound(m); fx_apply(m, false); }   // retune: a type kept carries on, no restart
    if ((m->changed & 1) && !picking) { m->changed &= ~1u; snprintf(key, len, "program"); return 1; }
    if ((m->changed & 2) && !picking) { m->changed &= ~2u; snprintf(key, len, "combi"); return 1; }
    if (m->changed & kChgSound) { m->changed &= ~kChgSound; snprintf(key, len, "sound"); return 1; }
    if (m->changed & kChgPage) { m->changed &= ~kChgPage; snprintf(key, len, "bank"); return 1; }
    if (m->changed & kChgLibSrc) { m->changed &= ~kChgLibSrc; snprintf(key, len, "lib_src"); return 1; }
    if (m->changed & kChgLibAuthor) { m->changed &= ~kChgLibAuthor; snprintf(key, len, "lib_author"); return 1; }
    if (m->changed & kChgLibBank) { m->changed &= ~kChgLibBank; snprintf(key, len, "lib_bank"); return 1; }
    if ((m->hlAuthor || m->hlBank) && now_s() - m->hlTime > 1.5) {
        if (m->hlAuthor) m->hlReport |= 1;
        if (m->hlBank) m->hlReport |= 2;
        m->hlAuthor = m->hlBank = 0;
    }
    if (m->hlReport & 1) { m->hlReport &= ~1; snprintf(key, len, "lib_author_hl"); return 1; }
    if (m->hlReport & 2) { m->hlReport &= ~2; snprintf(key, len, "lib_bank_hl"); return 1; }
    if (m->hlReport & 4) { m->hlReport &= ~4; snprintf(key, len, "lib_aidx"); return 1; }
    if (m->hlReport & 8) { m->hlReport &= ~8; snprintf(key, len, "lib_bidx"); return 1; }
    for (int i = 0; i < kFxKeyCount; ++i)
        if (m->changed & (1u << (2 + i))) { m->changed &= ~(1u << (2 + i)); snprintf(key, len, "%s", kFxKeys[i]); return 1; }
    return 0;
}
