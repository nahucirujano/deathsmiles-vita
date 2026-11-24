#!/usr/bin/env python3
# Generates LiveArea manual pages 001-005 (cover + controls), 960x544 PNG.
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/man/out"
COVER = "/tmp/man/hi-01.png"
PAD = "/mnt/user-data/uploads/0e070cf0-image.png"
os.makedirs(OUT, exist_ok=True)
W, H = 960, 544
FD = "/usr/share/fonts/truetype/dejavu/"
def F(name, s): return ImageFont.truetype(FD + name, s)
TITLE = F("DejaVuSerif-Bold.ttf", 30)
SUB = F("DejaVuSans.ttf", 15)
LAB = F("DejaVuSans-Bold.ttf", 15)
LAB2 = F("DejaVuSans.ttf", 12)
BODY = F("DejaVuSans.ttf", 15)
BODYB = F("DejaVuSans-Bold.ttf", 15)
PINK = (236, 170, 230); WHITE = (245, 240, 250); GREY = (180, 170, 195)

def background():
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(28 + 20 * t), int(14 + 6 * t), int(38 + 22 * t)))
    return im

def header(im, title, sub=None):
    d = ImageDraw.Draw(im)
    d.text((40, 22), title, font=TITLE, fill=WHITE)
    if sub: d.text((42, 62), sub, font=SUB, fill=GREY)
    d.line([(40, 88), (W - 40, 88)], fill=(150, 90, 160), width=2)
    d.text((W - 40, H - 22), "Deathsmiles", font=LAB2, fill=GREY, anchor="rs")

# ---------- cover ----------
def cover():
    src = Image.open(COVER).convert("RGB")
    sw, sh = src.size
    cw = sw - 139                  # trim left edge so the bottom logos fall outside
    ch = int(cw * H / W)           # crop height for 960x544 ratio
    top = int(sh * 0.06)          # drop the guide's own title line
    im = src.crop((sw - cw, top, sw, top + ch)).resize((W, H), Image.LANCZOS)
    # dark band bottom-left for our title
    band = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(band)
    for x in range(560):
        bd.line([(x, 440), (x, H)], fill=int(200 * (1 - x / 560)))
    band = band.filter(ImageFilter.GaussianBlur(18))
    im = Image.composite(Image.new("RGB", (W, H), (10, 4, 16)), im, band)
    import cv2, numpy as np
    a = np.array(im)
    g = cv2.cvtColor(a, cv2.COLOR_RGB2GRAY)
    m = np.zeros_like(g)
    m[0:24, 0:300] = (g[0:24, 0:300] < 150) * 255
    m = cv2.dilate(m, np.ones((5, 5), np.uint8))
    im = Image.fromarray(cv2.inpaint(a, m, 7, cv2.INPAINT_TELEA))
    d = ImageDraw.Draw(im)
    d.text((32, 470), "Instruction Manual", font=F("DejaVuSerif-Bold.ttf", 34), fill=WHITE)
    d.text((34, 512), "PlayStation Vita Port", font=SUB, fill=PINK)
    return im

# ---------- controller pages ----------
pad = Image.open(PAD).convert("RGBA")
S = 0.74
pad_s = pad.resize((int(pad.width * S), int(pad.height * S)), Image.LANCZOS)
PX, PY = (W - pad_s.width) // 2, 200
P = {  # button positions in the original 713x309 image
    "up": (67, 75), "left": (40, 103), "right": (95, 103), "down": (67, 131),
    "dpad": (67, 103), "lstick": (85, 183),
    "tri": (648, 76), "sq": (620, 103), "cir": (676, 103), "x": (648, 130),
    "rstick": (628, 186), "select": (619, 246), "start": (656, 246),
    "L": (70, 8), "R": (643, 8), "screen": (360, 145),
}
def pt(k):
    x, y = P[k]; return (PX + x * S, PY + y * S)

LX = 205   # left label column right edge
RX = PX + pad_s.width + 18  # right label column left edge

def label(d, key, y, text, side, note=None):
    keys = key if isinstance(key, tuple) else (key,)
    if side == "L":
        ex = LX + 8
        d.text((LX, y), text, font=LAB, fill=WHITE, anchor="rm")
        if note: d.text((LX, y + 15), note, font=LAB2, fill=GREY, anchor="rm")
    else:
        ex = RX - 8
        d.text((RX, y), text, font=LAB, fill=WHITE, anchor="lm")
        if note: d.text((RX, y + 15), note, font=LAB2, fill=GREY, anchor="lm")
    for k in keys:
        bx, by = pt(k)
        d.line([(ex, y), (bx, by)], fill=PINK, width=2)
        d.ellipse([bx - 3, by - 3, bx + 3, by + 3], fill=PINK)
    if len(keys) > 1:
        d.ellipse([ex - 3, y - 3, ex + 3, y + 3], fill=PINK)

def ctrl_page(title, sub, labels, screen_lines, foot):
    im = background()
    header(im, title, sub)
    im.paste(pad_s, (PX, PY), pad_s)
    d = ImageDraw.Draw(im)
    for key, y, text, side, *note in labels:
        label(d, key, y, text, side, note[0] if note else None)
    # text on the console screen
    sx, sy = pt("screen")
    yy = sy - 11 * len(screen_lines)
    for ln in screen_lines:
        d.text((sx, yy), ln, font=LAB2 if not ln.isupper() else LAB, fill=WHITE, anchor="mm")
        yy += 22
    fy = 448
    for ln in foot:
        d.text((40, fy), ln, font=BODY, fill=GREY); fy += 22
    return im

pages = [cover()]

pages.append(ctrl_page(
    "Controls", "Buttons shared by every control type",
    [("L", 118, "L button", "L", "Select + L: change control type"),
     ("dpad", 250, "D-pad", "L", "Move"),
     ("lstick", 335, "Left stick", "L", "Move"),
     ("R", 118, "R button", "R", "Depends on control type"),
     (("x", "cir"), 215, "\u00d7 / \u25cb buttons", "R", "Menus: confirm / cancel"),
     ("rstick", 300, "Right stick", "R", "Not used"),
     ("select", 350, "SELECT", "R", "+ L: change control type"),
     ("start", 395, "START", "R", "Pause"),
     ],
    ["THREE CONTROL TYPES", "iOS  >  360  >  Custom", "Press SELECT + L to switch"],
    ["The current type is shown on screen when you switch, and it is remembered next time.",
     "Touch screen: the on-screen buttons are still there and work just like on iPhone.",
     "They hide as soon as you use the physical buttons, which take priority."]))

pages.append(ctrl_page(
    "Controls: Type iOS", "The original iPhone layout. Your character fires automatically.",
    [(("R", "tri"), 140, "Lock Shot / Power Up", "R", "Hold"),
     ("dpad", 250, "D-pad / left stick", "L", "Move"),
     ("sq", 205, "Shot / Laser", "R"),
     ("cir", 265, "Bomb", "R"),
     ("x", 325, "Change direction", "R", "Fire left / right"),
     ("start", 395, "START", "R", "Pause"),
     ],
    ["TYPE iOS", "Auto Shot ON"],
    ["Auto Shot keeps firing while you play; the buttons add laser, Lock Shot and bombs.",
     "Touch screen: the on-screen buttons are still there and work just like on iPhone.",
     "They hide as soon as you use the physical buttons, which take priority."]))

pages.append(ctrl_page(
    "Controls: Type 360", "Based on the Xbox 360 version of Deathsmiles.",
    [("L", 118, "L button", "L", "Turbo shot (left)"),
     ("dpad", 250, "D-pad / left stick", "L", "Move"),
     ("R", 118, "R button", "R", "Turbo shot (right)"),
     ("tri", 160, "Bomb", "R"),
     ("sq", 210, "Lock Shot / Power Up", "R", "Hold"),
     ("cir", 265, "Shot / Laser (right)", "R", "Tap: shot  ·  Hold: laser"),
     ("x", 325, "Shot / Laser (left)", "R", "Tap: shot  ·  Hold: laser"),
     ("start", 395, "START", "R", "Pause"),
     ],
    ["TYPE 360", "Auto Shot OFF"],
    ["Firing left or right also turns your character to face that side.",
     "In menus the buttons keep their normal behaviour."]))

pages.append(ctrl_page(
    "Controls: Type Custom", "An alternative layout with the shot buttons on the left side.",
    [("L", 118, "L button", "L", "Turbo shot (left)"),
     ("dpad", 250, "D-pad / left stick", "L", "Move"),
     ("R", 118, "R button", "R", "Turbo shot (right)"),
     ("tri", 160, "Lock Shot / Power Up", "R", "Hold"),
     ("sq", 210, "Shot / Laser (left)", "R", "Tap: shot  ·  Hold: laser"),
     ("cir", 265, "Bomb", "R"),
     ("x", 325, "Shot / Laser (right)", "R", "Tap: shot  ·  Hold: laser"),
     ("start", 395, "START", "R", "Pause"),
     ],
    ["TYPE CUSTOM", "Auto Shot OFF"],
    ["Firing left or right also turns your character to face that side.",
     "In menus the buttons keep their normal behaviour."]))

for i, p in enumerate(pages, 1):
    p.save(os.path.join(OUT, "%03d.png" % i), optimize=True)
sheet = Image.new("RGB", (W * 2 + 30, (H + 10) * 3 + 10), (60, 60, 60))
for i, p in enumerate(pages):
    sheet.paste(p, (10 + (i % 2) * (W + 10), 10 + (i // 2) * (H + 10)))
sheet.save(os.path.join(OUT, "sheet.png"))
print("ok", len(pages))
