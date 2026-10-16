"""Lays out the editable textures by what they show, in assets/textures and, at the same paths, in the layers
(assets/textures-hd, assets/textures-ai), so a picture, its hand-made version and its AI version stay side by side:

    environment/foliage/  ground/  walls/  sky-water/  props/  effects/
    text/  characters/  unsure/          (made by hand; unsure: look first)
    models/<archive entry>/              (character, monster and weapon models' sheets)
    ui/                                  (interface: 2D pictures)
    other/                               (effects files, script resources, made at run time)
    unsorted/                            (new pictures not classified yet)

File names: <archive entry>[r<resource>]_<offset>_<content hash, 8 digits>.png (models/<entry>/<offset>_<hash>.png).

What a picture shows comes from data/texture-classes.tsv (path, class, note; classes are the folder names above,
e.g. environment/foliage); pictures it does not list go by rule (2D -> ui, model files -> models, others ->
unsorted). The build does not care where pictures are: psptex pack reads their paths from assets/textures/index.tsv
and finds replacements by picture. So moving a picture is: the file in every layer, its index row, the layers'
manifests (enhanced.tsv, matched.tsv, manual.tsv, verified.txt) and the class list. data/texture-environment.txt (what
scripts/enhance_textures.py enhances) is written again from the class list.

    python scripts/organize_textures.py --dry-run     # what would move
    python scripts/organize_textures.py               # move (journal: data/texture-moves/<time>.tsv)
    python scripts/organize_textures.py --undo data/texture-moves/<time>.tsv

To reclassify a picture, change its class in data/texture-classes.tsv and run it again. Nothing is deleted: a picture
whose target is taken by a different file stays where it is (reported).
"""

import argparse
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'assets'
TEXTURES = ASSETS / 'textures'
LAYERS = [ASSETS / 'textures-hd', ASSETS / 'textures-ai']
CLASSES = ROOT / 'data/texture-classes.tsv'
ENVIRONMENT = ROOT / 'data/texture-environment.txt'
MOVES = ROOT / 'data/texture-moves'
MANIFESTS = ['enhanced.tsv', 'matched.tsv', 'manual.tsv', 'verified.txt']
MODELS = range(3200, 3700)  # archive entries of character, monster and weapon models


def set_root(root):
    global ROOT, ASSETS, TEXTURES, LAYERS, CLASSES, ENVIRONMENT, MOVES
    ROOT = root
    ASSETS = ROOT / 'assets'
    TEXTURES = ASSETS / 'textures'
    LAYERS = [ASSETS / 'textures-hd', ASSETS / 'textures-ai']
    CLASSES = ROOT / 'data/texture-classes.tsv'
    ENVIRONMENT = ROOT / 'data/texture-environment.txt'
    MOVES = ROOT / 'data/texture-moves'


def read_lines(path):
    return path.read_text(encoding='utf-8').split('\n') if path.exists() else []


def write_lines(path, lines):
    path.write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def index_rows():
    """(line number, columns) of every picture row of index.tsv."""
    lines = read_lines(TEXTURES / 'index.tsv')
    return lines, [(i, l.split('\t')) for i, l in enumerate(lines) if l and not l.startswith('#')]


def source(row):
    """'02598', '01525r23', 'runtime': where the picture comes from (index columns entry, resource)."""
    entry, resource = row[6] if len(row) > 6 else '-', row[7] if len(row) > 7 else '-'
    if entry == '-':
        return 'runtime'
    return entry + ('r' + resource if resource != '-' else '')


def offset(row):
    return '%06X' % int(row[8], 16) if len(row) > 8 and row[8] not in ('', '-') else '000000'


def target(row, cls):
    name = '%s_%s_%s.png' % (source(row), offset(row), row[1][:8])
    if cls == 'models':
        return 'models/%s/%s_%s.png' % (source(row), offset(row), row[1][:8])
    return '%s/%s' % (cls, name)


def default_class(row):
    if len(row) > 5 and row[5] == '2d':
        return 'ui'
    entry = row[6] if len(row) > 6 else '-'
    if entry.isdigit() and int(entry) in MODELS:
        return 'models'
    return 'unsorted'


def replace_paths(path, moved):
    """Fields of a manifest naming a moved picture (alone, or after '# ...: ') get its new path."""
    if not path.exists():
        return 0
    lines, changed = read_lines(path), 0
    for i, line in enumerate(lines):
        fields = line.split('\t')
        for j, f in enumerate(fields):
            head, sep, tail = f.rpartition(' ')
            key = tail if sep else f
            if key in moved:
                fields[j] = head + sep + moved[key]
                changed += 1
        lines[i] = '\t'.join(fields)
    if changed:
        write_lines(path, lines)
    return changed


def move_files(moves, dry_run):
    """moves: [(old, new)] relative paths. Moves each in assets/textures and every layer; returns what moved."""
    done, blocked = {}, []
    for old, new in moves:
        roots = [r for r in [TEXTURES] + LAYERS if (r / old).exists()]
        if not (TEXTURES / old).exists():
            blocked.append((old, 'not in assets/textures'))
            continue
        clash = [r for r in roots if (r / new).exists() and (r / new).read_bytes() != (r / old).read_bytes()]
        if clash:
            blocked.append((old, 'a different file is at %s in %s' % (new, clash[0].name)))
            continue
        if not dry_run:
            for r in roots:
                (r / new).parent.mkdir(parents=True, exist_ok=True)
                if (r / new).exists():  # the same file already there (an earlier, interrupted run)
                    os.remove(r / old)
                else:
                    os.replace(r / old, r / new)
        done[old] = new
    return done, blocked


def prune(dirs):
    """Folders left empty by the moves (only those: nothing else is removed)."""
    for d in sorted(dirs, key=lambda p: -len(p.parts)):
        while d not in [TEXTURES] + LAYERS and d.exists() and not any(d.iterdir()):
            d.rmdir()
            d = d.parent


def apply(moved, dry_run, journal_name):
    """After the files: index.tsv, manifests, class list, environment list, journal."""
    if dry_run or not moved:
        return
    lines, rows = index_rows()
    for i, cols in rows:
        if cols[0] in moved:
            cols[0] = moved[cols[0]]
            lines[i] = '\t'.join(cols)
    write_lines(TEXTURES / 'index.tsv', lines)
    for layer in LAYERS:
        for m in MANIFESTS:
            replace_paths(layer / m, moved)
    replace_paths(CLASSES, moved)
    write_environment()
    MOVES.mkdir(parents=True, exist_ok=True)
    write_lines(MOVES / journal_name, ['# organize_textures.py: old path\tnew path'] +
                ['%s\t%s' % kv for kv in moved.items()] + [''])
    dirs = set()
    for old in moved:
        for r in [TEXTURES] + LAYERS:
            dirs.add((r / old).parent)
    prune(dirs)


def write_environment():
    envs = [l.split('\t')[0] for l in read_lines(CLASSES)
            if l and not l.startswith('#') and len(l.split('\t')) > 1 and l.split('\t')[1].startswith('environment')]
    write_lines(ENVIRONMENT, ['# environment pictures the local AI enhances (scripts/enhance_textures.py --only); '
                              'written from data/texture-classes.tsv by scripts/organize_textures.py'] + envs + [''])


def organize(dry_run):
    _, rows = index_rows()
    by_path = {cols[0]: cols for _, cols in rows}
    classes = {}
    for line in read_lines(CLASSES):
        cols = line.split('\t')
        if line and not line.startswith('#') and len(cols) > 1 and cols[0] in by_path:
            classes[cols[0]] = cols[1]
    moves = []
    for path, cols in by_path.items():
        cls = classes.get(path) or default_class(cols)
        new = target(cols, cls)
        if new != path:
            moves.append((path, new))
    moved, blocked = move_files(moves, dry_run)
    apply(moved, dry_run, time.strftime('%Y%m%d-%H%M%S') + '.tsv')
    counts = {}
    for path, cols in by_path.items():
        cls = classes.get(path) or default_class(cols)
        counts[cls] = counts.get(cls, 0) + 1
    for cls in sorted(counts):
        print('%6d  %s' % (counts[cls], cls))
    print('%s %d pictures (with their layer files), %d already in place, %d left where they are'
          % ('would move' if dry_run else 'moved', len(moved), len(by_path) - len(moves), len(blocked)))
    for old, why in blocked[:20]:
        print('  not moved: %s (%s)' % (old, why))


def undo(journal, dry_run):
    pairs = [l.split('\t') for l in read_lines(journal) if l and not l.startswith('#')]
    moved, blocked = move_files([(new, old) for old, new in pairs], dry_run)
    apply(moved, dry_run, 'undo-' + journal.name)
    print('%s %d pictures back, %d left where they are' % ('would move' if dry_run else 'moved', len(moved), len(blocked)))
    for old, why in blocked[:20]:
        print('  not moved: %s (%s)' % (old, why))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--dry-run', action='store_true', help='only say what would move')
    ap.add_argument('--undo', type=Path, metavar='JOURNAL', help='move the pictures of a journal back')
    ap.add_argument('--root', type=Path, help='another checkout (with assets/ and data/) instead of this one (tests)')
    args = ap.parse_args()
    if args.root:
        set_root(args.root.resolve())
    if not (TEXTURES / 'index.tsv').exists():
        sys.exit('%s not found' % (TEXTURES / 'index.tsv'))
    if args.undo:
        undo(args.undo, args.dry_run)
    else:
        organize(args.dry_run)


if __name__ == '__main__':
    main()
