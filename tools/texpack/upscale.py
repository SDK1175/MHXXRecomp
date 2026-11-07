"""Makes a texture pack: upscales textures that mhxx-texextract saved and puts them where the game loads them.

    python tools/texpack/upscale.py <textures folder> <pack folder> [--groups player,weapon,otomo] [--scale 4]

<textures folder>: mhxx-texextract's output (its PNGs and index.txt). <pack folder>: the game's textures/load
(build/textures/load, or textures/load next to mhgurecomp.exe). Colour textures are upscaled by Real-ESRGAN's
anime model (realesrgan-x4plus-anime: it keeps the hand-painted lines of the gear sharp), on the graphics card
through realesrgan-ncnn-vulkan (downloaded from its GitHub release the first time, into tools/texpack/realesrgan).
Normal maps and masks are resized smoothly instead (the model would invent bumps and edges in them).

--groups: the top folders of the textures' own names to do (index.txt's last column): player = armour, weapon,
otomo = Palico gear, enemy = monsters, stage = the maps, npc, effect. gui is never done (text gets smudged).
--scale: 4 (128 -> 512) or 2 (1024 -> 2048: --scale 2 --groups enemy). Textures larger than --max-side (default
256 at 4x, 1024 at 2x) are left as they are. What is already in the pack folder is skipped, so a run can be
stopped and started again.

Needs Pillow: pip install pillow.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

try:
    from PIL import Image
except ImportError:
    sys.exit('Pillow is needed: pip install pillow')

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.join(HERE, 'realesrgan')
TOOL_URL = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
MODEL = 'realesrgan-x4plus-anime'
CHUNK = 400  # textures per realesrgan run (progress, and a stop loses at most this many)


def tool_exe():
    exe = os.path.join(TOOL_DIR, 'realesrgan-ncnn-vulkan.exe' if os.name == 'nt' else 'realesrgan-ncnn-vulkan')
    if os.path.exists(exe):
        return exe
    if os.name != 'nt':
        sys.exit(f'put realesrgan-ncnn-vulkan (with its models folder) in {TOOL_DIR}')
    os.makedirs(TOOL_DIR, exist_ok=True)
    zpath = os.path.join(TOOL_DIR, 'realesrgan.zip')
    print(f'downloading realesrgan-ncnn-vulkan ({TOOL_URL})')
    urllib.request.urlretrieve(TOOL_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(TOOL_DIR)
    os.remove(zpath)
    if not os.path.exists(exe):
        sys.exit(f'{exe} not found after unpacking')
    return exe


def read_index(src):
    rows = []
    with open(os.path.join(src, 'index.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\r\n').split(' ', 5)
            if len(p) < 5:
                continue
            key, size, kind, arc, name = p[0], p[1], p[2], p[3], p[4]
            w, h = map(int, size.split('x'))
            rows.append((key, w, h, kind, name.split('\\')[0].lower()))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--groups', default='player,weapon,otomo')
    ap.add_argument('--scale', type=int, default=4, choices=(2, 4))
    ap.add_argument('--max-side', type=int, default=0)
    ap.add_argument('--gpu', default='auto', help='realesrgan -g (auto, 0, 1, ...)')
    a = ap.parse_args()
    groups = {g.strip().lower() for g in a.groups.split(',') if g.strip()} - {'gui'}
    max_side = a.max_side or (256 if a.scale == 4 else 1024)
    os.makedirs(a.out, exist_ok=True)

    todo = [r for r in read_index(a.src)
            if r[4] in groups and max(r[1], r[2]) <= max_side
            and not os.path.exists(os.path.join(a.out, r[0] + '.png'))]
    color = [r for r in todo if r[3] == 'color']
    other = [r for r in todo if r[3] != 'color']
    print(f'{len(todo)} textures to do ({len(color)} colour by AI, {len(other)} normal maps / masks resized), '
          f'x{a.scale}, groups {",".join(sorted(groups))}')

    for i, (key, w, h, kind, _) in enumerate(other):
        im = Image.open(os.path.join(a.src, key + '.png'))
        im.resize((w * a.scale, h * a.scale), Image.LANCZOS).save(os.path.join(a.out, key + '.png'))
        if (i + 1) % 500 == 0:
            print(f'  normal maps / masks: {i + 1}/{len(other)}')

    if not color:
        print('done')
        return
    exe = tool_exe()
    work = os.path.join(a.out, '_work')
    t0 = time.time()
    for c0 in range(0, len(color), CHUNK):
        part = color[c0:c0 + CHUNK]
        win, wout = os.path.join(work, 'in'), os.path.join(work, 'out')
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(win), os.makedirs(wout)
        alpha = {}
        for key, w, h, kind, _ in part:  # the model gets RGB; alpha is resized on its own and put back
            im = Image.open(os.path.join(a.src, key + '.png')).convert('RGBA')
            alpha[key] = im.getchannel('A')
            im.convert('RGB').save(os.path.join(win, key + '.png'))
        r = subprocess.run([exe, '-i', win, '-o', wout, '-n', MODEL, '-s', '4', '-f', 'png', '-g', a.gpu],
                           cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode:
            sys.exit(f'realesrgan failed ({r.returncode}):\n{r.stderr[-2000:]}')
        for key, w, h, kind, _ in part:
            up = os.path.join(wout, key + '.png')
            if not os.path.exists(up):
                print(f'  {key}: no output, skipped')
                continue
            im = Image.open(up).convert('RGB')
            size = (w * a.scale, h * a.scale)
            if im.size != size:
                im = im.resize(size, Image.LANCZOS)
            im.putalpha(alpha[key].resize(size, Image.LANCZOS))
            im.save(os.path.join(a.out, key + '.png'))
        done = c0 + len(part)
        el = time.time() - t0
        print(f'  colour: {done}/{len(color)}  ({el / 60:.1f} min, about {el / done * (len(color) - done) / 60:.0f} min left)')
    shutil.rmtree(work, ignore_errors=True)
    print('done')


if __name__ == '__main__':
    main()
