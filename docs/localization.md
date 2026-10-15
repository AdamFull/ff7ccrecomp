# Text, translation and mods

The game's dialogue and menu text lives in **MBD message tables**: 664 tables with 11,667 messages, stored as
resources inside the Atel script modules of `discimg.pkg`. The European disc (ULES-01044) contains English text
only; the game never asks the system for a language.

## Exporting and translating

```
cctext export "Crisis Core - Final Fantasy VII EU" text_en.po
```

This writes a gettext PO file, which Poedit, Weblate, Lokalize and similar tools open directly. Each message has a
context `EEEEE.RR.MMM` (archive entry, resource, message) and the English text as `msgid`; the translation goes
into `msgstr`. Messages left empty or unchanged keep the original text.

```
cctext build "Crisis Core - Final Fantasy VII EU" text_fr.po mods/french
cctext verify "Crisis Core - Final Fantasy VII EU"     # round-trip check of every message
```

`build` rewrites only the modules whose text changed and writes them to `mods/french/pkg/NNNNN.atel`.

Scripts address messages by byte offset inside their table (for example `Mes(2128, 18, ...)` shows the message at
offset 2128 of resource 18), and some resources by their offset in the module. So `build` never moves anything: a
changed message that fits in its original space is written in place; a longer one keeps its header where it was and
its pages move to the end of the table; a table that grows is moved to the end of the module while every other
resource stays put. Verified in game: translated subtitles in the opening show, and the run stays identical
otherwise.

## Text syntax

| Text | Meaning |
|---|---|
| a line break | a new line in the text box |
| `{PAGE}` | end of a page: the game waits for a button, then clears the box |
| `{CHOICE:200}` | a choice list; the following lines are the options (the number is the game's argument, in hex) |
| `{C10}`, `{C11:0100}` | other control codes (hex number and optional argument): button icons, inserted names, item names and counts, values. Keep them, moving them where the sentence needs them |
| `{G:150}` | a glyph of the font by number, for glyphs that have no character assigned |

The font has `A–Z`, `a–z`, `0–9`, `! ? " ' , . : ; - / ( ) & < > % [ ] ¥ + # _ | ^ ° ~ * = ★ ☆ ℃ ㎝` and `△` (the
Triangle button in a circle): glyphs 0–95 of its sheet, which also holds an italic copy. Characters outside it
(accents, other scripts) are rejected by `cctext build` with the message they appear in. Supporting them is the
font work of Phase 9 (extended glyph sheets or a replaced text renderer).

## Mods

At startup the game reads mod folders from `mods/` next to the settings (`--mods DIR` chooses another place):

```
mods/
  french/
    pkg/01954.atel      replaces archive entry 1954
    textures/DEA7A4783C669CE6_zack.png      replaces a texture (Vulkan renderer)
```

Folders apply in name order; a later folder wins when two replace the same entry. Nothing on the disc is modified:
the game is given a patched `discimg.fse` over a virtual archive in which every entry keeps its original order
(entries after a resized one move). On Android the folder is `Android/data/com.ff7ccrecomp/files/mods/`.

### Textures

A PNG in `textures/` replaces the game texture whose content hash starts its name (16 hex digits; anything may
follow). `--tex-dump VBLANK:DIR` writes the textures drawn at that moment under exactly such names, so a dumped file
can be edited and dropped into a mod as it is. The picture may be any size up to 2048x2048: drawn at 2x or 4x the
original it simply looks sharper. Palette textures drawn in several colours have one hash per palette.

Without mods, "Upscale textures (2x)" in the pause menu enlarges the game's 3D textures when they are loaded, with
an edge-directed filter that keeps diagonal edges smooth; textures used for the 2D interface keep their pixels.
