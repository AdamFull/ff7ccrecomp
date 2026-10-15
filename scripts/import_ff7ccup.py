"""Hand-made high-resolution textures from FF7CCUP (github.com/eqprog/FF7CCUP, a PPSSPP texture pack for the US
release) mapped onto ours in assets/textures-hd, which the build packs where a picture in assets/textures is unedited.

Only hand-made pictures are taken: the interface, redrawn by hand (ui_upscale/), and scene textures someone edited
by hand. The pack's other scene textures are neural-network upscales. Which ones were edited comes from the pack's
history: a picture whose last change was a small commit (a few dozen files) that edits it, or that puts back an
earlier small edit (re-uploads after a bulk pass). Bulk commits - re-upscales with new models, batch transparency
fixes - touch hundreds of files at once.

    git clone --depth 1 https://github.com/eqprog/FF7CCUP data/third-party/FF7CCUP
    python scripts/import_ff7ccup.py

Needs the GitHub CLI (gh, logged in) for the history, cached in data/third-party/FF7CCUP-history.tsv for the
checked-out commit. psptex match compares every picture of the pack (an original whose closest picture is not
hand-made gets nothing) and lists matches and near misses in assets/textures-hd/matched.tsv. Pictures already in
assets/textures-hd are never removed or replaced (it is edited by hand); only missing ones are added.

The pack has no license; its interface readme asks for credit and allows changes. It stays out of this (public)
repository: data/ and assets/ are git-ignored here (assets/textures-hd is versioned in its own private repository).
"""

import argparse
import collections
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = 'eqprog/FF7CCUP'
SMALL = 40  # files in a commit: more is a bulk pass
EDIT = re.compile(r'edit|fix|update|noise|recreate|redraw|ornament|detail|priority|clean|rivet|wip|kunsel', re.I)

# Checked by eye: interface pictures redrawn further than psptex match accepts (a dropped (R) mark, glyphs drawn
# anew), as <our picture> -> <pack picture>. The DMW digits are otherwise unmatchable: the redrawn ones look as much
# alike as different digits do.
VERIFIED = {
    '00002.bin/000090_45429CFA1503C2B8.png': 'ui_upscale/square_logo.png',
    '00002.bin/000090_C288E7A025EA2C95.png': 'ui_upscale/square_logo.png',
    '00002.bin/000090_C9D5F7865B213820.png': 'ui_upscale/square_logo.png',
    # Save / load screen artwork (an upscale outside their interface folder matches it more closely)
    '01465.bin/051280_43E13C4971419BEA.png': 'ui_upscale/save.png',
    # DMW reel digits 1-7 (32x32; their N.png are 4x, N_big.png belong to a larger set)
    '01470.bin/0000C0_919F533A2496018D.png': 'ui_upscale/1.png',
    '01470.bin/0008E0_9F9D7D78D0A82AF8.png': 'ui_upscale/2.png',
    '01470.bin/001100_563791271013D62D.png': 'ui_upscale/3.png',
    '01470.bin/001920_64F366E6CD58AA2E.png': 'ui_upscale/4.png',
    '01470.bin/002140_2CFA1CE1DEF858F8.png': 'ui_upscale/5.png',
    '01470.bin/002960_3E18B14D772CAE5F.png': 'ui_upscale/6.png',
    '01470.bin/003180_8C0571810974B5E8.png': 'ui_upscale/7.png',
}


def gh(*args):
    out = subprocess.run(['gh', 'api', *args], check=True, capture_output=True, text=True, encoding='utf-8').stdout
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def history(pack, cache):
    """(sha, date, files in the commit, status, path, message) for every file change, newest commit first."""
    head = subprocess.run(['git', '-C', str(pack), 'rev-parse', 'HEAD'], check=True, capture_output=True,
                          text=True).stdout.strip()
    if cache.exists():
        lines = cache.read_text(encoding='utf-8').splitlines()
        if lines and lines[0] == '# ' + head:
            return [line.split('\t') for line in lines[1:]]
    print('fetching the history of', REPO, '(once per checkout)...', file=sys.stderr)
    commits = gh('--paginate', f'repos/{REPO}/commits?sha={head}&per_page=100',
                 '--jq', '.[] | {sha: .sha, date: .commit.author.date, msg: (.commit.message | split("\\n")[0])}')
    rows = []
    for i, c in enumerate(commits):
        files = gh('--paginate', f'repos/{REPO}/commits/{c["sha"]}', '--jq',
                   '.files[] | {status: .status, path: .filename}')
        for f in files:
            rows.append([c['sha'], c['date'], str(len(files)), f['status'], f['path'], c['msg'].replace('\t', ' ')])
        if i % 50 == 0:
            print(f'  {i}/{len(commits)} commits', file=sys.stderr)
    cache.write_text('# ' + head + '\n' + ''.join('\t'.join(r) + '\n' for r in rows), encoding='utf-8')
    return rows


def hand_made(rows):
    changes = collections.defaultdict(list)  # path -> [(date, files, message)]
    for sha, date, files, status, path, msg in rows:
        if status != 'removed':
            changes[path].append((date, int(files), msg))
    picked = []
    for path, h in changes.items():
        if not path.lower().endswith('.png'):
            continue
        h.sort()
        _, files, msg = h[-1]
        edited_before = any(n <= SMALL and EDIT.search(m) for _, n, m in h[:-1])
        if path.startswith('ui_upscale/') or (files <= SMALL and (EDIT.search(msg) or edited_before)):
            picked.append(path)
    return sorted(picked)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--pack', default=ROOT / 'data/third-party/FF7CCUP', type=Path)
    ap.add_argument('--psptex', default=ROOT / 'build/windows-x64-release/psprecore/tools/psptex/psptex.exe', type=Path)
    args = ap.parse_args()
    out = ROOT / 'assets/textures-hd'
    rows = history(args.pack, args.pack.parent / (args.pack.name + '-history.tsv'))
    # The interface as checked out (some of it was renamed or moved since it was added).
    ui = [p.relative_to(args.pack).as_posix() for p in (args.pack / 'ui_upscale').rglob('*.png')]
    picked = sorted(set(hand_made(rows)) | set(ui))
    only = args.pack.parent / (args.pack.name + '-handmade.txt')
    only.write_text('# hand-made pictures of ' + REPO + '\n' + '\n'.join(picked) + '\n', encoding='utf-8')
    print(len(picked), 'hand-made pictures in the pack', file=sys.stderr)
    subprocess.run([str(args.psptex), 'match', str(args.pack), str(ROOT / 'assets/textures'), str(out),
                    '--only', str(only)], check=True)
    # The verified pictures. assets/textures-hd is edited by hand: nothing there is ever removed or replaced, only
    # missing pictures are added (to take a new version, delete the picture by hand first).
    added = kept = 0
    for ours, theirs in VERIFIED.items():
        if not (ROOT / 'assets/textures' / ours).exists() or not (args.pack / theirs).exists():
            continue
        if (out / ours).exists():
            kept += 1
            continue
        (out / ours).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.pack / theirs, out / ours)
        added += 1
    credits = out / 'FF7CCUP UI readme (credits).txt'
    readme = args.pack / 'ui_upscale/Crisis Core UI Upscale Readme.txt'
    if readme.exists() and not credits.exists():  # their credits travel with the pictures
        shutil.copyfile(readme, credits)
    print(f'verified pictures: {added} added, {kept} already there (kept) - pack them with a build', file=sys.stderr)


if __name__ == '__main__':
    main()
