"""Fullban M-One skin artwork: one background per page (frames, captions, logo and the backlit display baked in) and
the display arrows. Run from vst/: python3 art/make_art.py <folder with TitilliumWeb-*.ttf>.
Coordinates are skin pixels (1280x628 plugin area; layout.conf uses shadow y = skin y + 86) -- keep them in sync with
layout.conf (the GEOMETRY block below is the single source)."""
import sys, os, random
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONTS = sys.argv[1]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)))
W, H = 1280, 628
BG_TOP, BG_BOT = (24, 32, 42), (11, 14, 19)
PANEL, LINE = (12, 16, 22, 110), (43, 50, 62)
AQUA, AQUA_HI, INK, DIM, FAINT = (47, 210, 198), (143, 243, 234), (232, 237, 242), (138, 149, 165), (86, 96, 112)
LCD_A, LCD_B, LCD_INK = (40, 58, 255), (18, 26, 222), (220, 212, 255)   # the M1's blue backlight, lavender pixels

# GEOMETRY (skin px)
PLAY_LCD = tuple(int(v) for v in os.environ.get("MONE_LCD", "24,16,784,124").split(","))   # the big display
PLAY_LOGO_CX = (PLAY_LCD[2] + 1260) // 2
QUICK = (20, 224, 1260, 430)
SOUND = (20, 440, 1260, 622)
BROWSE_BAR = (20, 10, 1260, 110)
BROWSE_LCD = (1078, 30, 1240, 90)

def font(size, w="Bold"): return ImageFont.truetype(os.path.join(FONTS, "TitilliumWeb-%s.ttf" % w), size)

def plate():
    im = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3)) + (255,))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))   # faint aqua light from the top
    ImageDraw.Draw(glow).ellipse((W * 0.15, -H * 0.9, W * 0.85, H * 0.45), fill=AQUA + (16,))
    im = Image.alpha_composite(im, glow.filter(ImageFilter.GaussianBlur(90)))
    rnd = random.Random(7)                            # very fine grain, barely there
    grain = Image.new("RGBA", (W, H), (0, 0, 0, 0)); g = grain.load()
    for _ in range(26000):
        x, y = rnd.randrange(W), rnd.randrange(H); v = rnd.choice((255, 0)); g[x, y] = (v, v, v, 7)
    return Image.alpha_composite(im, grain)

PIANO = os.path.join(OUT, "src", "piano.jpg")   # the owner's own photo

KEYS = os.path.join(OUT, "src", "keys.png")      # the owner's keyboard photo, keys only (art/cut_keys.py)

def keys(im, opacity):
    """The cut-out keybed (keys rising from bottom left to top right), black and white, faded into the plate."""
    k = Image.open(KEYS).convert("LA")
    z = float(os.environ.get("MONE_KEYS_ZOOM", "1.0"))
    k = k.resize((int(W * z), int(W * z * k.height / k.width)), Image.LANCZOS)
    dx, dy = int(os.environ.get("MONE_KEYS_DX", "0")), int(os.environ.get("MONE_KEYS_DY", "-300"))
    lum, a = k.split()
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    tint = Image.merge("RGBA", (lum, lum, Image.eval(lum, lambda v: min(255, int(v * 1.08))), a))
    layer.paste(tint, (dx, dy))
    m = Image.new("L", (W, H), 0)                                  # vignette: strongest in the middle
    ImageDraw.Draw(m).ellipse((-W * 0.1, -H * 0.3, W * 1.1, H * 1.3), fill=255)
    m = m.filter(ImageFilter.GaussianBlur(110))
    alpha = Image.eval(ImageChops.multiply(layer.split()[3], m), lambda v: int(v * opacity))
    layer.putalpha(alpha)
    im.alpha_composite(layer)
    return im

def piano(im, opacity=0.20):
    if os.path.exists(KEYS) and os.environ.get("MONE_BG", "keys") == "keys":
        return keys(im, opacity)
    """The piano photo laid diagonally (keys rising from bottom left to top right), black and white, faded into the plate."""
    if not os.path.exists(PIANO):
        return im
    p = Image.open(PIANO).convert("L")
    edge = Image.new("L", p.size, 255)                                # the photo's own extent, to feather its edges
    def place(img, fill):
        img = img.rotate(float(os.environ.get("MONE_PIANO_ROT", "-74")), resample=Image.BICUBIC, expand=True, fillcolor=fill)
        img = img.transpose(Image.FLIP_LEFT_RIGHT)                    # keys rise to the top right
        flip = os.environ.get("MONE_PIANO_FLIP", "turn")
        if flip == "turn": img = img.rotate(180)                      # same diagonal, keyboard the other way round
        elif flip == "mirror": img = img.transpose(Image.FLIP_LEFT_RIGHT)
        sc = float(os.environ.get("MONE_PIANO_ZOOM", "0.6")) * W / img.width
        img = img.resize((int(img.width * sc), int(img.height * sc)), Image.LANCZOS)
        f2 = os.environ.get("MONE_PIANO_FLIP2", "")   # a last flip of the picture: h = left/right, v = top/bottom
        if f2 == "h": img = img.transpose(Image.FLIP_LEFT_RIGHT)
        elif f2 == "v": img = img.transpose(Image.FLIP_TOP_BOTTOM)
        cx0, cy0 = (img.width - W) // 2 + int(os.environ.get("MONE_PIANO_DX", "140")), (img.height - H) // 2 - 40
        return img.crop((cx0, cy0, cx0 + W, cy0 + H))
    p = place(p, 0)
    edge = place(edge, 0).filter(ImageFilter.GaussianBlur(int(os.environ.get("MONE_PIANO_FEATHER", "70"))))
    # soft vignette fade: strongest through the middle band, gone at the edges
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).ellipse((-W * 0.05, -H * 0.25, W * 1.05, H * 1.25), fill=255)
    m = m.filter(ImageFilter.GaussianBlur(120))
    m = Image.eval(ImageChops.multiply(m, edge), lambda v: int(v * opacity))
    tint = Image.merge("RGBA", (p, p, Image.eval(p, lambda v: min(255, int(v * 1.08))), m))   # a hint of cool blue in the whites
    im.alpha_composite(tint)
    return im

def panel(im, box, title=None):
    lay = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(lay)
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((box[0], box[1] + 4, box[2], box[3] + 4), 16, fill=(0, 0, 0, 90))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(6)))
    d.rounded_rectangle(box, 16, fill=PANEL, outline=LINE + (255,), width=1)
    d.line([(box[0] + 16, box[1] + 1), (box[2] - 16, box[1] + 1)], fill=(255, 255, 255, 14))   # top edge catch-light
    if title:
        x = box[0] + 22
        for ch in title:   # letter-spaced caption
            d.text((x, box[1] + 20), ch, font=font(20), fill=AQUA, anchor="lm"); x += d.textlength(ch, font=font(20)) + 4
        d.line([(x + 12, box[1] + 20), (box[2] - 22, box[1] + 20)], fill=LINE + (255,), width=1)
    im.alpha_composite(lay)

def spaced(d, xy, text, f, fill, spacing, anchor="m"):
    total = sum(d.textlength(c, font=f) for c in text) + spacing * (len(text) - 1)
    x = xy[0] - total / 2 if anchor == "m" else xy[0]
    for c in text:
        d.text((x, xy[1]), c, font=f, fill=fill, anchor="lm"); x += d.textlength(c, font=f) + spacing

def lcd(im, box):
    x0, y0, x1, y1 = box
    glow = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 18, fill=(40, 60, 255, 90))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(14)))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((x0 - 7, y0 - 7, x1 + 7, y1 + 7), 16, fill=(5, 7, 10, 255))                     # bezel
    d.rounded_rectangle((x0 - 7, y0 - 7, x1 + 7, y1 + 7), 16, outline=(58, 66, 80, 255), width=1)
    face = Image.new("RGBA", (x1 - x0, y1 - y0)); fd = ImageDraw.Draw(face)
    for y in range(y1 - y0):                                                                           # backlight
        t = y / max(1, y1 - y0 - 1)
        fd.line([(0, y), (x1 - x0, y)], fill=tuple(int(LCD_A[i] + (LCD_B[i] - LCD_A[i]) * t) for i in range(3)) + (255,))
    px = face.load()                                                                                   # faint dot-matrix texture
    for y in range(1, y1 - y0, 3):
        for x in range(1, x1 - x0, 3):
            r, g, b, a = px[x, y]; px[x, y] = (min(255, r + 26), min(255, g + 26), min(255, b + 4), a)   # unlit LCD dots
    hot = Image.new("RGBA", face.size, (0, 0, 0, 0))                                                   # brighter middle, like a lamp behind it
    ImageDraw.Draw(hot).ellipse((face.size[0] * 0.1, -face.size[1] * 0.6, face.size[0] * 0.9, face.size[1] * 1.4), fill=(90, 110, 255, 60))
    face.alpha_composite(hot.filter(ImageFilter.GaussianBlur(60)))
    shade = Image.new("RGBA", face.size, (0, 0, 0, 0))                                                 # inner shadow
    ImageDraw.Draw(shade).rectangle((0, 0, face.size[0], 10), fill=(0, 0, 0, 80))
    face.alpha_composite(shade.filter(ImageFilter.GaussianBlur(5)))
    mask = Image.new("L", face.size, 0); ImageDraw.Draw(mask).rounded_rectangle((0, 0, face.size[0] - 1, face.size[1] - 1), 10, fill=255)
    im.paste(face, (x0, y0), mask)

def page_play():
    im = piano(plate(), float(os.environ.get("MONE_PIANO_OPACITY", "0.30"))); d = ImageDraw.Draw(im)
    lcd(im, PLAY_LCD)
    d = ImageDraw.Draw(im)
    spaced(d, (PLAY_LOGO_CX, 40), "FULLBAN", font(17), AQUA, 5)
    big = font(58); txt = "M-ONE"
    tw = d.textlength(txt, font=big)
    d.text((PLAY_LOGO_CX - tw / 2, 88), "M-", font=big, fill=INK, anchor="lm")
    d.text((PLAY_LOGO_CX - tw / 2 + d.textlength("M-", font=big), 88), "ONE", font=big, fill=AQUA, anchor="lm")
    spaced(d, (PLAY_LOGO_CX, 128), "WORKSTATION", font(12, "SemiBold"), DIM, 3)
    panel(im, QUICK, "QUICK EDIT")
    panel(im, SOUND, "SOUND")
    d = ImageDraw.Draw(im)
    d.text((640, 508), "Quick-edit knobs at 0 = the sound as stored", font=font(17, "SemiBold"), fill=DIM, anchor="lm")
    d.text((640, 536), "Turn them to shape any program or combination", font=font(17, "SemiBold"), fill=FAINT, anchor="lm")
    d.text((640, 588), "Engine: PicoFaceM1 by Michi71  ·  GPL-3.0", font=font(15, "SemiBold"), fill=FAINT, anchor="lm")
    return im

LIB_LCD = ((81, 70, 569, 120), (711, 70, 1199, 120))   # the Library author / bank displays (stepper text areas)

def page_browse():
    im = piano(plate(), 0.14)
    panel(im, (20, 4, 1260, 60))
    for box in LIB_LCD:
        lcd(im, box)
    return im

FX_BAR = (20, 10, 1260, 90)
FX_COL = ((20, 104, 630, 620), (650, 104, 1260, 620))

def page_fx():
    im = piano(plate(), 0.14)
    panel(im, FX_BAR)
    for k, box in enumerate(FX_COL):
        panel(im, box, "EFFECT %d" % (k + 1))
        lcd(im, (box[0] + 82, 232, box[2] - 82, 288))   # the type display, the Play page's backlit M1 blue
    d = ImageDraw.Draw(im)
    d.text((40, 50), "FX", font=font(30), fill=AQUA, anchor="lm")
    return im

def arrow(name, left, on):
    w, h = 96, 110
    im = Image.new("RGBA", (w * 4, h * 4), (0, 0, 0, 0)); d = ImageDraw.Draw(im)   # 4x, then down: smooth edges
    if on: d.rounded_rectangle((8, 8, w * 4 - 8, h * 4 - 8), 36, fill=(220, 212, 255, 50))
    cx, cy, s = w * 2, h * 2, 120
    pts = [(cx + s * 0.55, cy - s), (cx + s * 0.55, cy + s), (cx - s * 0.75, cy)] if left else \
          [(cx - s * 0.55, cy - s), (cx - s * 0.55, cy + s), (cx + s * 0.75, cy)]
    d.polygon(pts, fill=(220, 212, 255, 255 if on else 225))
    im.resize((w, h), Image.LANCZOS).save(os.path.join(OUT, name))

page_play().convert("RGB").save(os.path.join(OUT, "play.png"))
page_browse().convert("RGB").save(os.path.join(OUT, "browse.png"))
page_fx().convert("RGB").save(os.path.join(OUT, "fx.png"))
for left, nm in ((True, "arrow_l"), (False, "arrow_r")):
    arrow(nm + ".png", left, False); arrow(nm + "_on.png", left, True)
print("art written to", OUT)
