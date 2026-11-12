"""Controller button pictures for the game's button prompts (runtime/nvn/vulkan/buttonicons.c): Xbox and PlayStation.

  python buttonicons.py <out.png> [<out.h>]

Drawn here from shapes and strokes (no font, nothing taken from a game): circles, rounded keys, letters as strokes.
One sheet: rows of 24-pixel cells (the font of the text's prompts), then rows of 32-pixel cells (the HUD's), each row
a style, each column a Switch button: A B X Y L R ZL ZR + -.
Styles: 0 Xbox by position (the right button is B), 1 PlayStation by position, 2 Xbox by label (A is A).
"""
import sys
from PIL import Image, ImageDraw, ImageFilter

SS = 8  # supersampling
INK = (34, 32, 34, 255)
FILL = (250, 250, 250, 255)
XBOX = {'A': (96, 168, 40), 'B': (210, 48, 40), 'X': (40, 110, 210), 'Y': (220, 170, 20)}
PS = {'cross': (90, 130, 220), 'circle': (220, 70, 70), 'square': (210, 110, 180), 'triangle': (40, 170, 130)}

# letters on a 10 x 14 grid, as strokes
LET = {
    'A': [[(1, 14), (5, 0), (9, 14)], [(2.6, 9), (7.4, 9)]],
    'B': [[(2, 0), (2, 14), (6.5, 14), (8.5, 12.3), (8.5, 9.2), (6.5, 7), (2, 7)], [(2, 0), (6, 0), (8, 1.7), (8, 5.2), (6, 7)]],
    'X': [[(1.5, 0), (8.5, 14)], [(8.5, 0), (1.5, 14)]],
    'Y': [[(1.2, 0), (5, 7), (8.8, 0)], [(5, 7), (5, 14)]],
    'L': [[(2.5, 0), (2.5, 14), (8.5, 14)]],
    'R': [[(2, 14), (2, 0), (6.5, 0), (8.5, 1.8), (8.5, 5.2), (6.5, 7), (2, 7)], [(5.5, 7), (8.8, 14)]],
    'T': [[(1, 0), (9, 0)], [(5, 0), (5, 14)]],
    '1': [[(2.5, 2.5), (5.5, 0), (5.5, 14)], [(2.5, 14), (8.5, 14)]],
    '2': [[(1.5, 2.5), (3.5, 0), (6.5, 0), (8.5, 2.2), (8.5, 5), (1.5, 14), (8.8, 14)]],
}


def text(d, s, cx, cy, h, w, color):
    """letters s centred at (cx, cy), h tall, stroke w"""
    sc = h / 14
    adv = 10 * sc * 0.92
    x0 = cx - adv * len(s) / 2
    for i, ch in enumerate(s):
        for line in LET[ch]:
            pts = [(x0 + i * adv + x * sc * 0.92, cy - h / 2 + y * sc) for x, y in line]
            d.line(pts, fill=color, width=max(1, round(w)), joint='curve')
            for p in (pts[0], pts[-1]):
                d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=color)


def finish(big, n):
    """down to n pixels, with the game's soft light edge around the dark outline"""
    img = big.resize((n, n), Image.LANCZOS)
    a = img.split()[3]
    glow = a.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
    halo = Image.new('RGBA', (n, n), (255, 255, 255, 0))
    halo.putalpha(glow.point(lambda v: int(v * 0.85)))
    halo.alpha_composite(img)
    return halo


def face(n, kind, mark):
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    m, ow = N * 0.07, N * 0.085
    d.ellipse((m, m, N - m, N - m), fill=INK)
    d.ellipse((m + ow, m + ow, N - m - ow, N - m - ow), fill=FILL)
    c, h, w = N / 2, N * 0.42, N * 0.088
    if kind == 'xbox':
        text(d, mark, c, c, h, w, XBOX[mark] + (255,))
    else:
        col = PS[mark] + (255,)
        r = N * 0.2
        lw = round(N * 0.085)
        if mark == 'cross':
            d.line((c - r, c - r, c + r, c + r), fill=col, width=lw)
            d.line((c - r, c + r, c + r, c - r), fill=col, width=lw)
        elif mark == 'circle':
            d.ellipse((c - r, c - r, c + r, c + r), outline=col, width=lw)
        elif mark == 'square':
            d.rectangle((c - r * .9, c - r * .9, c + r * .9, c + r * .9), outline=col, width=lw)
        else:
            d.polygon([(c, c - r * 1.05), (c + r * 1.1, c + r * .8), (c - r * 1.1, c + r * .8)], outline=col, width=lw)
    return finish(big, n)


def key(n, label, trigger):
    """a shoulder button: a rounded key (a trigger: its top rounder)"""
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    x0, y0, x1, y1 = N * .06, N * .2, N * .94, N * .84
    ow = N * .085
    rad = N * (.3 if trigger else .14)
    d.rounded_rectangle((x0, y0, x1, y1), rad, fill=INK)
    d.rounded_rectangle((x0 + ow, y0 + ow, x1 - ow, y1 - ow), max(1, rad - ow), fill=FILL)
    text(d, label, N / 2, (y0 + y1) / 2, N * .3, N * .075, INK)
    return finish(big, n)


def menu(n, which):
    """+ and -: Xbox's Menu (three lines) and View (two windows); PlayStation's Options / Create the same marks"""
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    m, ow = N * .07, N * .085
    d.ellipse((m, m, N - m, N - m), fill=INK)
    d.ellipse((m + ow, m + ow, N - m - ow, N - m - ow), fill=FILL)
    c, lw = N / 2, round(N * .075)
    if which == 'menu':
        for dy in (-.15, 0, .15):
            d.line((c - N * .2, c + N * dy, c + N * .2, c + N * dy), fill=INK, width=lw)
    else:
        s = N * .17
        d.rectangle((c - s * 1.1, c - s * 1.0, c + s * .5, c + s * .4), outline=INK, width=lw)
        d.rectangle((c - s * .5, c - s * .4, c + s * 1.1, c + s * 1.0), fill=FILL, outline=INK, width=lw)
    return finish(big, n)


def row(n, style):
    # Switch A (right), B (bottom), X (top), Y (left), L, R, ZL, ZR, +, -
    if style == 0:
        faces = [face(n, 'xbox', m) for m in 'BAYX']
    elif style == 1:
        faces = [face(n, 'ps', m) for m in ('circle', 'cross', 'triangle', 'square')]
    else:
        faces = [face(n, 'xbox', m) for m in 'ABXY']
    if style == 1:
        sh = [key(n, 'L1', False), key(n, 'R1', False), key(n, 'L2', True), key(n, 'R2', True)]
    else:
        sh = [key(n, 'LB', False), key(n, 'RB', False), key(n, 'LT', True), key(n, 'RT', True)]
    return faces + sh + [menu(n, 'menu'), menu(n, 'view')]


def sheet():
    img = Image.new('RGBA', (10 * 32, 3 * 24 + 3 * 32), (0, 0, 0, 0))
    y = 0
    for n in (24, 32):
        for style in range(3):
            for i, g in enumerate(row(n, style)):
                img.alpha_composite(g, (i * n, y))
            y += n
    return img


if __name__ == '__main__':
    img = sheet()
    img.save(sys.argv[1], optimize=True)
    if len(sys.argv) > 2:
        data = open(sys.argv[1], 'rb').read()
        with open(sys.argv[2], 'w') as f:
            f.write('// Made by tools/buttonicons.py (drawn there): the sheet of controller button pictures, a PNG\n')
            f.write('static const unsigned char BUTTON_ICONS_PNG[] = {\n')
            for i in range(0, len(data), 24):
                f.write('    ' + ','.join(str(b) for b in data[i:i + 24]) + ',\n')
            f.write('};\n')
