// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Michi71 (tools/m1_extract, MIT), 2026 Fullban (C++ port)
//
// m1_romload.cpp -- C++ port of tools/m1_extract (m1_rom.py, m1_make_blob.py, m1_fx_tables.py): reads the
// user's Korg M1 ROM set when the plugin starts and builds every table the engine reads. Offsets, checks and
// arithmetic follow the Python one to one; see the comments there for where each value comes from.
#include "m1_romload.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

namespace m1load {
namespace {

constexpr uint32_t kOsSize = 0x20000, kBase = 0xE0000;
constexpr uint32_t kDS = 0xF75C0, kFX = 0xF2C30, kTPL = 0xF4280;
constexpr int kMsTable = 0x6978, kMsRec = 14, kSplitRec = 10, kSampleRec = 14, kDwgsFirst = 77, kDwgsStride = 0x3FC;
constexpr uint32_t kPcmWords = 0x200000;
constexpr int kTplWords = 57;
constexpr double kFs = 31250.0;

struct Ctx {
    char* err; size_t n; bool bad = false;
    bool fail(const char* fmt, ...) {
        if (!bad) { va_list a; va_start(a, fmt); vsnprintf(err, n, fmt, a); va_end(a); bad = true; }
        return false;
    }
};

uint8_t* read_file(const char* path, size_t want, size_t* got) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0 || (want && (size_t)len != want)) { fclose(f); if (got) *got = len < 0 ? 0 : (size_t)len; return nullptr; }
    uint8_t* b = (uint8_t*)malloc(len ? (size_t)len : 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = nullptr; }
    fclose(f);
    if (got) *got = (size_t)len;
    return b;
}

// Directory listing, sorted by name like Python's sorted(glob()).
struct Dir { char names[256][256]; int n = 0; };
int cmp_name(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }
void list_dir(const char* dir, Dir& d) {
    DIR* h = opendir(dir);
    if (!h) return;
    while (dirent* e = readdir(h)) {
        if (e->d_name[0] == '.' || d.n >= 256 || strlen(e->d_name) >= 256) continue;
        strcpy(d.names[d.n++], e->d_name);
    }
    closedir(h);
    qsort(d.names, (size_t)d.n, sizeof d.names[0], cmp_name);
}
bool ends_ci(const char* s, const char* ext) {
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !strcasecmp(s + a - b, ext);
}
const char* find_ci(const Dir& d, const char* name) {
    for (int i = 0; i < d.n; ++i) if (!strcasecmp(d.names[i], name)) return d.names[i];
    return nullptr;
}
long fsize(const char* dir, const char* name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    struct stat st; return stat(p, &st) ? -1 : (long)st.st_size;
}
uint8_t* read_in(const char* dir, const char* name, size_t want, size_t* got = nullptr) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    return read_file(p, want, got);
}

inline int s8(uint8_t v) { return v >= 128 ? v - 256 : v; }
inline int s16(long v) { v &= 0xFFFF; return v & 0x8000 ? (int)(v - 0x10000) : (int)v; }
inline long floordiv(long a, long b) { long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) --q; return q; }

// The OS image with the content checks of m1_fx_tables._Img.
struct Img {
    const uint8_t* b; Ctx* c;
    const uint8_t* lin(uint32_t a, uint32_t n) {
        if (a < kBase || a - kBase + n > kOsSize) { c->fail("address %05X outside the OS image", a); return nullptr; }
        return b + (a - kBase);
    }
    uint8_t u8(uint32_t a) { const uint8_t* p = lin(a, 1); return p ? p[0] : 0; }
    uint16_t u16(uint32_t a) { const uint8_t* p = lin(a, 2); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
    uint16_t ds16(uint32_t off) { return u16(kDS + off); }
    void ds16s(uint32_t off, int n, int* out) { for (int i = 0; i < n; ++i) out[i] = ds16(off + 2 * i); }
    static int hexbytes(const char* hex, uint8_t* out) {
        int n = 0;
        for (; hex[0] && hex[1]; hex += 2) { unsigned v; sscanf(hex, "%2x", &v); out[n++] = (uint8_t)v; }
        return n;
    }
    void expect(uint32_t a, const char* hex, const char* what) {
        uint8_t w[16]; int n = hexbytes(hex, w);
        const uint8_t* p = lin(a, (uint32_t)n);
        if (p && memcmp(p, w, (size_t)n)) c->fail("not a Korg M1 Rev 19 OS: %s", what);
    }
    void fx(uint32_t off, const char* hex, const char* what) { expect(kFX + off, hex, what); }
    uint16_t imm16(uint32_t off, const char* op, const char* what) {
        uint8_t w[8]; int n = hexbytes(op, w);
        const uint8_t* p = lin(kFX + off, (uint32_t)n);
        if (p && memcmp(p, w, (size_t)n)) { c->fail("not a Korg M1 Rev 19 OS: %s", what); return 1; }
        return u16(kFX + off + (uint32_t)n);
    }
    uint8_t imm8(uint32_t off, const char* op, const char* what) {
        uint8_t w[8]; int n = hexbytes(op, w);
        const uint8_t* p = lin(kFX + off, (uint32_t)n);
        if (p && memcmp(p, w, (size_t)n)) { c->fail("not a Korg M1 Rev 19 OS: %s", what); return 1; }
        return u8(kFX + off + (uint32_t)n);
    }
};

struct Tpl {
    Img* m; int offs[34];
    uint64_t w[34][kTplWords];
    bool init() {
        int ptrs[34]; m->ds16s(0x836C, 34, ptrs);
        for (int t = 0; t < 34; ++t) offs[t] = m->ds16((uint32_t)ptrs[t]);
        for (int t = 0; t < 33; ++t)
            if (offs[t] % 0x15C || offs[t] >= 22 * 0x15C) return m->c->fail("not a Korg M1 Rev 19 OS: descriptor of type %d", t);
        for (int t = 0; t < 34; ++t) {
            const uint8_t* raw = m->lin(kTPL + (uint32_t)offs[t], 6 * kTplWords);
            if (!raw) return false;
            for (int k = 0; k < kTplWords; ++k) {
                uint64_t v = 0;
                for (int j = 5; j >= 0; --j) v = v << 8 | raw[6 * k + j];
                w[t][k] = v;
            }
        }
        return true;
    }
    const uint64_t* operator()(int t) const { return w[t]; }
};

double coef(uint64_t w) {
    unsigned c = (unsigned)(w >> 16) & 0x3FF;
    return (c & 0x200 ? -(double)(c & 0x1FF) : (double)(c & 0x1FF)) / 512.0;
}
int16_t dist(const uint64_t* T, int src, int dst) { return (int16_t)(((T[dst] & 0x7FFF) - (T[src] & 0x7FFF)) & 0x7FFF); }

unsigned q15mul(long a, long b) {
    long long p = (long long)s16(a) * (long long)s16(b);
    uint64_t u = (uint32_t)p;
    return (unsigned)(((u << 1) >> 16) & 0xFFFF);
}

double comb_feedback(long W, int sign_byte, const int* pow_tab, int rt, long k1ba1) {
    if (rt > 0x62) rt = 0x62;
    long long q = (W / (rt + 2)) & 0xFFFF;
    long long P = q * k1ba1;
    long ax = (long)((P >> 8) & 0xFFFF);
    int n = (int)((P >> 24) & 0xFF);
    ax >>= 1;
    long cx = ax;
    ax >>= 1;
    ax /= 24;
    ax = (ax - 0xAAA) & 0xFFFF;
    long dx = q15mul(ax, cx);
    dx = q15mul((dx + 0x2000) & 0xFFFF, cx);
    dx = q15mul((dx - 0x4000) & 0xFFFF, cx);
    dx = (dx + 0x4000) & 0xFFFF;
    if (n > 6) n = 6;                                     // pow table has 7 entries (never exceeded on Rev 19)
    uint64_t p = ((uint64_t)dx * (uint64_t)pow_tab[n]) & 0xFFFFFFFFu;
    dx = (long)(((p << 1) >> 16) & 0xFFFF);
    long mag = (16 * dx) / 0x1EF; if (mag > 511) mag = 511;
    bool negative = sign_byte < 0x80;
    return (negative ? -(double)mag : (double)mag) / 512.0;
}

double comb_damp(long Wd, int hd, long k1ef) {
    if (hd > 100) hd = 100;
    long dx = (Wd * k1ef) / 0x200;
    return (double)(((dx * hd) / 100) >> 5) / 512.0;
}

void fill_fb_damp(float fb[4][98], float damp[4][100], int combFirst, const int* wf, const int* wd,
                  const uint8_t* sign, const int* pw, long k1ba1, long k1ef) {
    for (int k = 0; k < 4; ++k) {
        int c = combFirst + k;
        for (int r = 0; r < 98; ++r) fb[k][r] = (float)comb_feedback(wf[c], sign[c], pw, r, k1ba1);
        for (int h = 0; h < 100; ++h) damp[k][h] = (float)comb_damp(wd[c], h, k1ef);
    }
}

bool fx_tables(const uint8_t* os, m1::FxTables& F, Ctx& c) {
    Img m{os, &c};
    m.fx(0x0A64, "81ff7dc6", "edit buffer compare (C67D)");
    Tpl* tp = (Tpl*)calloc(1, sizeof(Tpl));
    if (!tp) return c.fail("out of memory");
    tp->m = &m;
    if (!tp->init()) { free(tp); return false; }
    const Tpl& tpl = *tp;
    double G = m.imm16(0x0990, "b8", "MDE factor") / 512.0;

    // 1. Reverbs 0-5
    m.fx(0x098C, "8b97d684", "damp table ref DS:84D6");
    m.fx(0x09D9, "8b871685", "feedback table ref DS:8516");
    m.fx(0x0A2F, "8b8fc884", "power table ref DS:84C8");
    m.fx(0x0A47, "8a8f5685", "sign table ref DS:8556");
    long k1ef = m.imm16(0x0990, "b8", "MDE factor 0x1EF");
    long k1ba1 = m.imm16(0x09EE, "b9", "feedback scale 0x1BA1");
    if (k1ef != 0x1EF && !c.bad) { free(tp); return c.fail("not a Korg M1 Rev 19 OS: MDE factor"); }
    int pw[7], wd[32], wf[32]; uint8_t sign[32];
    m.ds16s(0x84C8, 7, pw); m.ds16s(0x84D6, 32, wd); m.ds16s(0x8516, 32, wf);
    { const uint8_t* p = m.lin(kDS + 0x8556, 32); if (p) memcpy(sign, p, 32); else memset(sign, 0, 32); }
    int base[6][3], ers[6][3];
    for (int t = 0; t < 6; ++t) { m.ds16s(0x8576 + 6 * t, 3, base[t]); m.ds16s(0x85A2 + 6 * t, 3, ers[t]); }
    m.fx(0x0B0F, "81c57685", "pre-delay base table ref DS:8576");
    m.fx(0x0AA3, "81c5a285", "ER level table ref DS:85A2");
    for (int t = 0; t < 6; ++t) {
        const uint64_t* T = tpl(t);
        m1::FxRevType& R = F.rev[t];
        const int g[4] = {11, 16, 37, 49}, co[8] = {32, 44, 34, 46, 35, 47, 36, 48};
        for (int k = 0; k < 4; ++k) R.g[k] = (float)coef(T[g[k]]);
        for (int k = 0; k < 8; ++k) R.comb_out[k] = (float)coef(T[co[k]]);
        const int ap[4][2] = {{14, 13}, {18, 17}, {39, 36}, {51, 48}}, cd[4][2] = {{23, 19}, {26, 22}, {27, 25}, {30, 28}};
        const int tl[4][2] = {{23, 31}, {24, 33}, {29, 34}, {32, 35}}, tr[4][2] = {{23, 43}, {26, 45}, {29, 46}, {30, 47}};
        for (int k = 0; k < 4; ++k) {
            R.ap_d[k] = dist(T, ap[k][0], ap[k][1]); R.comb_d[k] = dist(T, cd[k][0], cd[k][1]);
            R.tap_l[k] = dist(T, tl[k][0], tl[k][1]); R.tap_r[k] = dist(T, tr[k][0], tr[k][1]);
        }
        R.g_in = 1.0f;
        fill_fb_damp(R.fb, R.damp, 4 * t, wf, wd, sign, pw, k1ba1, k1ef);
        R.base_ap = (int16_t)base[t][0]; R.base_erl = (int16_t)base[t][1]; R.base_err = (int16_t)base[t][2]; R.base_erc = 0;
        R.er_scale[0] = (float)(ers[t][2] / 512.0); R.er_scale[1] = (float)(ers[t][0] / 512.0); R.er_scale[2] = (float)(ers[t][1] / 512.0);
    }

    // 2. Delay damping
    long c_e1 = m.imm16(0x05F7, "b9", "damp b range"), c_1ef = m.imm16(0x05FC, "b8", "damp b scale");
    long c_e1b = m.imm16(0x0601, "b9", "damp b divisor"), c_1ef2 = m.imm16(0x0641, "b9", "damp a scale");
    long c_12c = m.imm16(0x0646, "b9", "damp a divisor");
    if (!c_e1b || !c_12c) { free(tp); return c.fail("not a Korg M1 Rev 19 OS: delay damping"); }
    for (int h = 0; h < 100; ++h) {
        F.dly_a[h] = (float)(floordiv(h * c_1ef2, c_12c) / 512.0);
        F.dly_b[h] = (float)(-floordiv(c_1ef * (c_e1 - h), c_e1b) / 512.0);
    }

    // 3. LFO
    m.fx(0x0671, "ac32e43d6300", "LFO speed patcher");
    long seg1 = m.imm16(0x0674, "3d", "speed segment 1"), mul2 = m.imm16(0x068B, "b9", "speed step 2");
    long off2 = m.imm16(0x0690, "05", "speed base 2"), seg2 = m.imm16(0x0683, "3d", "speed segment 2");
    long mul3 = m.imm16(0x0696, "b9", "speed step 3"), off3 = m.imm16(0x069E, "05", "speed base 3");
    long num = m.imm16(0x06A1, "b9", "speed numerator"), den = m.imm16(0x06A6, "b9", "speed denominator");
    if (!den) den = 1;
    for (long v = 0; v < 217; ++v) {
        long a = v <= seg1 ? (v + 1) * 3 : v <= seg2 ? (v - seg1) * mul2 + off2 : (v - seg2) * mul3 + off3;
        F.lfo_inc[v] = (int32_t)floordiv(a * num, den);
    }
    // assumed waves (m1_make_blob.fill_assumed)
    memset(F.wt, 0, sizeof F.wt);
    double sine[128], tri[128], w2[128];
    for (int i = 0; i < 128; ++i) {
        double s = sin(M_PI * (i + 0.5) / 128), s2 = sin(2 * M_PI * (i + 0.5) / 128);
        sine[i] = nearbyint(511 * (s * s)) / 512; w2[i] = nearbyint(511 * (s2 * s2)) / 512;
        tri[i] = 8 * (i < 127 - i ? i : 127 - i) / 512.0;
    }
    for (int i = 0; i < 130; ++i) { F.wt[i] = (float)sine[i % 128]; F.wt[0x102 + i] = (float)tri[i % 128]; F.wt2[i] = (float)w2[i % 128]; }

    // 4. factory effect parameters: content check only
    { const uint8_t* f = m.lin(kDS + 0x7502, 34 * 8);
      bool any = false; if (f) for (int i = 0; i < 8; ++i) any |= f[8 * 33 + i] != 0;
      if (f && (any || f[0] > 97)) { free(tp); return c.fail("not a Korg M1 Rev 19 OS: factory effect parameters"); } }

    // 5. Early reflections 6-8
    m.fx(0x0CCC, "8b8d4688", "ER tap table ref DS:8846");
    int t_fw[24]; m.ds16s(0x8846, 24, t_fw);
    double k_ms = (double)m.imm16(0x0CC2, "b9", "ER time scale") / (double)m.imm16(0x0CC7, "b9", "ER time div");
    double k_t = m.imm16(0x0CD2, "b9", "ER tap divisor");
    double ap_c[4] = {(double)m.imm16(0x0D05, "b9", "ER AP0 slope"), (double)m.imm16(0x0D26, "b9", "ER AP1 slope"),
                      (double)m.imm16(0x0D47, "b9", "ER AP2 slope"), (double)m.imm16(0x0D70, "b8", "ER AP3 slope")};
    double k_ap = m.imm16(0x0D0A, "b9", "ER AP divisor");
    double ms = kFs / 1000.0;
    double erT[25], erA[4];
    erT[0] = 0.0;
    for (int i = 0; i < 24; ++i) erT[i + 1] = t_fw[i] * k_ms / k_t / ms;
    for (int i = 0; i < 4; ++i) erA[i] = ap_c[i] * k_ms / k_ap;
    for (int i = 0; i < 25; ++i) F.er_T[i] = (float)erT[i];
    for (int i = 0; i < 4; ++i) F.er_A[i] = (float)erA[i];
    for (int t = 6; t <= 8; ++t) {
        const uint64_t* T = tpl(t);
        m1::FxErType& E = F.er[t - 6];
        const int g[4] = {37, 41, 45, 50};
        for (int k = 0; k < 4; ++k) E.g[k] = (float)(coef(T[g[k]]) * G);
        E.tg[0] = (float)coef(T[11]);
        for (int k = 1; k < 25; ++k) E.tg[k] = (float)coef(T[12 + k]);
    }

    // 6. Tremolo shape, exciter blend
    F.trem_k = (float)((double)m.imm16(0x1160, "b9", "tremolo shape") / (double)m.imm16(0x1165, "b9", "shape div") / 512.0);
    F.exc_k = (float)(-(double)m.imm16(0x057F, "b9", "exciter scale") / (double)m.imm16(0x0584, "b9", "exciter div") / 512.0);

    // 7. Rotary speaker
    {
        const uint64_t* T = tpl(24);
        long slow_k = m.imm16(0x0F4C, "b9", "horn slow slope"), slow_0 = m.imm16(0x0F51, "05", "horn slow base");
        long fast_k = m.imm16(0x0F5F, "b9", "horn fast slope"), fast_0 = m.imm16(0x0F64, "05", "horn fast base");
        long dv = m.imm16(0x0F68, "b9", "horn div"); if (!dv) dv = 1;
        double tick = m.u8(0xE05C5) | m.u8(0xE05C9) << 8;
        m.expect(0xE05C0, "b0b0e60eb0", "tick timer setup");
        double tick_samples = tick / 2e6 * kFs;
        int horn_ticks = m.imm8(0x0FB0, "24", "horn step mask") + 1;
        int drum_ticks = m.imm8(0x0FAC, "3c", "drum step period");
        m1::FxRotary& r = F.rot;
        r.c20 = (float)coef(T[3]); r.c24 = (float)coef(T[11]); r.c28 = (float)coef(T[6]);
        r.c30 = (float)coef(T[10]); r.c38 = (float)coef(T[7]); r.c40 = (float)coef(T[4]); r.c44 = (float)coef(T[12]);
        r.c2c = (float)(1.0 - coef(T[12]) * (1.0 + coef(T[11])));            // assumed (fill_assumed)
        r.h_fb_tap = (int16_t)(dist(T, 4, 6) + 1); r.h_w = 0; r.h_fix = (int16_t)(dist(T, 4, 11) + 1);
        r.h_base = dist(T, 4, 18); r.d_w = 0; r.d_base = dist(T, 12, 27);
        const int ml[5][2] = {{39, 41}, {39, 42}, {39, 43}, {32, 38}, {32, 40}};
        const int mr[5][2] = {{39, 49}, {39, 50}, {39, 51}, {32, 46}, {32, 48}};
        for (int k = 0; k < 5; ++k) { r.micL[k] = dist(T, ml[k][0], ml[k][1]); r.micR[k] = dist(T, mr[k][0], mr[k][1]); }
        r.g120 = (float)coef(T[42]); r.g124 = (float)coef(T[43]); r.g130 = (float)coef(T[39]); r.g134 = (float)coef(T[41]);
        r.accel[0] = (int16_t)nearbyint(horn_ticks * tick_samples);
        r.accel[1] = (int16_t)nearbyint(drum_ticks * tick_samples);
        for (int s = -10; s <= 10; ++s) {
            r.slow[s + 10] = (int16_t)floordiv(slow_k * s + slow_0, dv);
            r.fast[s + 10] = (int16_t)floordiv(fast_k * s + fast_0, dv);
        }
        r.depth_k[0] = 0.99f; r.depth_k[1] = 0.99f;                            // assumed
        r.depth_k[2] = (float)(99 * 0.512); r.depth_k[3] = (float)(99 * 0.512);
    }

    // 8. Symphonic ensemble (offsets assumed, see fill_assumed)
    {
        m1::FxSym& s = F.sym;
        for (int k = 0; k < 3; ++k) s.mix2[k] = 0.1f;
        const int bl[3] = {300 + 154, 300 + 157, 300}, br[3] = {300 + 94, 300 + 217, 300 + 60};
        for (int k = 0; k < 3; ++k) { s.baseL[k] = (int16_t)bl[k]; s.baseR[k] = (int16_t)br[k]; }
        s.inc[0] = (int16_t)m.imm16(0x1290, "b8", "sym LFO1 inc"); s.inc[1] = (int16_t)m.imm16(0x1296, "b8", "sym LFO2 inc");
        s.off[0] = 0xA000 >> 9; s.off[1] = 0xC000 >> 9; s.off[2] = 0xE000 >> 9;
    }

    // 9. Delay/Hall, Delay/Room reverb halves
    m.fx(0x0BD0, "81c59a85", "combo base table ref DS:859A");
    int cb[4]; m.ds16s(0x859A, 4, cb);
    int cidx[2] = {m.imm16(0x0C3B, "bb", "Delay/Hall comb index"), m.imm16(0x0C66, "bb", "Delay/Room comb index")};
    for (int t = 25; t <= 26; ++t) {
        const uint64_t* T = tpl(t);
        m1::FxComboRev& R = F.crev[t - 25];
        R.g[0] = (float)coef(T[10]); R.g[1] = (float)coef(T[36]); R.g[2] = (float)coef(T[15]);
        R.ger[0] = (float)coef(T[40]); R.ger[1] = (float)coef(T[41]);
        const int co[4] = {31, 33, 34, 35};
        for (int k = 0; k < 4; ++k) R.cout[k] = (float)coef(T[co[k]]);
        const int ap[3][2] = {{13, 12}, {38, 37}, {17, 16}};
        const int cd[4][2] = {{22, 18}, {23, 21}, {28, 24}, {29, 27}}, ct[4][2] = {{22, 30}, {23, 32}, {26, 33}, {31, 34}};
        for (int k = 0; k < 3; ++k) R.ap_d[k] = dist(T, ap[k][0], ap[k][1]);
        for (int k = 0; k < 4; ++k) { R.comb_d[k] = dist(T, cd[k][0], cd[k][1]); R.comb_tap[k] = dist(T, ct[k][0], ct[k][1]); }
        double e[8]; for (int k = 0; k < 8; ++k) e[k] = coef(T[2 + k]);       // assumed pre-filter (fill_assumed)
        R.eq[0] = (float)e[0]; R.eq[1] = (float)(e[2] * e[3]); R.eq[2] = (float)e[4]; R.eq[3] = (float)(e[6] * e[7]);
        R.eq[4] = (float)(1.0 + e[1]); R.eq[5] = (float)(1.0 + e[5]);
        R.g_in = 1.0f;
        int ci = cidx[t - 25];
        if (ci < 0 || ci + 4 > 32) { free(tp); return c.fail("not a Korg M1 Rev 19 OS: comb index"); }
        fill_fb_damp(R.fb, R.damp, ci, wf, wd, sign, pw, k1ba1, k1ef);
        R.base_ap = (int16_t)cb[2 * (t - 25)]; R.base_b = (int16_t)cb[2 * (t - 25) + 1]; R.base_c = 0;
    }

    // 10. Delay/E.Ref ER half
    {
        const uint64_t* T = tpl(27);
        double g3 = coef(T[38]) * G;
        m1::FxComboEr& E = F.cer;
        E.g[0] = (float)(coef(T[30]) * G); E.g[1] = (float)(coef(T[34]) * G); E.g[2] = (float)g3; E.g[3] = (float)g3;
        E.tg[0] = (float)coef(T[4]);
        for (int k = 1; k < 25; ++k) E.tg[k] = (float)coef(T[5 + k]);
        for (int k = 0; k < 25; ++k) E.T[k] = (float)erT[k];
        E.A[0] = (float)erA[0]; E.A[1] = (float)erA[1]; E.A[2] = (float)erA[2]; E.A[3] = (float)erA[2];
        E.g_in = (float)(coef(T[2]) * coef(T[3]));
    }

    // EQ tables (m1_make_blob: REV19 eq_k / eq_gain, linear image offsets)
    for (int i = 0; i < 5; ++i) F.eq_k[i] = (int16_t)s16(os[0x175C0 + 0x8966 + 2 * i] | os[0x175C0 + 0x8966 + 2 * i + 1] << 8);
    for (int i = 0; i < 13; ++i) F.eq_gain[i] = (int16_t)s16(os[0x175C0 + 0x8970 + 2 * i] | os[0x175C0 + 0x8970 + 2 * i + 1] << 8);
    free(tp);
    return !c.bad;
}

inline uint16_t u16at(const uint8_t* b, uint32_t o) { return (uint16_t)(b[o] | b[o + 1] << 8); }

}  // namespace

void release(Rom* r) {
    if (!r) return;
    free(r->pcm);
    free(r);
}

Rom* load(const char* dir, char* err, size_t errLen) {
    Ctx c{err, errLen};
    if (errLen) err[0] = 0;
    Dir* d = (Dir*)calloc(1, sizeof(Dir));
    Rom* R = (Rom*)calloc(1, sizeof(Rom));
    uint8_t* os = nullptr; uint8_t* coefRom = nullptr; uint8_t* syx = nullptr;
    if (!d || !R) { c.fail("out of memory"); goto out; }
    list_dir(dir, *d);
    if (!d->n) { c.fail("no ROM files found"); goto out; }

    // ---- PCM: IC36..IC39 low bytes, IC40..IC43 high bytes
    R->pcm = (int16_t*)malloc(kPcmWords * 2);
    if (!R->pcm) { c.fail("out of memory"); goto out; }
    R->pcmWords = kPcmWords;
    for (int k = 0; k < 4; ++k) {
        char lo_n[16], hi_n[16];
        snprintf(lo_n, sizeof lo_n, "IC%d.bin", 36 + k); snprintf(hi_n, sizeof hi_n, "IC%d.bin", 40 + k);
        const char* lo_f = find_ci(*d, lo_n); const char* hi_f = find_ci(*d, hi_n);
        if (!lo_f || !hi_f) { c.fail("missing IC%d.bin / IC%d.bin (PCM)", 36 + k, 40 + k); goto out; }
        uint8_t* lo = read_in(dir, lo_f, 0x80000); uint8_t* hi = read_in(dir, hi_f, 0x80000);
        if (!lo || !hi) { free(lo); free(hi); c.fail("IC%d/IC%d: expected 512 KB each", 36 + k, 40 + k); goto out; }
        uint8_t* out = (uint8_t*)R->pcm + k * 0x100000;
        for (uint32_t i = 0; i < 0x80000; ++i) { out[2 * i] = lo[i]; out[2 * i + 1] = hi[i]; }
        free(lo); free(hi);
    }

    // ---- coefficient ROM
    {
        const char* f = find_ci(*d, "IC30.bin"); if (!f) f = find_ci(*d, "IC26.bin"); if (!f) f = find_ci(*d, "IC20.bin");
        if (!f) { c.fail("missing IC30.bin (coefficient ROM; IC20/IC26 also do)"); goto out; }
        coefRom = read_in(dir, f, 0x10000);
        if (!coefRom) { c.fail("%s: expected 64 KB", f); goto out; }
        for (int ph = 0; ph < 4096; ph += 97) {
            const uint8_t* r = coefRom + ph * 8;
            int s = (s8(r[0]) + s8(r[1])) + s8(r[2]) * 2 + (r[3] + r[4]) * 8 + s8(r[5]) * 2 + (s8(r[6]) + s8(r[7]));
            if (s != 1226) { c.fail("%s is not the M1 coefficient ROM", f); goto out; }
        }
    }

    // ---- OS: two 64 KB halves, even/odd found by content
    {
        int cand[256], nc = 0;
        for (int i = 0; i < d->n; ++i) {
            const char* nm = d->names[i];
            if (!ends_ci(nm, ".bin") || fsize(dir, nm) != 0x10000) continue;
            if (!strcasecmp(nm, "ic20.bin") || !strcasecmp(nm, "ic26.bin") || !strcasecmp(nm, "ic30.bin")) continue;
            cand[nc++] = i;
        }
        uint8_t* halves[256] = {nullptr};
        for (int i = 0; i < nc; ++i) halves[i] = read_in(dir, d->names[cand[i]], 0x10000);
        os = (uint8_t*)malloc(kOsSize);
        bool found = false;
        for (int a = 0; os && a < nc && !found; ++a)
            for (int b = 0; b < nc && !found; ++b) {
                if (a == b || !halves[a] || !halves[b]) continue;
                for (uint32_t i = 0; i < 0x10000; ++i) { os[2 * i] = halves[a][i]; os[2 * i + 1] = halves[b][i]; }
                uint32_t o = 0xF75C0 - kBase + kMsTable;
                found = !memcmp(os + o + 4, "Piano     ", 10) && !memcmp(os + o + 14 + 4, "E.Piano 1 ", 10);
            }
        for (int i = 0; i < nc; ++i) free(halves[i]);
        if (!found) { c.fail("no original M1 OS: need the two 64 KB halves of the Rev 19 OS"); goto out; }
    }

    // ---- factory preload: an M1 ALL DATA dump
    {
        size_t len = 0;
        for (int i = 0; i < d->n && !syx; ++i) {
            if (!ends_ci(d->names[i], ".syx")) continue;
            uint8_t* b = read_in(dir, d->names[i], 0, &len);
            if (b && len > 8 && b[0] == 0xF0 && b[1] == 0x42 && (b[2] & 0xF0) == 0x30 && b[3] == 0x19 && b[4] == 0x50) syx = b;
            else free(b);
        }
        if (!syx) { c.fail("no M1 ALL DATA dump (*.syx)"); goto out; }
        size_t end = 8; while (end < len && syx[end] != 0xF7) ++end;
        if (end >= len) { c.fail("ALL DATA dump has no end"); goto out; }
        size_t groups = (end - 8) / 8, ulen = groups * 7;
        if (ulen < 13261 + 143 * 100) { c.fail("ALL DATA dump too short"); goto out; }
        uint8_t* u = (uint8_t*)malloc(ulen);
        if (!u) { c.fail("out of memory"); goto out; }
        for (size_t g = 0; g < groups; ++g) {
            const uint8_t* s = syx + 8 + 8 * g; uint8_t msb = s[0];
            for (int j = 0; j < 7; ++j) u[7 * g + j] = (uint8_t)(s[1 + j] | (((msb >> j) & 1) << 7));
        }
        memcpy(R->global, u, 21);
        memcpy(R->kits, u + 21, 840);
        memcpy(R->combis, u + 861, 12400);
        memcpy(R->programs, u + 13261, 14300);
        free(u);
    }

    {
        const uint8_t* img = os;
        auto ds = [&](uint32_t off) { return img + 0x175C0 + off; };
        auto u16s = [&](uint32_t off, int n, uint16_t* out) { for (int i = 0; i < n; ++i) out[i] = u16at(img, off + 2 * i); };
        // sanity checks of m1_make_blob.main
        int16_t eqk0 = (int16_t)u16at(img, 0x175C0 + 0x8966), eqk1 = (int16_t)u16at(img, 0x175C0 + 0x8968), eqk2 = (int16_t)u16at(img, 0x175C0 + 0x896A);
        if (eqk0 != -7790 || eqk1 != -7407 || eqk2 != -6690) { c.fail("shelf table not where Rev 19 has it"); goto out; }
        u16s(0x175C0 + 0x8272, 101, R->pan101);
        if (R->pan101[0] != 0 || R->pan101[100] != 511 || R->pan101[50] != 360) { c.fail("balance table not where Rev 19 has it"); goto out; }
        u16s(0x10C22, 256, R->egRise); u16s(0x10E22, 256, R->egFall); u16s(0x116AB, 256, R->velCurve);
        u16s(0x10B5A, 100, R->mgDelay); u16s(0x109CA, 100, R->mgIntP); u16s(0x10A92, 100, R->mgIntF);
        u16s(0x175C0 + 0x738E, 100, R->mgFreq); u16s(0x175C0 + 0x0616, 153, R->pitchS); u16s(0x175C0 + 0x0748, 128, R->pitchF);
        u16s(0x175C0 + 0x0854, 14, R->panReg); u16s(0x175C0 + 0x7230, 100, R->delayStart);
        memcpy(R->scale, img + 0x175C0 + 0x0848, 12);
        if (R->velCurve[252] != 15629 || R->pitchS[128] != 4096 || R->mgIntP[99] != 8191 ||
            R->scale[0] != 0 || R->scale[1] != -38 || R->scale[2] != 5 || R->egRise[0] == 0) {
            c.fail("OS tables not where Rev 19 has them"); goto out;
        }

        // multisounds
        int ns = 0;
        for (int i = 0; i < 100; ++i) {
            const uint8_t* rec = ds(kMsTable + kMsRec * i);
            uint16_t ptr = u16at(rec, 0); uint8_t kind = rec[2], nsplit = rec[3];
            bool dwgs = kind == 2;
            uint32_t off = dwgs ? (uint32_t)((i - kDwgsFirst) * kDwgsStride) : 0;
            m1::Multisound& M = R->ms[i];
            M.first = (uint16_t)ns; M.count = nsplit; M.dwgs = dwgs ? 1 : 0;
            int nl = 10; while (nl > 0 && (rec[4 + nl - 1] == ' ' || rec[4 + nl - 1] == 0)) --nl;
            for (int j = 0; j < nl; ++j) M.name[j] = rec[4 + j] == '"' ? '\'' : (char)rec[4 + j];
            M.name[nl] = 0;
            for (int k = 0; k < nsplit; ++k) {
                if (ns >= 640) { c.fail("too many splits"); goto out; }
                const uint8_t* s = ds(ptr + kSplitRec * k);
                const uint8_t* r = ds(u16at(s, 0));
                uint32_t start = r[2] | r[3] << 8 | r[4] << 16, loop = r[6] | r[7] << 8 | r[8] << 16;
                uint32_t end = r[10] | r[11] << 8 | r[12] << 16; uint8_t flags = r[9];
                uint32_t bank = (end >> 20) & 0xF;
                if (bank > 1) { c.fail("multisound %d split %d: not internal PCM", i, k); goto out; }
                auto A = [&](uint32_t x) { return ((bank << 20) | (x & 0xFFFFF)) + off; };
                m1::Split& S = R->splits[ns++];
                S.start = A(start); S.loop = A(loop); S.end = A(end);
                S.tune128 = (int16_t)u16at(r, 0);
                S.top = s[2]; S.pan = s[3]; S.root = s[5] & 0x7F;
                S.flags = (uint8_t)((flags & 4 ? 1 : 0) | (s[5] & 0x80 ? 2 : 0));
                S.tune = (int8_t)s[4]; S.level = (int8_t)s[6]; S.cutoff = (int8_t)s[7]; S.decay = (int8_t)s[8];
                if (!(S.start <= S.end && S.end < kPcmWords && S.loop <= S.end)) { c.fail("multisound %d split %d: out of range", i, k); goto out; }
            }
        }
        R->nSplits = ns;

        // drum sounds
        for (int i = 0; i < 44; ++i) {
            const uint8_t* r = ds(0x5B9E + kSampleRec * i);
            uint32_t start = r[2] | r[3] << 8 | r[4] << 16, loop = r[6] | r[7] << 8 | r[8] << 16;
            uint32_t end = r[10] | r[11] << 8 | r[12] << 16, bank = (end >> 20) & 0xF;
            m1::DrumSound& D = R->drums[i + 1];
            D.start = (bank << 20) | (start & 0xFFFFF); D.loop = (bank << 20) | (loop & 0xFFFFF);
            D.end = (bank << 20) | (end & 0xFFFFF); D.flags = r[9] & 4 ? 1 : 0; D.tune128 = (int16_t)u16at(r, 0);
            if (bank > 1 || !(D.start <= D.end && D.end < kPcmWords)) { c.fail("drum sound %d: out of range", i + 1); goto out; }
            if (D.flags && !(D.loop <= D.end)) { c.fail("drum sound %d: loop past the end", i + 1); goto out; }
        }
        if (memcmp(ds(0x6F48), "Kick 1    ", 10)) { c.fail("drum-sound names not where Rev 19 has them"); goto out; }
        R->nDrums = 44;

        // coefficient ROM tables
        memcpy(R->kernel, coefRom, 0x8000);
        for (int i = 0; i < 4096; ++i) {
            R->vdf1k[i] = (uint16_t)(coefRom[0xA000 + i] | coefRom[0xB000 + i] << 8);
            R->vdf2k[i] = (uint16_t)(coefRom[0xE000 + i] | coefRom[0xF000 + i] << 8);
            R->vdf1t0[i] = (int16_t)(coefRom[0x8000 + i] | coefRom[0x9000 + i] << 8);
            R->vdf2t0[i] = (int16_t)(coefRom[0xC000 + i] | coefRom[0xD000 + i] << 8);
        }

        if (!fx_tables(img, R->fx, c)) goto out;
    }

out:
    free(d); free(os); free(coefRom); free(syx);
    if (c.bad) { release(R); return nullptr; }
    return R;
}

}  // namespace m1load
