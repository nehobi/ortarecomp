"""Draws the app icon: a white dragon in flight on a dark round badge.

    python tools/make_icon.py        writes icons/app.png and icons/app.ico

Our own drawing (paths below), so the icon carries nothing from the game.
Needs Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024                                    # drawing size; scaled down after
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "icons")


def bezier(p0, p1, p2, p3, n=24):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        pts.append((u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                    u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
    return pts


def path(cmds):
    """M x y / L x y / C x1 y1 x2 y2 x y, as a flat list, to a polygon."""
    pts, cur, i = [], None, 0
    while i < len(cmds):
        c = cmds[i]
        if c in ("M", "L"):
            cur = (cmds[i + 1], cmds[i + 2])
            pts.append(cur)
            i += 3
        elif c == "C":
            p1, p2, p3 = (cmds[i + 1], cmds[i + 2]), (cmds[i + 3], cmds[i + 4]), (cmds[i + 5], cmds[i + 6])
            pts += bezier(cur, p1, p2, p3)
            cur = p3
            i += 7
        else:
            raise ValueError(c)
    return pts


# Facing right, wings raised. Coordinates on a 1024 square.
FAR_WING = ["M", 600, 470, "C", 640, 360, 720, 250, 850, 170,
            "C", 830, 250, 815, 300, 790, 345, "C", 820, 340, 850, 345, 875, 355,
            "C", 830, 400, 760, 450, 660, 520, "L", 600, 470]
NEAR_WING = ["M", 470, 500, "C", 430, 360, 330, 220, 150, 140,
             "C", 185, 215, 205, 270, 212, 315, "C", 170, 295, 125, 292, 85, 300,
             "C", 130, 335, 160, 375, 178, 415, "C", 140, 405, 100, 412, 68, 430,
             "C", 120, 470, 175, 500, 230, 525, "C", 200, 530, 170, 545, 150, 565,
             "C", 280, 580, 400, 590, 520, 600, "L", 470, 500]
BODY = ["M", 330, 650,                      # tail root, below
        "C", 380, 545, 480, 470, 600, 465,  # back
        "C", 660, 462, 690, 440, 705, 400,  # base of the neck
        "C", 718, 360, 740, 320, 780, 300,  # neck up to the head
        "L", 805, 245,                      # horn
        "L", 818, 298,
        "C", 860, 302, 905, 320, 950, 350,  # snout top
        "L", 958, 378,
        "C", 905, 384, 860, 386, 820, 382,  # jaw
        "C", 795, 395, 780, 420, 770, 455,  # throat
        "C", 755, 520, 715, 590, 655, 625,  # chest
        "L", 680, 690, "L", 640, 705, "L", 610, 645,   # fore leg, tucked
        "C", 570, 665, 520, 675, 475, 672,  # belly
        "L", 490, 740, "L", 445, 755, "L", 425, 680,   # hind leg
        "C", 395, 685, 360, 680, 330, 650]
TAIL = ["M", 360, 600, "C", 260, 650, 210, 740, 250, 830,
        "C", 280, 880, 360, 885, 405, 850,
        "L", 455, 885, "L", 440, 815, "L", 380, 805,      # spade tip
        "C", 340, 830, 300, 815, 300, 780,
        "C", 300, 720, 350, 680, 410, 660, "L", 360, 600]


FIT = 0.86                                  # the drawing, scaled to sit inside the badge


def draw(size_px):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((24, 24, S - 24, S - 24), fill=(14, 22, 38, 255))
    d.ellipse((24, 24, S - 24, S - 24), outline=(70, 120, 175, 255), width=18)
    dragon = Image.new("L", (S, S), 0)
    dd = ImageDraw.Draw(dragon)
    for p in (FAR_WING, NEAR_WING, BODY, TAIL):
        dd.polygon([(512 + (x - 512) * FIT, 512 + (y - 512) * FIT + 20) for x, y in path(p)], fill=255)
    # a little blue glow behind the white, so it reads on the dark badge
    glow = dragon.filter(ImageFilter.GaussianBlur(14))
    img.paste((90, 160, 230, 255), (0, 0), glow.point(lambda v: v * 0.6))
    img.paste((245, 248, 252, 255), (0, 0), dragon)
    return img.resize((size_px, size_px), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    big = draw(256)
    big.save(os.path.join(OUT, "app.png"))
    big.save(os.path.join(OUT, "app.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    main()
