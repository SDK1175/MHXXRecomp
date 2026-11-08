"""Compresses a texture pack: each <key>.png in the pack folder gets a <key>.dds beside it, in the GPU's own block
format with its mip levels, which the game loads in place of the PNG (smaller on disk, and 4-8 times smaller in
graphics memory, as the game's own textures are).

    python tools/texpack/compress.py <pack folder> <textures folder> [--delete-png] [--quality]

<pack folder>: the textures/load the pictures are in. <textures folder>: mhxx-texextract's output, whose index.txt
says which are normal maps and masks. Formats: colour BC1 (no transparency) or BC7 (with it; --quality: BC7 for all
colour), normal maps BC5 (red and green), masks BC4 (one channel). --delete-png: the PNGs removed once their DDS is
written (keep them if you may want to redo it in another format). Already compressed ones are skipped.

Windows: Microsoft's texconv (DirectXTex, MIT) does the encoding, on the graphics card for BC7; it is downloaded from
its GitHub release the first time, into tools/texpack/texconv. Needs Pillow: pip install pillow.
"""
import argparse
import os
import subprocess
import sys
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit('Pillow is needed: pip install pillow')

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.join(HERE, 'texconv')
TOOL_URL = 'https://github.com/microsoft/DirectXTex/releases/latest/download/texconv.exe'
CHUNK = 300


def texconv():
    exe = os.path.join(TOOL_DIR, 'texconv.exe')
    if os.path.exists(exe):
        return exe
    if os.name != 'nt':
        sys.exit(f'texconv runs on Windows: put texconv.exe in {TOOL_DIR}')
    os.makedirs(TOOL_DIR, exist_ok=True)
    print(f'downloading texconv ({TOOL_URL})')
    urllib.request.urlretrieve(TOOL_URL, exe)
    return exe


def kinds(src):
    k = {}
    with open(os.path.join(src, 'index.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split(' ')
            if len(p) >= 3:
                k[p[0]] = p[2]
    return k


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pack')
    ap.add_argument('src')
    ap.add_argument('--delete-png', action='store_true')
    ap.add_argument('--quality', action='store_true', help='BC7 for all colour textures (twice BC1\'s size)')
    a = ap.parse_args()
    pack = os.path.abspath(a.pack)
    kind = kinds(a.src)
    groups = {}  # format -> [png paths]
    pngs = []
    for root, _, files in os.walk(pack):
        if os.path.basename(root).startswith('_'):
            continue
        for f in files:
            if len(f) == 20 and f.lower().endswith('.png'):
                pngs.append(os.path.join(root, f))
    done = 0
    for p in pngs:
        dds = p[:-4] + '.dds'
        if os.path.exists(dds):
            done += 1
            continue
        k = kind.get(os.path.basename(p)[:16].lower(), 'color')
        if k == 'normal':
            fmt = 'BC5_UNORM'
        elif k == 'mask':
            fmt = 'BC4_UNORM'
        elif a.quality:
            fmt = 'BC7_UNORM'
        else:
            with Image.open(p) as im:
                opaque = im.mode != 'RGBA' or im.getchannel('A').getextrema()[0] >= 250
            fmt = 'BC1_UNORM' if opaque else 'BC7_UNORM'
        groups.setdefault(fmt, []).append(p)
    todo = sum(len(v) for v in groups.values())
    print(f'{len(pngs)} pictures: {done} already compressed, {todo} to do (' +
          ', '.join(f'{len(v)} {f[:3]}' for f, v in sorted(groups.items())) + ')')
    if not todo:
        return
    exe = texconv()
    n = 0
    for fmt, files in sorted(groups.items()):
        for c0 in range(0, len(files), CHUNK):
            part = files[c0:c0 + CHUNK]
            by_dir = {}
            for p in part:
                by_dir.setdefault(os.path.dirname(p), []).append(p)
            for d, fs in by_dir.items():
                lst = os.path.join(pack, '_texconv_list.txt')
                with open(lst, 'w', encoding='utf-8') as f:
                    f.write('\n'.join(fs) + '\n')
                r = subprocess.run([exe, '-nologo', '-y', '-m', '0', '-f', fmt, '-o', d, '-flist', lst],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
                os.remove(lst)
                if r.returncode:
                    sys.exit(f'texconv failed ({r.returncode}):\n{r.stdout[-2000:]}')
            for p in part:
                dds = p[:-4] + '.dds'
                if not os.path.exists(dds):
                    print(f'  {p}: no DDS written')
                elif a.delete_png:
                    os.remove(p)
            n += len(part)
            print(f'  {n}/{todo}')
    print('done')


if __name__ == '__main__':
    main()
