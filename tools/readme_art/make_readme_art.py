#!/usr/bin/env python3
# make_readme_art.py <repo>: the README's title, divider and section plates (our own art, fonts under the OFL),
# and the gallery crops taken from captures already in docs/images. Nothing is read from the retail game.
#
#   python3 -I tools/readme_art/make_readme_art.py .
import math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = os.path.join(ROOT, 'docs/images/readme'); os.makedirs(OUT, exist_ok=True)
GF = '/usr/share/fonts/truetype/google-fonts/ofl/'
SERIF_B = GF + 'cardo/Cardo-Bold.ttf'; SERIF = GF + 'cardo/Cardo-Regular.ttf'; SERIF_I = GF + 'cardo/Cardo-Italic.ttf'
INK = (10, 8, 9)            # the plate: near black, a little warm
BONE = (236, 222, 196)
FLAME = [(255, 244, 200), (255, 200, 90), (240, 120, 30), (150, 40, 10)]

def plate(w, h, r=18):
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0)); ImageDraw.Draw(im).rounded_rectangle((0, 0, w - 1, h - 1), r, fill=INK + (255,))
    return im

def flame(h, phase, lean=0.0):
    """A candle flame drawn as stacked ellipses; phase moves its height and lean."""
    w = h * 0.42; im = Image.new('RGBA', (int(w * 3), int(h * 1.6)), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    cx = im.width / 2; base = im.height - 2
    hh = h * (0.92 + 0.08 * math.sin(phase * 2.1)); sway = (lean + 0.12 * math.sin(phase * 1.3)) * h
    for k, col in enumerate(reversed(FLAME)):
        s = 1.0 - k * 0.22; top = base - hh * s; ww = w * s
        d.polygon([(cx - ww / 2, base - ww * 0.4), (cx + sway * s, top), (cx + ww / 2, base - ww * 0.4)], fill=col + (255,))
        d.ellipse((cx - ww / 2, base - ww * 0.9, cx + ww / 2, base), fill=col + (255,))
    return im

def glow(size, centre, radius, strength):
    g = Image.new('L', size, 0); d = ImageDraw.Draw(g); x, y = centre
    d.ellipse((x - radius, y - radius, x + radius, y + radius), fill=int(255 * strength))
    g = g.filter(ImageFilter.GaussianBlur(radius * 0.55))
    col = Image.new('RGBA', size, (255, 150, 50, 0)); col.putalpha(g); return col

def save_gif(frames, durs, path):
    pal_src = frames[len(frames) // 2].convert('RGB').quantize(255, method=Image.MEDIANCUT)
    out = []
    for fr in frames:
        a = np.array(fr); alpha = a[:, :, 3] > 127
        q = Image.fromarray(a[:, :, :3], 'RGB').quantize(palette=pal_src, dither=Image.Dither.NONE)
        p = np.array(q).astype(np.int16) + 1; p[p > 255] = 255; p[~alpha] = 0
        im = Image.fromarray(p.astype(np.uint8), 'P'); im.putpalette([0, 0, 0] + pal_src.getpalette()[:255 * 3]); out.append(im)
    out[0].save(path, save_all=True, append_images=out[1:], duration=durs, loop=0, transparency=0, disposal=2)

def candle(h):
    """A white taper with a drip, h tall."""
    w = max(6, int(h * 0.22)); im = Image.new('RGBA', (w + 6, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rectangle((3, 4, 3 + w - 1, h - 1), fill=(222, 210, 186, 255)); d.rectangle((3, 4, 4, h - 1), fill=(250, 244, 230, 255))
    d.ellipse((3 + w - 4, 6, 3 + w + 1, 14), fill=(222, 210, 186, 255)); d.line((3 + w // 2, 0, 3 + w // 2, 5), fill=(40, 30, 24, 255), width=2)
    return im

# ---------- title: the word lit by two candles that flicker ----------
def title():
    W, H = 960, 260; f = ImageFont.truetype(SERIF_B, 132); fs = ImageFont.truetype(SERIF, 26)
    word = 'VRealms'; sub = 'REALMS  OF  THE  HAUNTING   ·   IN  VR'
    frames, durs = [], []
    n = 20
    for i in range(n):
        ph = i / n * 2 * math.pi
        c = plate(W, H)
        k1 = 0.55 + 0.25 * math.sin(ph * 2) + 0.08 * math.sin(ph * 7); k2 = 0.55 + 0.25 * math.sin(ph * 2 + 2.1) + 0.08 * math.sin(ph * 5 + 1)
        c.alpha_composite(glow((W, H), (110, 120), 150, k1 * 0.55)); c.alpha_composite(glow((W, H), (W - 110, 120), 150, k2 * 0.55))
        # the word: bone, warmed on the side nearer the brighter candle
        m = Image.new('L', (W, H), 0); ImageDraw.Draw(m).text((W / 2, 112), word, font=f, fill=255, anchor='mm')
        grad = np.zeros((H, W, 3), np.float32)
        xs = np.linspace(0, 1, W)[None, :, None]
        warm = np.array([255, 196, 120], np.float32); cool = np.array(BONE, np.float32)
        lit = (k1 * (1 - xs) + k2 * xs)
        grad[:] = cool * (1 - 0.35 * lit) + warm * 0.35 * lit
        txt = Image.fromarray(np.clip(grad, 0, 255).astype(np.uint8), 'RGB').convert('RGBA'); txt.putalpha(m)
        sh = Image.new('RGBA', (W, H), (0, 0, 0, 0)); sh.putalpha(m.filter(ImageFilter.GaussianBlur(3)).point(lambda v: int(v * 0.9)))
        c.alpha_composite(sh, (3, 4)); c.alpha_composite(txt)
        ImageDraw.Draw(c).text((W / 2, 205), sub, font=fs, fill=(170, 150, 130, 255), anchor='mm')
        for cx, k, lean in ((110, k1, -0.05), (W - 110, k2, 0.05)):
            cd = candle(96); c.alpha_composite(cd, (cx - cd.width // 2, 140))
            fl = flame(int(40 * (0.85 + 0.3 * k)), ph * 3 + cx, lean); c.alpha_composite(fl, (cx - fl.width // 2, 144 - fl.height + 6))
        frames.append(c); durs.append(90)
    save_gif(frames, durs, os.path.join(OUT, 'title.gif'))
    frames[0].save(os.path.join(OUT, 'title_still.png'), optimize=True)

# ---------- divider: a thin rule with flames along it ----------
def divider():
    W, H = 960, 56; frames, durs = [], []; n = 16
    for i in range(n):
        c = plate(W, H, 12); d = ImageDraw.Draw(c)
        d.line((40, H - 16, W - 40, H - 16), fill=(120, 90, 60, 255), width=2)
        for j, x in enumerate(range(80, W - 40, 100)):
            ph = i / n * 2 * math.pi + j * 1.7
            c.alpha_composite(glow((W, H), (x, H - 28), 26, 0.35 + 0.15 * math.sin(ph * 2)))
            fl = flame(22 + int(3 * math.sin(ph * 3)), ph, 0.0); c.alpha_composite(fl, (x - fl.width // 2, H - 16 - fl.height + 4))
        frames.append(c); durs.append(110)
    save_gif(frames, durs, os.path.join(OUT, 'divider.gif'))

# ---------- section plates ----------
def section(text, name):
    f = ImageFont.truetype(SERIF_B, 40); tw = int(ImageDraw.Draw(Image.new('L', (1, 1))).textlength(text, font=f))
    W, H = tw + 120, 70; c = plate(W, H, 14)
    c.alpha_composite(glow((W, H), (40, H // 2), 34, 0.6))
    fl = flame(30, 1.0); c.alpha_composite(fl, (40 - fl.width // 2, H // 2 + 14 - fl.height))
    ImageDraw.Draw(c).text((80, H // 2 + 2), text, font=f, fill=BONE + (255,), anchor='lm')
    c.save(os.path.join(OUT, name), optimize=True)

# ---------- gallery crops from captures already in the repo ----------
def crops():
    I = os.path.join(ROOT, 'docs/images')
    def up2(src, out):
        im = Image.open(os.path.join(I, src)).convert('RGB'); im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(os.path.join(OUT, out), optimize=True)
    up2('held_lamp_held_up_367c710c.png', 'g_lamp.png')
    up2('gestures_ward_near_03548253.png', 'g_ward.png')
    up2('mount_low_health_edge_6af79ee2.png', 'g_wrist.png')
    use = Image.open(os.path.join(I, 'hands/engine_useitem_flat_ad6d1ec5.png')).convert('RGB')
    use.crop((1920, 0, 2560, 400)).save(os.path.join(OUT, 'g_candle.png'), optimize=True)        # "R hand grip: candle lit"
    inv = Image.open(os.path.join(I, 'hands/engine_invhand2_flat_a80512e5.png')).convert('RGB')
    inv.crop((1920, 0, 2560, 400)).save(os.path.join(OUT, 'g_inv_touch.png'), optimize=True)     # "fingertip near the panel"
    inv.crop((1280, 400, 1920, 800)).save(os.path.join(OUT, 'g_inv_examine.png'), optimize=True) # "index tap: examine"
    colt = Image.open(os.path.join(I, 'colt/support_fit_rightshoots_skin.png')).convert('RGB')
    colt.crop((0, 0, 960, 390)).save(os.path.join(OUT, 'g_colt.png'), optimize=True)

title(); divider(); crops()
for t, n in (('What it is', 'h_what.png'), ('Your hands', 'h_hands.png'), ('In the house', 'h_house.png'),
             ('Where it stands', 'h_status.png'), ('Running it', 'h_run.png'), ('Reading the code', 'h_code.png'), ('The original, 1996', 'h_original.png')):
    section(t, n)
print('ok', sorted(os.listdir(OUT)))
