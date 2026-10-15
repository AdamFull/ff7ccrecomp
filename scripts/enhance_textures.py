"""AI-enhanced textures for everything without a hand-made version: scene textures in assets/textures that are
unedited and have no picture in assets/textures-hd go through an image-editing model in ComfyUI
(scripts/comfyui_enhance.py, FLUX.2 [klein] 4B) and land in assets/textures-ai, which the build packs below the
hand-made layer (assets/textures edits > assets/textures-hd > assets/textures-ai).

    python scripts/enhance_textures.py --limit 20       # try a few first
    python scripts/enhance_textures.py                  # all the rest (about 5 s a texture on an 8 GB GPU)
    python scripts/enhance_textures.py --prompt-file my_prompt.txt

Pictures already in assets/textures-ai are never replaced (delete one to have it made again; edit one and it stays
yours). Each run writes review sheets (original | model | result) to data/enhance-review/ to spot bad ones.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def review(assets, layer, tmp, out_dir, per_sheet=12):
    from PIL import Image, ImageDraw
    names = sorted((tmp / 'in').glob('*.png'))
    listed = (tmp / 'list.txt').read_text(encoding='utf-8').splitlines() if (tmp / 'list.txt').exists() else []
    rows = []
    for inp, rel in zip(names, listed):
        fin, raw_path = layer / rel, tmp / 'out' / inp.name
        if not fin.exists() or not raw_path.exists():
            continue
        orig = Image.open(assets / rel).convert('RGBA')
        raw = Image.open(raw_path).convert('RGBA')
        pad = (Image.open(inp).width - orig.width) // 2
        k = raw.width / (orig.width + 2 * pad)
        raw = raw.crop((round(pad * k), round(pad * k), round((pad + orig.width) * k), round((pad + orig.height) * k)))
        h = 256
        w = max(1, int(h * orig.width / orig.height))
        row = Image.new('RGB', (3 * w + 20, h + 16), (40, 40, 40))
        for i, im in enumerate((orig, raw, Image.open(fin).convert('RGBA'))):
            t = im.resize((w, h), Image.NEAREST if i == 0 else Image.LANCZOS)
            bg = Image.new('RGBA', t.size, (255, 0, 255, 255))
            bg.alpha_composite(t)
            row.paste(bg.convert('RGB'), (i * (w + 10), 16))
        ImageDraw.Draw(row).text((2, 2), rel + '   original | model | result', fill=(255, 255, 0))
        rows.append(row)
    out_dir.mkdir(parents=True, exist_ok=True)
    sheets = 0
    for s in range(0, len(rows), per_sheet):
        chunk = rows[s:s + per_sheet]
        sheet = Image.new('RGB', (max(r.width for r in chunk), sum(r.height + 6 for r in chunk)), (0, 0, 0))
        y = 0
        for r in chunk:
            sheet.paste(r, (0, y))
            y += r.height + 6
        sheet.save(out_dir / ('review_%03d.png' % (s // per_sheet)))
        sheets += 1
    return sheets


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--limit', type=int, help='at most N textures')
    ap.add_argument('--prompt-file', type=Path, help='the instruction given with every texture')
    ap.add_argument('--detail', type=int, default=100,
                    help='percent of the original\'s colours and shapes pulled back where the model drifted '
                         '(psptex enhance --detail; default 100)')
    ap.add_argument('--strength', type=int, default=100,
                    help='percent of the model\'s new detail kept, the rest from the original smoothly enlarged '
                         '(psptex enhance --strength; default 100, lower is softer)')
    ap.add_argument('--2d', dest='with_2d', action='store_true', help='interface textures too')
    ap.add_argument('--redo', action='store_true',
                    help='make again the pictures an earlier run made that nobody changed since (edited ones stay)')
    ap.add_argument('--psptex', type=Path, default=ROOT / 'build/windows-x64-release/psprecore/tools/psptex/psptex.exe')
    args = ap.parse_args()

    tmp = ROOT / 'data/enhance-tmp'
    upscaler = '"%s" "%s" "{in}" "{out}"' % (sys.executable, ROOT / 'scripts/comfyui_enhance.py')
    if args.prompt_file:
        upscaler += ' --prompt-file "%s"' % args.prompt_file.resolve()
    cmd = [str(args.psptex), 'enhance', str(ROOT / 'assets/textures'), str(ROOT / 'assets/textures-ai'),
           '--skip-layer', str(ROOT / 'assets/textures-hd'), '--upscaler', upscaler, '--detail', str(args.detail),
           '--strength', str(args.strength),
           '--temp', str(tmp), '--keep-temp']
    if args.limit:
        cmd += ['--limit', str(args.limit)]
    if args.with_2d:
        cmd.append('--2d')
    if args.redo:
        cmd.append('--redo')
    code = subprocess.run(cmd).returncode
    sheets = review(ROOT / 'assets/textures', ROOT / 'assets/textures-ai', tmp, ROOT / 'data/enhance-review')
    print('review sheets: %d in %s' % (sheets, ROOT / 'data/enhance-review'), file=sys.stderr)
    sys.exit(code)


if __name__ == '__main__':
    main()
