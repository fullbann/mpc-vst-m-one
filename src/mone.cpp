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
constexpr unsigned kChgLibSrc = 1u << 27, kChgLibAuthor = 1u << 28, kChgLibBank = 1u << 29, kChgSound = 1u << 30;
static_assert(2 + sizeof kFxKeys / sizeof kFxKeys[0] <= 27, "MOne::changed bits");

struct MOne {
    m1::Engine eng;
    m1::RomTables rom{};
    int16_t fxMem[65536];
    uint8_t kernel[32768];
    bool noRom = false;                           // no ROMs: silent, the display says so
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
void fx_mark(MOne* m) { m->changed |= ((1u << kFxKeyCount) - 1u) << 2; }
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
void lib_param_set(MOne* m, const char* r, int iv) {
    const int na = g_nAuth > 0 ? g_nAuth : 1;
    const int nb = g_nAuth > 0 ? g_auth[clampi(m->libAuthor, 0, g_nAuth - 1)].count : 1;
    if (!strcmp(r, "src")) {
        const int s = clampi(iv, 0, 1);
        if (s != m->libSrc) { m->libSrc = s; lib_changed(m, kChgLibSrc); }
    } else if (!strcmp(r, "author")) {
        const int a = clampi(iv, 0, na - 1);
        if (a != m->libAuthor) { m->libAuthor = a; m->libBank = 0; lib_changed(m, kChgLibAuthor | kChgLibBank); }
    } else if (!strcmp(r, "author_prev") || !strcmp(r, "author_next")) {
        if (!iv) return;
        m->libAuthor = (m->libAuthor + (r[7] == 'n' ? 1 : -1) + na) % na; m->libBank = 0;
        lib_changed(m, kChgLibAuthor | kChgLibBank);
    } else if (!strcmp(r, "bank")) {
        const int b = clampi(iv, 0, nb - 1);
        if (b != m->libBank) { m->libBank = b; lib_changed(m, kChgLibBank); }
    } else if (!strcmp(r, "bank_prev") || !strcmp(r, "bank_next")) {   // runs on through the authors
        if (!iv) return;
        if (r[5] == 'n') { if (++m->libBank >= nb) { m->libBank = 0; m->libAuthor = (m->libAuthor + 1) % na; } }
        else if (--m->libBank < 0) { m->libAuthor = (m->libAuthor - 1 + na) % na;
                                     m->libBank = g_nAuth > 0 ? g_auth[m->libAuthor].count - 1 : 0; }
        lib_changed(m, kChgLibAuthor | kChgLibBank);
    }
}

void select_sound(MOne* m) {
    memcpy(m->progs, src_progs(m), sizeof m->progs);
    memcpy(m->combis, src_combis(m), sizeof m->combis);
    m->eng.setKits(src_kits(m));
    if (m->mode) m->eng.selectCombi(m->combi); else m->eng.selectProgram(m->program);
    if (m->fxFollow) {
        const int idx = m->mode ? m->combi : m->program;
        memcpy(m->fxUser, m->fxCache[m->mode].idx == idx ? m->fxCache[m->mode].b : stored_fx(m), sizeof m->fxUser);
        fx_mark(m);
    }
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

void set_param(void* i, const char* k, const char* v) {
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
        else for (int j = 0; j < kMacros; ++j) if (!strcmp(k, kMacroKeys[j])) m->macro[j] = clampi(iv, -50, 50);
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
    char nm[16];
    if (m->noRom) {
        if (!strcmp(k, "sound_display")) return snprintf(buf, n, "NO M1 ROMS");
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
        if (!strcmp(r, "author")) return snprintf(buf, n, "%d", m->libAuthor);
        if (!strcmp(r, "bank")) return snprintf(buf, n, "%d", m->libBank);
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
    if (m->changed & 1) { m->changed &= ~1u; snprintf(key, len, "program"); return 1; }
    if (m->changed & 2) { m->changed &= ~2u; snprintf(key, len, "combi"); return 1; }
    if (m->changed & kChgSound) { m->changed &= ~kChgSound; snprintf(key, len, "sound"); return 1; }
    if (m->changed & kChgLibSrc) { m->changed &= ~kChgLibSrc; snprintf(key, len, "lib_src"); return 1; }
    if (m->changed & kChgLibAuthor) { m->changed &= ~kChgLibAuthor; snprintf(key, len, "lib_author"); return 1; }
    if (m->changed & kChgLibBank) { m->changed &= ~kChgLibBank; snprintf(key, len, "lib_bank"); return 1; }
    for (int i = 0; i < kFxKeyCount; ++i)
        if (m->changed & (1u << (2 + i))) { m->changed &= ~(1u << (2 + i)); snprintf(key, len, "%s", kFxKeys[i]); return 1; }
    return 0;
}
