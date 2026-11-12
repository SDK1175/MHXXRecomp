"""Controller button pictures for the game's button prompts (runtime/nvn/vulkan/buttonicons.c): Xbox and PlayStation.

  python buttonicons.py <out.png> [<out.h>]

Drawn here from shapes and strokes (no font, nothing taken from a game): circles, rounded keys, letters as strokes.
One sheet: rows of 24-pixel cells (the font of the text's prompts), then rows of 32-pixel cells (the HUD's); in each
size a row for Xbox, then PlayStation; each column a button of the controller, in config.h's PAD_SRC_* order from
south (the runtime picks the one each Switch button is pressed with, [Controller] and FaceButtons applied).
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
    '3': [[(1.5, 1.5), (3.5, 0), (6.5, 0), (8.3, 1.8), (8.3, 5), (6, 6.8), (4, 6.8)], [(6, 6.8), (8.5, 8.8), (8.5, 12.2), (6.5, 14), (3.5, 14), (1.3, 12.3)]],
    '4': [[(6.5, 14), (6.5, 0), (1, 9.5), (9, 9.5)]],
    'P': [[(2, 14), (2, 0), (6.5, 0), (8.5, 1.8), (8.5, 5.4), (6.5, 7.2), (2, 7.2)]],
    'S': [[(8.3, 1.8), (6.5, 0), (3.5, 0), (1.6, 1.8), (1.6, 4.8), (3.5, 6.8), (6.5, 7.2), (8.5, 9.2), (8.5, 12.2), (6.5, 14), (3.5, 14), (1.4, 12.2)]],
    'M': [[(1, 14), (1, 0), (5, 9), (9, 0), (9, 14)]],
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


def dpad(n, arm):
    """the d-pad, the pressed arm dark (0 up, 1 down, 2 left, 3 right)"""
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    c, a, t, ow = N / 2, N * .43, N * .17, N * .07
    for x0, y0, x1, y1 in ((c - t, c - a, c + t, c + a), (c - a, c - t, c + a, c + t)):
        d.rectangle((x0, y0, x1, y1), fill=INK)
    for x0, y0, x1, y1 in ((c - t + ow, c - a + ow, c + t - ow, c + a - ow), (c - a + ow, c - t + ow, c + a - ow, c + t - ow)):
        d.rectangle((x0, y0, x1, y1), fill=FILL)
    k = a - ow - t * .2
    box = [(c - t + ow, c - a + ow, c + t - ow, c - t), (c - t + ow, c + t, c + t - ow, c + a - ow),
           (c - a + ow, c - t + ow, c - t, c + t - ow), (c + t, c - t + ow, c + a - ow, c + t - ow)][arm]
    d.rectangle(box, fill=INK)
    return finish(big, n)


def stick(n, label):
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    m, ow = N * 0.07, N * 0.085
    d.ellipse((m, m, N - m, N - m), fill=INK)
    d.ellipse((m + ow, m + ow, N - m - ow, N - m - ow), fill=FILL)
    text(d, label, N / 2, N / 2, N * .3, N * .075, INK)
    return finish(big, n)


def touchpad(n):
    N = n * SS
    big = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((N * .06, N * .24, N * .94, N * .76), N * .12, fill=INK)
    d.rounded_rectangle((N * .145, N * .325, N * .855, N * .675), N * .06, fill=FILL)
    return finish(big, n)


# The controller's buttons, in config.h's PAD_SRC_* order from PAD_SRC_SOUTH
COLUMNS = ['south', 'east', 'west', 'north', 'lb', 'rb', 'lt', 'rt', 'start', 'back', 'up', 'down', 'left', 'right',
           'ls', 'rs', 'p1', 'p2', 'p3', 'p4', 'misc', 'touchpad']


def glyph(n, style, col):
    ps = style == 1
    if col in ('south', 'east', 'west', 'north'):
        if ps:
            return face(n, 'ps', {'south': 'cross', 'east': 'circle', 'west': 'square', 'north': 'triangle'}[col])
        return face(n, 'xbox', {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y'}[col])
    if col in ('lb', 'rb', 'lt', 'rt'):
        names = {'lb': 'L1', 'rb': 'R1', 'lt': 'L2', 'rt': 'R2'} if ps else {'lb': 'LB', 'rb': 'RB', 'lt': 'LT', 'rt': 'RT'}
        return key(n, names[col], col in ('lt', 'rt'))
    if col in ('start', 'back'):
        return menu(n, 'menu' if col == 'start' else 'view')
    if col in ('up', 'down', 'left', 'right'):
        return dpad(n, ['up', 'down', 'left', 'right'].index(col))
    if col in ('ls', 'rs'):
        return stick(n, ('L3' if col == 'ls' else 'R3') if ps else ('LS' if col == 'ls' else 'RS'))
    if col.startswith('p'):
        return key(n, col.upper(), False)
    if col == 'misc':
        return key(n, 'M', False)
    return touchpad(n)


# Keyboard: a blank key (24 and 32), and letters for the key's name, drawn on it by the runtime (DejaVu Sans Bold,
# Bitstream Vera licence: its letters may be used in pictures like these). CHARS' order is the runtime's.
CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789^v<>'
CW, CH = 12, 16
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'


def chars():
    from PIL import ImageFont
    img = Image.new('RGBA', (len(CHARS) * CW, CH), (0, 0, 0, 0))
    f = ImageFont.truetype(FONT, 15 * SS)
    for i, ch in enumerate(CHARS):
        big = Image.new('RGBA', (CW * SS, CH * SS), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        w2, h2 = CW * SS, CH * SS
        if ch in '^v<>':  # (the arrow keys: a triangle)
            c, r = w2 / 2, w2 * .36
            pts = {'^': [(c, h2 * .25), (c + r, h2 * .7), (c - r, h2 * .7)], 'v': [(c, h2 * .75), (c + r, h2 * .3), (c - r, h2 * .3)],
                   '<': [(c - r, h2 / 2), (c + r, h2 * .25), (c + r, h2 * .75)], '>': [(c + r, h2 / 2), (c - r, h2 * .25), (c - r, h2 * .75)]}[ch]
            d.polygon(pts, fill=INK)
        else:
            box = d.textbbox((0, 0), ch, font=f)
            d.text(((w2 - (box[2] - box[0])) / 2 - box[0], (h2 - (box[3] - box[1])) / 2 - box[1]), ch, font=f, fill=INK)
        img.alpha_composite(big.resize((CW, CH), Image.LANCZOS), (i * CW, 0))
    return img


def sheet():
    """rows: 24-pixel cells (Xbox, PlayStation), then 32-pixel ones (the same); then a blank key of 24 and one of 32,
    then the letters (CW x CH each)"""
    img = Image.new('RGBA', (len(COLUMNS) * 32, 2 * 24 + 2 * 32 + 32 + CH), (0, 0, 0, 0))
    y = 0
    for n in (24, 32):
        for style in range(2):
            for i, col in enumerate(COLUMNS):
                img.alpha_composite(glyph(n, style, col), (i * n, y))
            y += n
    img.alpha_composite(key(24, '', False), (0, y))
    img.alpha_composite(key(32, '', False), (24, y))
    img.alpha_composite(chars(), (0, y + 32))
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
