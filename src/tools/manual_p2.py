#!/usr/bin/env python3
# LiveArea manual pages 006+ (guide part), 960x544 PNG, same style as manual_p1.py.
# Text adapted and condensed from Rising Star Games' "Deathsmiles Strategy Guide" (credited in the README).
from PIL import Image, ImageDraw, ImageFont
import os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/man/out"
os.makedirs(OUT, exist_ok=True)
W, H = 960, 544
FD = "/usr/share/fonts/truetype/dejavu/"
def F(n, s): return ImageFont.truetype(FD + n, s)
TITLE = F("DejaVuSerif-Bold.ttf", 30); SUB = F("DejaVuSans.ttf", 15)
BODY = F("DejaVuSans.ttf", 16); BODYB = F("DejaVuSans-Bold.ttf", 16)
SMALL = F("DejaVuSans.ttf", 12); H2 = F("DejaVuSerif-Bold.ttf", 20)
PINK = (236, 170, 230); WHITE = (245, 240, 250); GREY = (180, 170, 195); CARD = (52, 30, 64)

def background():
    im = Image.new("RGB", (W, H)); d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H; d.line([(0, y), (W, y)], fill=(int(28 + 20 * t), int(14 + 6 * t), int(38 + 22 * t)))
    return im

def header(im, title, sub=None):
    d = ImageDraw.Draw(im)
    d.text((40, 22), title, font=TITLE, fill=WHITE)
    if sub: d.text((42, 62), sub, font=SUB, fill=GREY)
    d.line([(40, 88), (W - 40, 88)], fill=(150, 90, 160), width=2)
    d.text((40, H - 22), "Based on the Deathsmiles Strategy Guide by Rising Star Games", font=SMALL, fill=GREY, anchor="ls")
    d.text((W - 40, H - 22), "Deathsmiles", font=SMALL, fill=GREY, anchor="rs")

def wrap(d, text, font, width):
    lines = []
    for para in text.split("\n"):
        words, cur = para.split(), ""
        for w in words:
            t = (cur + " " + w).strip()
            if d.textlength(t, font=font) <= width: cur = t
            else: lines.append(cur); cur = w
        lines.append(cur)
    return lines

def paragraphs(d, x, y, width, paras, font=BODY, lh=23, gap=12, color=WHITE):
    for p in paras:
        for ln in wrap(d, p, font, width):
            d.text((x, y), ln, font=font, fill=color); y += lh
        y += gap
    return y

def text_page(title, sub, paras):
    im = background(); header(im, title, sub)
    paragraphs(ImageDraw.Draw(im), 40, 108, W - 80, paras)
    return im

pages = []

pages.append(text_page("More Than First Expected", "Introduction", [
    "With infinite continues and only eight stages, Deathsmiles can look like a short game. Writing it off that way "
    "misses what the game is really about: score.",
    "Deathsmiles is built for both newcomers and veterans, and its true challenge is reaching the highest score you can. "
    "It sounds simple, but there is far more depth to it than first meets the eye. This is not a shooter where you just "
    "hold the fire button and hope to dodge the bullets.",
    "The following pages cover the characters, the stage routes, the rank system and Death Mode, Power Up mode, and "
    "the items and scoring tips that will help you climb higher in Arcade mode.",
    "Knowing what to do is one thing; doing it is another. And if you are feeling ambitious, try the fabled "
    "one-credit clear: the whole game without a single continue!",
]))

pages.append(text_page("Making the Future More Fun", "A short history of CAVE (1/2)", [
    "CAVE is one of the last great shooting-game developers of the arcade era. It earned a loyal following with its "
    "unique visuals, rewarding score systems and, at times, nail-biting difficulty.",
    "Its first release was the arcade hit DonPachi, but it was the sequel, DoDonPachi, that defined a new kind of "
    "shooter: fast action, screens full of bullets and flashy point bonuses for the most dedicated players. Its "
    "\"chaining\" system pushed players to destroy enemies in quick, unconventional patterns and combo their way "
    "through each stage. The simple controls (shot, laser and bomb) carried over into later CAVE games. Running in over "
    "1,500 arcades in Japan, DoDonPachi won a major Gamest award and kept appearing in the top 10 earnings charts, "
    "a rare feat for a shooter.",
    "CAVE then teamed up with other publishers for two of its most artistic games: Guwange, a dark manic shooter set in "
    "Japan's Muromachi period, and Progear, a cartoon-style mech adventure whose system turns bullet patterns into "
    "streams of point items.",
]))

pages.append(text_page("Making the Future More Fun", "A short history of CAVE (2/2)", [
    "Next came DoDonPachi Daioujou, even fiercer than its predecessor and with a deeper score attack, followed by the "
    "helicopter shooter Ketsui, which rewarded players for flying dangerously close to enemies. More hits followed: "
    "the beginner-friendly Espgaluda, the gorgeous but punishing Mushihimesama, Muchi Muchi Pork and many more, all "
    "known for detailed 2D sprite work, memorable soundtracks and addictive scoring.",
    "As its series grew into franchises with merchandise, console ports, sequels and fan events in Tokyo, CAVE grew "
    "too. It added mobile and online development and went public on the Japanese stock market in 2004.",
    "With the arcade market shrinking, CAVE looked for new players. Deathsmiles, its second horizontal shooter, was a "
    "hit thanks to its gothic imagery, flashy point system and accessible difficulty. It became CAVE's first "
    "self-published Xbox 360 release in 2009, selling over 80,000 copies worldwide, and later came to iPhone and "
    "iPod touch: the version this PlayStation Vita port is based on.",
]))

# ---------- characters ----------
PORTRAIT = {"Windia": "/tmp/man/por_windia.png", "Casper": "/tmp/man/por_casper.png",
            "Follett": "/tmp/man/por_follett.png", "Rosa": "/tmp/man/por_rosa.png"}
CHARS = {
    "Windia": ["Lock Shot: a large ring around her.",
               "Option: fires a crescent projectile in Laser mode. It moves when you tap Shot and stays in place while you hold Laser.",
               "A balanced character overall."],
    "Casper": ["Lock Shot: a large ring around her.",
               "Option: throws a boomerang-like cutter when you tap Shot. It moves while you hold Laser and stays in place when you tap Shot.",
               "Stronger than Windia, but slower in Laser mode."],
    "Follett": ["Lock Shot: forms around her option and locks onto enemies in front of her.",
                "Option: fires a wide shot when you tap Shot. It moves when you tap Shot and stays in place while you hold Laser.",
                "Her Lock Shot is very strong, has a wide range and is the easiest of all to lock on."],
    "Rosa": ["Lock Shot: forms around her option and locks onto enemies behind her.",
             "Option: fires a cluster of homing roses while you hold Laser. It moves while you hold Laser and stays in place when you tap Shot.",
             "The strongest character, but slow, and she cannot lock onto enemies in front of her."],
}
SHOTS = {n: "/tmp/man/shots_%s.png" % n.lower() for n in CHARS}
def char_page(name):
    im = background(); header(im, "Characters: " + name, "Shot, Laser and Lock Shot")
    d = ImageDraw.Draw(im)
    p = Image.open(PORTRAIT[name]).convert("RGB"); p.thumbnail((214, 380))
    d.rounded_rectangle([40, 104, 270, 500], radius=10, fill=p.getpixel((2, 2)))   # same tone as the portrait's parchment
    im.paste(p, (40 + (230 - p.width) // 2, 104 + (396 - p.height) // 2))
    sw = 206   # three separate screenshots (without the guide's light background between them)
    for k in range(3):
        c = Image.open("/tmp/man/shot_%s_%d.png" % (name.lower(), k)).convert("RGB")
        c = c.resize((sw, int(c.height * sw / c.width)), Image.LANCZOS); sh_h = c.height
        im.paste(c, (290 + k * (sw + 11), 104))
    ly = 104 + sh_h + 4
    for k, lab in enumerate(("Shot", "Laser", "Lock Shot")):
        d.text((290 + 103 + k * 217, ly), lab, font=SMALL, fill=GREY, anchor="ma")
    paragraphs(d, 290, ly + 30, W - 330, CHARS[name], font=BODY, lh=23, gap=10)
    return im
for n in ("Windia", "Casper", "Follett", "Rosa"): pages.append(char_page(n))

# speed table
im = background(); header(im, "Character Speed", "Movement speed in each mode")
d = ImageDraw.Draw(im)
cols = ["", "Windia", "Casper", "Follett", "Rosa"]
rows = [("Shot mode", "46 (62)", "52 (68)", "46 (62)", "36 (52)"),
        ("Laser mode", "28 (25)", "18 (25)", "24 (21)", "32 (29)"),
        ("Auto-lock mode", "32 (27)", "42 (37)", "26 (21)", "18 (13)")]
x0, cw, y = 60, 170, 130
d.rounded_rectangle([40, 110, W - 40, 330], radius=10, fill=CARD)
for i, c in enumerate(cols): d.text((x0 + 200 + (i - 1) * cw, y), c, font=BODYB, fill=PINK) if i else None
y += 46
for r in rows:
    d.text((x0, y), r[0], font=BODYB, fill=WHITE)
    for i, v in enumerate(r[1:]): d.text((x0 + 200 + i * cw, y), v, font=BODY, fill=WHITE)
    y += 44
paragraphs(d, 40, 350, W - 80, [
    "Numbers in parentheses are the speed in Power Up mode.",
    "Shot mode: tapping the shot button. Laser mode: holding it. Auto-lock mode: while the Lock Shot is active. "
    "Higher numbers mean faster movement.",
], font=BODY, lh=23, gap=8, color=GREY)
pages.append(im)


# ---------- scoring, stages, ranks (guide pages 5–6) ----------
def img_text_page(title, sub, img, paras, img_w=300, items=False):
    im = background(); header(im, title, sub); d = ImageDraw.Draw(im)
    p = Image.open(img).convert("RGB"); p.thumbnail((img_w, 396))
    im.paste(p, (40, 104)); d.rectangle([40, 104, 40 + p.width - 1, 104 + p.height - 1], outline=(150, 90, 160))
    x = 40 + p.width + 24
    if items:
        y = 104
        for n, t in paras:
            d.text((x, y), n, font=BODYB, fill=PINK)
            y = paragraphs(d, x + 28, y, W - x - 68, [t], font=BODY, lh=21, gap=10)
    else:
        paragraphs(d, x, 104, W - x - 40, paras, font=BODY, lh=22, gap=10)
    return im

pages.append(img_text_page("High Scoring Tips", "The basic scoring loop in Arcade mode", "/tmp/man/p5c_0.png", [
    ("1.", "Shoot enemies and collect the gems they drop before they hit the ground. Using the right weapon on the right enemy makes them drop more."),
    ("2.", "Collect 1,000 gems. The Power Up counter shows your total and maxes out at 1,000."),
    ("3.", "Activate Power Up mode, ideally right before a busy part of the stage with lots of enemies ahead."),
    ("4.", "Enemies now drop far more, and more valuable, gems. Let them hit the ground and split into skulls (the smallest gems) before you collect them."),
    ("5.", "The counter now counts down, at a different rate depending on your weapon. In Power Up mode only bullets can kill you. The number above your character is your +Value: push it to 10,000 to reach Fever, where the points really pile up."),
    ("6.", "Back to step 1!"),
], img_w=250, items=True))

pages.append(img_text_page("Other Considerations", "Scoring strategy", "/tmp/man/p5c_2.png", [
    "The most important thing for scoring is to spend as much time as possible in Power Up mode and to push your +Value as high as you can.",
    "Plan your stage order so that you reach 1,000 gems right before the parts of a stage that are best for Power Up mode.",
    "Playing a stage at rank 2 or 3 is harder, but gives you more chances to score big.",
    "Choosing rank 3 five times activates Death Mode for the rest of the game. Suicide bullets then appear when enemies die; your option can cancel them to add to your counter. Harder again, but with even more scoring opportunities.",
], img_w=250))

pages.append(img_text_page("There's More Than One Way Through...", "Stages", "/tmp/man/map.png", [
    "The world map has three rows of stages:",
    "Top: EX (Canyon) and Castle\nMiddle: A-2 (Graveyard), B-2 (Swamp), C-2 (Volcano)\nBottom: A-1 (Harbor Town), B-1 (Forest), C-1 (Lakeside)",
    "The letters A, B and C are chapters. Each chapter is split in two: one half on the bottom row and the other on the middle row.",
    "The EX stage (Canyon) does not belong to any chapter. The Castle is the final stage, has two parts and counts as Chapter 4.",
], img_w=300))

pages.append(img_text_page("Stage Select", "Choosing your route", "/tmp/man/p5c_1.png", [
    "Start with any of the three bottom stages, then pick any of the three middle stages. From then on you alternate between the bottom and middle rows until all six are cleared: you can never play two middle or two bottom stages in a row.",
    "Once all six are done, you can take on the EX stage or go straight to the Castle.",
    "There are many possible routes, all with similar scoring potential. A wide variety of paths without sacrificing balance is one of the game's most admirable traits.",
], img_w=250))

pages.append(text_page("Stage Rank Select", "Rank 1, 2 or 3", [
    "At the start of each bottom and middle stage you choose its rank: 1 is the easiest and 3 the hardest. Ranks 2 and 3 add more enemies, more bullets, higher difficulty overall and extra boss phases.",
    "The rank and your route also decide which life and bomb items appear in each stage (see the item pages).",
    "In this version you can choose any rank for any stage, as many times as you like.",
    "In the original arcade game, ranks 1 and 2 could each be chosen only once per row of the map, so the last two stages always had to be played at rank 3. That restriction was removed from the home versions, including this one.",
]))


# ---------- Death Mode, Power Up, Fever, bonus, items (guide pages 7–8) ----------
pages.append(text_page("Death Mode", "The hardest way to play", [
    "Choose rank 3 five times and you enter Death Mode. From then on, every enemy you destroy fires suicide bullets back at you: small, star-shaped bullets that your option can absorb.",
    "Once you are in Death Mode there is no way back, even if you choose rank 1 or 2 for your sixth stage.",
    "Every bullet absorbed in Death Mode is worth 100 points, plus your current +Value.",
    "If you play every stage at rank 3 and clear the EX stage before the Castle, the Castle is played at the highest rank in the game: Death Mode Level 2.",
]))

def table(d, x, y, widths, rows, head=True, rh=34):
    tw = sum(widths)
    for r, row in enumerate(rows):
        fill = (70, 40, 84) if (head and r == 0) else (CARD if r % 2 else (44, 26, 56))
        d.rectangle([x, y, x + tw, y + rh], fill=fill)
        cx = x
        for c, (w, cell) in enumerate(zip(widths, row)):
            if cell is not None:
                f = BODYB if (head and r == 0) or c == 0 else BODY
                d.text((cx + 10, y + rh // 2), cell, font=f, fill=PINK if (head and r == 0) else WHITE, anchor="lm")
            cx += w
        y += rh
    return y

im = background(); header(im, "Power Up Mode", "Spending your 1,000 gems"); d = ImageDraw.Draw(im)
y = paragraphs(d, 40, 104, W - 80, [
    "When the item counter reaches 1,000, activate Power Up mode: hold Lock Shot / Power Up (\u25b3 or R in Type iOS, "
    "\u25a1 in Type 360, \u25b3 in Type Custom). Your attack and your option get stronger, and only bullets can hurt you, "
    "so you can fly straight through enemies.",
    "The counter then drains from 1,000 to zero, and you return to normal mode when it runs out. How fast it drains depends on where you are and what you press:",
], gap=8)
y = table(d, 40, y + 2, [250, 630], [
    ("Situation", "Counter drain"),
    ("Stage, no button", "-1 every 4 frames"),
    ("Stage, shot button", "-1 every 2 frames per shot button held"),
    ("Stage, Lock Shot", "-3 every 2 frames (lock -1, plus both shot buttons)"),
    ("Boss fight", "-1 every 2 frames, whatever you press"),
    ("Bomb", "-4 every frame (added on top during boss fights)"),
], rh=31)
pages.append(im)

im = background(); header(im, "Fever Mode and All-Clear Bonus", "Where the big points come from"); d = ImageDraw.Draw(im)
d.text((40, 104), "Fever Mode", font=H2, fill=PINK)
y = paragraphs(d, 40, 136, W - 80, [
    "Push your +Value to its maximum of +10,000 and you enter Fever mode: every enemy you destroy drops High Crowns. You still need the right attack on certain enemies to get the most crowns out of them.",
], gap=14)
d.text((40, y), "All-Clear Bonus", font=H2, fill=PINK)
y = paragraphs(d, 40, y + 32, W - 80, ["When you clear the game, your end bonus is the sum of:"], gap=6)
table(d, 40, y, [220, 660], [
    ("Life bonus", "(130 x remaining lives) x 50,000 pts"),
    ("Bomb bonus", "remaining bombs x 1,000,000 pts"),
    ("Item bonus", "item counter total x 10,000 pts"),
], head=False, rh=34)
pages.append(im)

im = background(); header(im, "Life and Bomb Items", "Recovery items"); d = ImageDraw.Draw(im)
items = [("small", "Small: Prine a la mode", "Refills half a life block. Bonus: 100,000 pts."),
         ("medium", "Medium: Strawberry Parfait", "Refills one full life block. Bonus: 200,000 pts."),
         ("large", "Large: Decoration Cake", "Refills two full life blocks. Bonus: 500,000 pts. Only appears during the Chapter 4 final boss fight."),
         ("magic", "Bomb: \"Magic\"", "Adds one bomb to your stock (maximum 6). It gives no points unless your stock is already full; then each extra Magic is worth 300,000 pts.")]
y = 104
for key, name, desc in items:
    ic = Image.open("/tmp/man/ic_%s.png" % key).convert("RGB"); ic.thumbnail((84, 84))
    im.paste(ic, (40 + (84 - ic.width) // 2, y + (90 - ic.height) // 2))
    d.text((144, y + 6), name, font=BODYB, fill=PINK)
    paragraphs(d, 144, y + 32, W - 184, [desc], font=BODY, lh=21, gap=0)
    y += 100
pages.append(im)

im = background(); header(im, "Item Appearance Chart", "Where life and bomb items appear, by chapter and rank"); d = ImageDraw.Draw(im)
y = table(d, 40, 100, [120, 200, 140, 140, 140, 140], [
    ("Stage", "Location", "Ch.2 Rank 2", "Ch.2 Rank 3", "Ch.3 Rank 2", "Ch.3 Rank 3"),
    ("A-1", "Cyclops (small)", "-", "-", "-", "Small life x2 *"),
    ("B-1", "-", "-", "-", "-", "-"),
    ("C-1", "-", "-", "-", "-", "-"),
    ("A-2", "Cyclops (small)", "-", "Small life", "-", "Bomb"),
    ("B-2", "Old Car", "Small life", "Bomb", "Small life", "Bomb"),
    ("C-2", "Cyclops (small)", "-", "Small life", "-", "Medium life"),
    ("EX", "Beast Dog (mid-boss)", "Bomb", None, None, None),
    ("Castle", "Jitterbug (boss)", "Large life (defeat Jitterbug without bombing)", None, None, None),
], rh=34)
paragraphs(d, 40, y + 10, W - 80, ["* Only in Death Mode. Each item is dropped by the enemy listed under Location."], font=BODY, lh=21, color=GREY)
pages.append(im)


# ---------- point items, +Value, Power Up values, extends, thanks (guide pages 9–11, 2) ----------
im = background(); header(im, "Point Items", "Gems, crowns and the item counter"); d = ImageDraw.Draw(im)
pts = [("pt_large", "Large: High Crown", "2,000 pts. Item counter +10. On its first bounce it breaks into 2 Crown Tiaras, and on the second bounce those break into 4 Skeleton Rings."),
       ("pt_medium", "Medium: Crown Tiara", "800 pts. Item counter +5. Breaks into 2 Skeleton Rings when it hits the ground."),
       ("pt_small", "Small: Skeleton Ring", "100 pts. Item counter +1. Disappears 64 frames after it comes to rest on the ground.")]
y = 104
for key, name, desc in pts:
    ic = Image.open("/tmp/man/%s.png" % key).convert("RGB"); ic.thumbnail((84, 84))
    im.paste(ic, (40 + (84 - ic.width) // 2, y + (90 - ic.height) // 2))
    d.text((144, y + 6), name, font=BODYB, fill=PINK)
    paragraphs(d, 144, y + 32, W - 184, [desc], font=BODY, lh=21, gap=0)
    y += 100
paragraphs(d, 40, y + 4, W - 80, ["The item counter values apply in normal mode. Every suicide bullet absorbed in Death Mode also adds +1 to the item counter."], color=GREY)
pages.append(im)

im = background(); header(im, "The +Value", "Item acquisition total: the core of the scoring system"); d = ImageDraw.Draw(im)
y = paragraphs(d, 40, 104, W - 80, [
    "Once the item counter reaches 1,000 in normal mode, or once you enter Power Up mode, every item you absorb raises your +Value: a bonus added to every item you collect. It is shown next to your character as it grows. The maximum is +10,000, shown in bright orange, and reaching it starts Fever mode.",
    "In normal mode the +Value grows by High Crown +10, Crown Tiara +5, Skeleton Ring +1, so here it is better to grab High Crowns before they break. Each suicide bullet absorbed in Death Mode also adds +1.",
    "Pro strategy: you can max out the +Value without ever entering Power Up mode, by playing the EX and Castle stages entirely in normal mode with the counter at 1,000. Then ride the +10,000 bonus to the end. The +Value resets when you die, so stay alive! This is how the top players reach scores of 500 to 600 million.",
], gap=12)
pages.append(im)

im = background(); header(im, "Power Up Values and Extends", "How items grow the +Value in Power Up mode"); d = ImageDraw.Draw(im)
paragraphs(d, 40, 104, 420, [
    "In Power Up mode every item counts the same: a High Crown adds as much as a Skeleton Ring. What matters is your current +Value; the higher it is, the more each item adds.",
], gap=0)
table(d, 500, 104, [220, 200], [("Current +Value", "Each item adds"), ("0 and up", "+1"), ("100 and up", "+2"), ("200 and up", "+4"),
      ("500 and up", "+8"), ("1,000 and up", "+12"), ("2,000 and up", "+16"), ("5,000 and up", "+20")], rh=30)
y = paragraphs(d, 40, 210, 420, [
    "So in Power Up mode it pays to let High Crowns break: four Skeleton Rings add four times as much as one High Crown. At +5,000 and above that means +80 instead of +20.",
], gap=0)
d.text((40, 360), "Extends", font=H2, fill=PINK)
paragraphs(d, 40, 392, W - 80, [
    "You earn an extra life block at 20 million and at 45 million points. There are no other extends; only the life items can refill your life blocks.",
])
pages.append(im)

im = background(); header(im, "Thanks", "Credits for this guide"); d = ImageDraw.Draw(im)
y = paragraphs(d, 40, 104, W - 80, [
    "The original Deathsmiles Strategy Guide by Rising Star Games thanks the following people for their advice, strategies and help:",
], gap=6)
for n in ["Will Freeman", "Jon Rodgers (CAVE)", "Harry Cole", "EOJ, curator and administrator of the CAVE-STG forum, whose detailed strategy guide the original document was based on"]:
    y = paragraphs(d, 64, y, W - 120, ["\u2022 " + n], gap=2)
paragraphs(d, 40, y + 16, W - 80, [
    "This manual was adapted for the PlayStation Vita port from the Deathsmiles Strategy Guide by Rising Star Games.",
    "Deathsmiles \u00a9 CAVE CO., LTD. All rights reserved.",
], gap=10, color=GREY)
pages.append(im)

for i, p in enumerate(pages, 6):
    p.save(os.path.join(OUT, "%03d.png" % i), optimize=True)
sheet = Image.new("RGB", (W * 2 + 30, (H + 10) * ((len(pages) + 1) // 2) + 10), (60, 60, 60))
for i, p in enumerate(pages):
    sheet.paste(p, (10 + (i % 2) * (W + 10), 10 + (i // 2) * (H + 10)))
sheet.save(os.path.join(OUT, "sheet2.png"))
print("ok", len(pages))
