#!/usr/bin/env python3
"""Draw Switch-style shoulder prompts (L, R, ZL, ZR) and an original two-piece controller diagram in the
game's outline style and embed them in src/prompts.inc. Our own drawings (DejaVu Sans Bold); they replace the Xbox LB/RB/LT/RT images.
usage: tools/make_prompts.py   (needs Pillow)"""
import io, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
S = 4  # supersampling

def glyph(text, trigger, flip):
    big = Image.new("L", (70 * S, 70 * S), 0)   # drawn on a 70 px grid, scaled to the target size
    d = ImageDraw.Draw(big)
    if trigger:   # ZL/ZR: taller body with a sloped top edge, like a trigger seen from the front
        top, bot, l, r = 12 * S, 58 * S, 13 * S, 57 * S
        slope = 8 * S
        pts = [(l, top + slope), (r, top), (r, bot), (l, bot)] if not flip else [(l, top), (r, top + slope), (r, bot), (l, bot)]
        d.polygon(pts, outline=255, width=3 * S)
        d.line(pts + [pts[0]], fill=255, width=3 * S, joint="curve")
        fs, ty = (22 if len(text) > 1 else 28) * S, 39 * S
    else:         # L/R: rounded bumper
        d.rounded_rectangle((8 * S, 20 * S, 62 * S, 50 * S), radius=7 * S, outline=255, width=3 * S)
        fs, ty = 22 * S, 35 * S
    f = ImageFont.truetype(FONT, fs)
    d.text((35 * S, ty), text, font=f, fill=255, anchor="mm")
    return big

def la(alpha):
    return Image.merge("LA", (Image.new("L", alpha.size, 255), alpha))

def shadow(alpha):  # the game's "_shadow" twin: a soft glow of the same shape
    g = Image.eval(alpha.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(4)), lambda v: v * 3 // 4)
    return Image.composite(alpha, g, alpha)

out = {}
for name, text, trig, flip in [("JoypadShoulder_TL", "L", False, False), ("JoypadShoulder_TR", "R", False, False),
                               ("JoypadShoulder_BL", "ZL", True, False), ("JoypadShoulder_BR", "ZR", True, True)]:
    big = glyph(text, trig, flip)
    # full-size prompt (70 px) and the small one used inside text (53 px, "joypadShoulder_XX_TextInline")
    for fname, size in ((name, 70), ("j" + name[1:] + "_TextInline", 53)):
        a = big.resize((size, size), Image.LANCZOS)
        for suffix, img in (("", la(a)), ("_shadow", la(shadow(a)))):
            buf = io.BytesIO(); img.save(buf, "PNG", optimize=True)
            out[fname + suffix + ".png"] = buf.getvalue()
            img.save(os.path.join(ROOT, "src", "prompts", fname + suffix + ".png"))

# ---- Controller page diagram (Config_A_B.png, 912x520): an original two-piece pad of our own design,
# faceted to match the game's geometric look. The game prints SUPER / SMARTBOMB / MOVE / SHOOT at the
# ends of the four leader lines, so those keep the original positions (symmetric around x = 455.5).
def diagram():
    W, H, S2 = 912, 520, 2
    big = Image.new("L", (W * S2, H * S2), 0)
    d = ImageDraw.Draw(big)
    sc = lambda pts: [(x * S2, y * S2) for x, y in pts]
    mirror = lambda pts: [(911 - x, y) for x, y in pts]
    body, detail = 150, 150
    pod = [(332, 152), (432, 152), (444, 172), (444, 338), (424, 390), (380, 410), (336, 396), (306, 350), (298, 262), (306, 192)]
    bumper = [(338, 134), (424, 134), (434, 147), (330, 147)]
    for shape in (pod, mirror(pod)):
        d.line(sc(shape + [shape[0]]), fill=body, width=5 * S2, joint="curve")
    for shape in (bumper, mirror(bumper)):
        d.polygon(sc(shape), outline=detail, width=4 * S2)
    def ring(cx, cy, r, w): d.ellipse(sc([(cx - r, cy - r), (cx + r, cy + r)]), outline=detail, width=w * S2)
    # offset sticks: left stick high and outward, right stick low and inward (d-pad / buttons opposite)
    for cx, cy in ((352, 214), (507, 300)):
        ring(cx, cy, 26, 4); ring(cx, cy, 15, 3)
    # d-pad (left half, low and inward)
    cx, cy, a, t = 404, 300, 22, 8
    plus = [(cx - t, cy - a), (cx + t, cy - a), (cx + t, cy - t), (cx + a, cy - t), (cx + a, cy + t), (cx + t, cy + t),
            (cx + t, cy + a), (cx - t, cy + a), (cx - t, cy + t), (cx - a, cy + t), (cx - a, cy - t), (cx - t, cy - t)]
    d.polygon(sc(plus), outline=detail, width=3 * S2)
    # face buttons (right half), Switch arrangement: X top, A right, B bottom, Y left
    f = ImageFont.truetype(FONT, 13 * S2)
    cx, cy, gap = 559, 214, 22
    for lab, (dx, dy) in {"X": (0, -1), "A": (1, 0), "B": (0, 1), "Y": (-1, 0)}.items():
        bx, by = cx + dx * gap, cy + dy * gap
        ring(bx, by, 11, 3)
        d.text((bx * S2, by * S2), lab, font=f, fill=detail, anchor="mm")
    # small hexagonal menu buttons on the inner shoulders
    import math
    for hx in (426, 485):
        hexa = [(hx + 7 * math.cos(math.pi / 3 * k), 182 + 7 * math.sin(math.pi / 3 * k)) for k in range(6)]
        d.polygon(sc(hexa), outline=detail, width=2 * S2)
    a = big.resize((W, H), Image.LANCZOS)
    # leader lines, exactly where the original has them (2 px, full strength)
    lines = Image.new("L", (W, H), 0); dl = ImageDraw.Draw(lines)
    # the MOVE line starts where the original does (the game prints the label there) and runs up to the left stick
    for x0, x1, xv, y, yv in ((315, 368, 367, 82, 134), (543, 596, 543, 82, 134), (315, 353, 352, 427, 240), (505, 596, 505, 427, 326)):
        dl.rectangle((x0, y, x1, y + 1), fill=255)
        dl.rectangle((xv, min(y, yv), xv + 1, max(y, yv)), fill=255)
    return Image.composite(lines, a, lines)

def rgba(alpha):
    return Image.merge("RGBA", (Image.new("L", alpha.size, 255),) * 3 + (alpha,))

dg = diagram()
for suffix, img in (("", rgba(dg)), ("_shadow", rgba(shadow(dg)))):
    buf = io.BytesIO(); img.save(buf, "PNG", optimize=True)
    out["Config_A_B" + suffix + ".png"] = buf.getvalue()
    img.save(os.path.join(ROOT, "src", "prompts", "Config_A_B" + suffix + ".png"))

with open(os.path.join(ROOT, "src", "prompts.inc"), "w") as f:
    f.write("// generated by tools/make_prompts.py - Switch-style shoulder prompts\n")
    for n, data in out.items():
        f.write('{"%s", %d, (const unsigned char*)"' % (n, len(data)))
        f.write("".join("\\x%02x" % b for b in data))
        f.write('"},\n')
print("wrote", len(out), "prompts")
