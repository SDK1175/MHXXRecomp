# Original 32x32 pixel crest for the MHXX Recomp launcher: two crossed blades ("double cross") on a navy shield
from PIL import Image
N = 32
im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
px = im.load()
NAVY, NAVY2, GOLD, GOLD2, GOLD3, DARK = (14, 30, 72, 255), (26, 52, 112, 255), (232, 184, 86, 255), (168, 118, 42, 255), (255, 232, 160, 255), (6, 10, 24, 255)
STEEL, STEEL2, EDGE = (226, 232, 244, 255), (138, 150, 176, 255), (255, 255, 255, 255)
def put(x, y, c):
    if 0 <= x < N and 0 <= y < N: px[x, y] = c
def inside(x, y):
    if y < 3 or y > 30: return False
    half = 13.5 if y < 17 else 13.5 - (y - 16) * 13.5 / 14
    return abs(x - 15.5) <= half
for y in range(N):
    for x in range(N):
        if inside(x, y):
            edge = not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1))
            put(x, y, GOLD if edge else (NAVY2 if y < 11 else NAVY))
for y in range(N):
    for x in range(N):
        if px[x, y] in (NAVY, NAVY2) and any(0 <= x+dx < N and 0 <= y+dy < N and px[x+dx, y+dy] == GOLD for dx, dy in ((1,0),(-1,0),(0,1),(0,-1))):
            put(x, y, DARK)
# gold highlight on the top rim
for x in range(4, 28): 
    if px[x, 3] == GOLD: put(x, 3, GOLD3)
def sword(mirror):  # hilt in the top corner, the blade down across the shield
    f = (lambda x: 31 - x) if mirror else (lambda x: x)
    GRIP, GRIP2 = (120, 72, 36, 255), (84, 48, 22, 255)
    put(f(5), 5, GOLD3); put(f(6), 5, GOLD); put(f(5), 6, GOLD2)  # pommel
    for i in range(2):  # grip
        put(f(6 + i), 6 + i, GRIP); put(f(7 + i), 6 + i, GRIP2)
    for k in range(-3, 4):  # guard across, centred on (9,9)
        put(f(9 + k), 9 - k, GOLD if abs(k) < 3 else GOLD2)
        if abs(k) < 3: put(f(9 + k + 1), 9 - k, GOLD2)
    for i in range(12):  # blade: (10,10) .. (21,21), two wide
        x, y = 10 + i, 10 + i
        put(f(x), y, STEEL); put(f(x + 1), y, STEEL2); put(f(x), y + 1, STEEL2)
    put(f(22), 22, EDGE)  # tip
sword(False); sword(True)
for (x, y, c) in ((15, 14, GOLD), (16, 14, GOLD), (14, 15, GOLD), (15, 15, GOLD3), (16, 15, GOLD3), (17, 15, GOLD), (15, 16, GOLD2), (16, 16, GOLD2)):
    put(x, y, c)
im.save('crest32.png')
big = im.resize((256, 256), Image.NEAREST)
big.save('mhxx-launcher-256.png')
im.resize((128, 128), Image.NEAREST).save('mhxx-launcher-128.png')
# ico: the 32 px art at whole multiples (32 64 128 256) and smoothed down for 16 / 24 / 48
frames = {s: (im.resize((s, s), Image.NEAREST) if s % 32 == 0 else im.resize((s, s), Image.LANCZOS)) for s in (16, 24, 32, 48, 64, 128, 256)}
frames[256].save('mhxx-launcher.ico', sizes=[(s, s) for s in frames], append_images=[frames[s] for s in (16, 24, 32, 48, 64, 128)])
pv = Image.new('RGBA', (720, 280), (240, 240, 240, 255))
pv.alpha_composite(big, (8, 12)); dark = Image.new('RGBA', (440, 280), (30, 30, 36, 255)); dark.alpha_composite(big, (12, 12))
for i, s in enumerate((16, 24, 32, 48)): dark.alpha_composite(frames[s], (290 + i * 0, 20 + sum((16, 24, 32, 48)[:i]) + i * 10))
pv.alpha_composite(dark, (280, 0)); pv.save('preview.png')
