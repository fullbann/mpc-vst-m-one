#!/usr/bin/env python3
# Fullban M-One "Preview" bank: 10 original programs, written from scratch parameter by parameter
# (no factory program data used). Writes Fullban/Preview.m1b (programs only).
import os, struct, sys

OSC = dict(PEG_SL=0, PEG_AT=1, PEG_AL=2, PEG_DT=3, PEG_RT=4, PEG_RL=5, PEG_TVEL=6, PEG_LVEL=7,
           CUTOFF=8, VDF_KC=9, VDF_KT=10, VDF_EGI=11, VDF_TKBD=12, VDF_TVEL=13, VDF_IVEL=14,
           VDF_AT=15, VDF_AL=16, VDF_DT=17, VDF_BP=18, VDF_ST=19, VDF_SL=20, VDF_RT=21, VDF_RL=22,
           LEVEL=23, VDA_KC=24, VDA_KT=25, VDA_VEL=26, VDA_TKBD=27, VDA_TVEL=28,
           VDA_AT=29, VDA_AL=30, VDA_DT=31, VDA_BP=32, VDA_ST=33, VDA_SL=34, VDA_RT=35,
           VDF_TKBD_SW=36, VDF_TVEL_SW=37, VDA_TKBD_SW=38, VDA_TVEL_SW=39)
MS = {n: i for i, n in enumerate(
    "Piano,E.Piano 1,E.Piano 2,Clav,Harpsicord,Organ 1,Organ 2,MagicOrgan,Guitar 1,Guitar 2,E.Guitar,Sitar 1,"
    "Sitar 2,A.Bass,Pick Bass,E.Bass,Fretless,SynthBass1,SynthBass2,Vibes,Bell,Tubular,BellRing,Karimba,KarimbaNT,"
    "SynMallet,Flute,Pan Flute,Bottles,Voices,Choir,Strings,Brass 1,Brass 2,Tenor Sax,Mute TP,Trumpet,TubaFlugel,"
    "DoubleReed,Koto Trem,BambooTrem,Rhythm,Lore,Lore NT,FlexaTone,WindBells,Pole,Pole NT,Block,Block NT,FingerSnap,"
    "Pop,Drop,Drop NT,Breath,Breath NT,Pluck,Pluck NT,Vibe Hit,VibeHit NT,Hammer,Metal Hit,MetalHitNT,Pick,"
    "Distortion,Dist NT,Bass Thumb,BasThumNT1,BasThumNT2,Wire,PanWave,PingWave,Fv Wave,Mv Wave,Voice Wave,"
    "VoiceWvNT1,VoiceWvNT2,DWGS E.P.1,DWGS E.P.2,DWGS E.P.3,DWGS Piano,DWGS Clav,DWGS Vibe,DWGS Bass1,DWGS Bass2,"
    "DWGS Bell,DWGS Orgn1,DWGS Orgn2,DWGS Voice,SquareWave,Digital 1,Saw Wave,Digital 2,25% Pulse,10% Pulse,"
    "Digital 3,Digital 4,Digital 5,DWGS TRI,DWGS Sine".split(","))}
FX = {n: i for i, n in enumerate(
    "Hall,Ensemble Hall,Concert Hall,Room,Large Room,Live Stage,Early Ref 1,Early Ref 2,Early Ref 3,Stereo Delay,"
    "Cross Delay,Chorus 1,Chorus 2,Flanger 1,Cross Flanger,Stereo Phaser,Phaser,Stereo Tremolo,Tremolo,Equalizer,"
    "Overdrive,Distortion,Exciter,Symphonic Ens,Rotary Speaker,Delay/Hall,Delay/Room,Delay/Early Ref,Delay/Delay,"
    "Delay/Chorus,Delay/Flanger,Delay/Phaser,Delay/Tremolo,No Effect".split(","))}

def b8(v):
    assert -128 <= v <= 255, v
    return v & 0xFF

def osc(**kw):
    """One oscillator block. Defaults: no pitch EG, open-ish filter, organ-style amp."""
    d = dict(PEG_SL=0, PEG_AT=0, PEG_AL=0, PEG_DT=0, PEG_RT=0, PEG_RL=0, PEG_TVEL=0, PEG_LVEL=0,
             CUTOFF=70, VDF_KC=60, VDF_KT=20, VDF_EGI=30, VDF_TKBD=0, VDF_TVEL=0, VDF_IVEL=30,
             VDF_AT=0, VDF_AL=99, VDF_DT=30, VDF_BP=60, VDF_ST=50, VDF_SL=40, VDF_RT=40, VDF_RL=0,
             LEVEL=85, VDA_KC=60, VDA_KT=0, VDA_VEL=40, VDA_TKBD=0, VDA_TVEL=0,
             VDA_AT=0, VDA_AL=99, VDA_DT=30, VDA_BP=90, VDA_ST=50, VDA_SL=85, VDA_RT=40,
             VDF_TKBD_SW=0, VDF_TVEL_SW=0, VDA_TKBD_SW=0, VDA_TVEL_SW=0)
    for k, v in kw.items():
        assert k in d, k
        d[k] = v
    out = bytearray(40)
    for k, v in d.items():
        out[OSC[k]] = b8(v)
    return bytes(out)

def fxblock(t1="No Effect", p1=(), t2="No Effect", p2=(), bal=(0, 0, 0, 0), serial=False):
    on = (0x03 if t1 != "No Effect" else 0) | (0x0C if t2 != "No Effect" else 0) | (0x10 if serial else 0)
    b = bytearray(25)
    b[0], b[1] = FX[t1], FX[t2]
    b[2:6] = bytes(bal)
    b[6], b[7] = 0, 0                       # Out 3/4 off
    b[8] = on
    for base, ps in ((9, p1), (17, p2)):
        for i, v in enumerate(ps):
            b[base + i] = b8(v)
    return bytes(b)

def u16(v):
    return [v & 0xFF, v >> 8]

def program(name, o1, o2=None, ms1="Piano", ms2=None, oct1=0, oct2=0, interval=0, detune=0, delay=0,
            mono=False, pmg=(0, 40, 0, 0), cmg=(0, 40, 0, 0), at=(0, 0, 0, 0, 0), js=(2, 0, 10, 0, 20, 0),
            fx=None):
    p = bytearray(143)
    nm = name.encode("ascii")[:10]
    p[0:10] = nm + b" " * (10 - len(nm))
    p[10] = 1 if o2 is not None else 0
    p[11] = 1 if mono else 0
    p[12], p[13] = MS[ms1], b8(oct1)
    p[14], p[15] = MS[ms2 or ms1], b8(oct2)
    p[16], p[17], p[18] = interval, b8(detune), delay
    p[19:23] = bytes(b8(v) for v in pmg)
    p[23:27] = bytes(b8(v) for v in cmg)
    p[27:32] = bytes(b8(v) for v in at)
    p[32:38] = bytes(b8(v) for v in js)
    p[38:63] = fx or fxblock()
    p[63:103] = o1
    p[103:143] = o2 if o2 is not None else o1
    return bytes(p)

# MG bits: 0x80 key sync, 0x20 OSC 1 on, 0x40 OSC 2 on, low 2 bits wave (0 tri).
VIB = (0x60, 46, 30, 8)     # gentle delayed vibrato on both oscillators

P = []

# 1. Dual Piano House: two pianos, the second an octave up and slightly detuned, bright and punchy.
P.append(program("DualPnoHse", ms1="Piano", ms2="Piano", oct1=0, oct2=1, detune=5,
    o1=osc(CUTOFF=80, VDF_KT=30, VDF_EGI=35, VDF_IVEL=45, VDF_AL=99, VDF_DT=40, VDF_BP=55, VDF_ST=60, VDF_SL=30, VDF_RT=35,
           LEVEL=84, VDA_VEL=45, VDA_DT=45, VDA_BP=80, VDA_ST=70, VDA_SL=45, VDA_RT=38),
    o2=osc(CUTOFF=74, VDF_KT=25, VDF_EGI=30, VDF_IVEL=40, VDF_DT=30, VDF_BP=45, VDF_ST=50, VDF_SL=20, VDF_RT=30,
           LEVEL=66, VDA_VEL=50, VDA_DT=35, VDA_BP=70, VDA_ST=60, VDA_SL=35, VDA_RT=34),
    fx=fxblock("Chorus 1", [22, 32, 0, 0, 12, 0, 1, 0], "Hall", [36, 0, 45, 25, 30, 0, 1, 0], bal=(25, 25, 22, 22))))

# 2. House Organ: one drawbar-style organ, quick decay to a lower sustain, short room.
P.append(program("HseOrgan", ms1="Organ 2",
    o1=osc(CUTOFF=78, VDF_EGI=20, VDF_DT=20, VDF_BP=50, VDF_SL=35, LEVEL=76, VDA_VEL=25,
           VDA_DT=22, VDA_BP=75, VDA_ST=40, VDA_SL=62, VDA_RT=22),
    fx=fxblock("Room", [18, 0, 50, 10, 40, 0, 0, 0], bal=(18, 18, 0, 0))))

# 3. Space Stab: voices plus a digital wave, short, into a stereo delay.
P.append(program("SpaceStab", ms1="Voices", ms2="Digital 3", oct2=1, detune=-6,
    o1=osc(CUTOFF=60, VDF_EGI=55, VDF_IVEL=50, VDF_AL=99, VDF_DT=25, VDF_BP=20, VDF_ST=30, VDF_SL=0, VDF_RT=25,
           LEVEL=78, VDA_VEL=40, VDA_DT=35, VDA_BP=55, VDA_ST=45, VDA_SL=0, VDA_RT=30),
    o2=osc(CUTOFF=55, VDF_EGI=60, VDF_DT=20, VDF_BP=10, VDF_SL=0, VDF_RT=20,
           LEVEL=62, VDA_VEL=40, VDA_DT=28, VDA_BP=40, VDA_ST=35, VDA_SL=0, VDA_RT=25),
    fx=fxblock("Stereo Delay", u16(300) + [30, 40] + u16(450) + [0, 0], "Hall", [50, 0, 40, 30, 30, 0, 0, 0],
               bal=(30, 30, 22, 22))))

# 4. Warm Pad: strings and voices, slow attack and release, ensemble hall.
P.append(program("WarmPad", ms1="Strings", ms2="Voices", detune=7, pmg=VIB,
    o1=osc(CUTOFF=58, VDF_EGI=25, VDF_AT=55, VDF_AL=70, VDF_DT=60, VDF_BP=50, VDF_ST=60, VDF_SL=45, VDF_RT=60,
           LEVEL=76, VDA_VEL=20, VDA_AT=55, VDA_DT=60, VDA_BP=95, VDA_ST=60, VDA_SL=88, VDA_RT=62),
    o2=osc(CUTOFF=55, VDF_EGI=20, VDF_AT=65, VDF_AL=60, VDF_SL=40, VDF_RT=62,
           LEVEL=62, VDA_VEL=15, VDA_AT=65, VDA_DT=60, VDA_BP=95, VDA_ST=60, VDA_SL=85, VDA_RT=64),
    fx=fxblock("Chorus 2", [35, 20, 0, 0, 15, 0, 0, 0], "Ensemble Hall", [70, 0, 40, 30, 30, 0, 0, 0],
               bal=(30, 30, 30, 30))))

# 5. Bell EP: a soft electric piano with a bell layer on top.
P.append(program("BellEP", ms1="DWGS E.P.1", ms2="Bell", oct2=1, detune=3,
    o1=osc(CUTOFF=70, VDF_EGI=30, VDF_IVEL=40, VDF_DT=45, VDF_BP=40, VDF_SL=25, LEVEL=90, VDA_VEL=45,
           VDA_DT=50, VDA_BP=70, VDA_ST=65, VDA_SL=40, VDA_RT=42),
    o2=osc(CUTOFF=75, VDF_EGI=20, VDF_DT=25, VDF_SL=10, LEVEL=55, VDA_VEL=55,
           VDA_DT=40, VDA_BP=45, VDA_ST=55, VDA_SL=20, VDA_RT=40),
    fx=fxblock("Stereo Phaser", [30, 18, 0, 20, 40, 0, 0, 0], "Hall", [40, 0, 50, 25, 30, 0, 0, 0],
               bal=(35, 35, 22, 22))))

# 6. Sub Bass: sine an octave down under a synth bass, mono, dry.
P.append(program("SubBass", ms1="DWGS Sine", ms2="SynthBass1", oct1=-1, oct2=0, mono=True,
    o1=osc(CUTOFF=60, VDF_KT=0, VDF_EGI=0, LEVEL=95, VDA_VEL=20, VDA_DT=40, VDA_BP=95, VDA_ST=60, VDA_SL=88, VDA_RT=15),
    o2=osc(CUTOFF=40, VDF_KT=10, VDF_EGI=50, VDF_IVEL=40, VDF_DT=25, VDF_BP=20, VDF_ST=30, VDF_SL=10, VDF_RT=15,
           LEVEL=60, VDA_VEL=40, VDA_DT=30, VDA_BP=60, VDA_ST=40, VDA_SL=40, VDA_RT=15)))

# 7. Saw Lead: two saws detuned, mono, with vibrato and a delay.
P.append(program("SawLead", ms1="Saw Wave", ms2="Saw Wave", detune=9, mono=True, pmg=(0x60, 48, 40, 10),
    o1=osc(CUTOFF=62, VDF_EGI=40, VDF_IVEL=30, VDF_DT=35, VDF_BP=50, VDF_SL=40, LEVEL=80, VDA_VEL=30,
           VDA_DT=30, VDA_BP=90, VDA_SL=85, VDA_RT=28),
    o2=osc(CUTOFF=62, VDF_EGI=40, VDF_IVEL=30, VDF_DT=35, VDF_BP=50, VDF_SL=40, LEVEL=80, VDA_VEL=30,
           VDA_DT=30, VDA_BP=90, VDA_SL=85, VDA_RT=28),
    fx=fxblock("Cross Delay", u16(250) + [35, 40] + u16(375) + [0, 0], "Room", [22, 0, 45, 15, 40, 0, 0, 0],
               bal=(25, 25, 15, 15))))

# 8. Glass Keys: vibes with a DWGS bell, early reflections.
P.append(program("GlassKeys", ms1="Vibes", ms2="DWGS Bell", oct2=1, detune=4,
    o1=osc(CUTOFF=78, VDF_EGI=20, VDF_DT=40, VDF_SL=30, LEVEL=78, VDA_VEL=50,
           VDA_DT=55, VDA_BP=60, VDA_ST=60, VDA_SL=20, VDA_RT=45),
    o2=osc(CUTOFF=80, VDF_EGI=15, VDF_SL=20, LEVEL=44, VDA_VEL=55, VDA_DT=45, VDA_BP=40, VDA_ST=50, VDA_SL=10, VDA_RT=42),
    fx=fxblock("Early Ref 2", [40, 15, 0, 0, 0, 0, 0, 0], "Hall", [45, 0, 40, 20, 30, 0, 0, 0],
               bal=(25, 25, 25, 25))))

# 9. Pluck Arp: a plucked attack over a digital wave, short, cross delay.
P.append(program("PluckArp", ms1="Pluck", ms2="Digital 2", detune=-4,
    o1=osc(CUTOFF=72, VDF_EGI=35, VDF_DT=20, VDF_BP=10, VDF_SL=0, LEVEL=95, VDA_VEL=40,
           VDA_DT=30, VDA_BP=40, VDA_ST=30, VDA_SL=0, VDA_RT=22),
    o2=osc(CUTOFF=50, VDF_EGI=60, VDF_IVEL=40, VDF_DT=22, VDF_BP=15, VDF_SL=0, VDF_RT=20, LEVEL=88, VDA_VEL=40,
           VDA_DT=32, VDA_BP=35, VDA_ST=30, VDA_SL=0, VDA_RT=22),
    fx=fxblock("Delay/Hall", u16(320) + [40, 40, 55, 0, 45, 20], "Chorus 1", [20, 30, 0, 0, 10, 0, 0, 0],
               bal=(35, 35, 20, 20), serial=True)))

# 10. Airy Choir: choir with a breath layer, long hall.
P.append(program("AiryChoir", ms1="Choir", ms2="Breath", detune=5, pmg=VIB,
    o1=osc(CUTOFF=65, VDF_EGI=20, VDF_AT=30, VDF_AL=80, VDF_SL=55, VDF_RT=55, LEVEL=88, VDA_VEL=25,
           VDA_AT=35, VDA_DT=50, VDA_BP=95, VDA_SL=85, VDA_RT=55),
    o2=osc(CUTOFF=70, VDF_EGI=10, VDF_SL=50, LEVEL=45, VDA_VEL=15, VDA_AT=45, VDA_DT=50, VDA_BP=90, VDA_SL=70, VDA_RT=55),
    fx=fxblock("Symphonic Ens", [40, 0, 0, 0, 0, 0, 0, 0], "Concert Hall", [80, 0, 40, 35, 35, 0, 0, 0],
               bal=(30, 30, 35, 35))))

assert len(P) == 10
blank = program("INIT", ms1="Piano", o1=osc(LEVEL=0))
progs = P + [blank] * 90
body = b"".join(progs)
assert len(body) == 14300
out = sys.argv[1] if len(sys.argv) > 1 else "Fullban"
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "Preview.m1b"), "wb") as f:
    f.write(b"M1B1" + bytes([0, 0, 0, 0]) + body)
print("wrote", os.path.join(out, "Preview.m1b"), len(body) + 8, "bytes:", ", ".join(p[:10].decode().strip() for p in P))
