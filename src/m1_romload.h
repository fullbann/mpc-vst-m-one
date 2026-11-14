// SPDX-License-Identifier: GPL-3.0-or-later
// m1_romload -- reads the user's own Korg M1 ROM set at run time (C++ port of tools/m1_extract, MIT, Michi71).
// Every table the engine needs is read or computed from the user's files; nothing from the ROMs is in the plugin.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "m1_engine/m1_engine.h"
#include "m1_engine/m1_fx.h"

namespace m1load {

struct Rom {
    int16_t* pcm;                    // 2 M words (4 MiB), separate allocation
    uint32_t pcmWords;
    m1::Multisound ms[100];
    m1::Split splits[640];
    int nSplits;
    m1::DrumSound drums[45];         // index 0 unused, 1..nDrums
    int nDrums;
    uint8_t kernel[32768];
    uint16_t vdf1k[4096], vdf2k[4096];
    int16_t vdf1t0[4096], vdf2t0[4096];
    uint16_t egRise[256], egFall[256], velCurve[256];
    uint16_t mgFreq[100], mgDelay[100], mgIntP[100], mgIntF[100];
    uint16_t pitchS[153], pitchF[128], panReg[14], pan101[101], delayStart[100];
    m1::FxTables fx;
    uint8_t programs[14300], combis[12400], kits[840], global[21];
    int8_t scale[12];
};

// Reads the ROM set in dir: IC36..IC43.bin (PCM), IC30.bin (or IC20/IC26), the two 64 KB halves of the Rev 19 OS
// (any names) and an M1 ALL DATA dump (*.syx). Returns nullptr on failure, with a short reason in err.
Rom* load(const char* dir, char* err, size_t errLen);
void release(Rom* r);

}  // namespace m1load
