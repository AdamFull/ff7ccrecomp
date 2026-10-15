# How the game draws text

Findings about the European release's text renderer (CCFF7ENUK), as groundwork for fonts with more characters.
Addresses are guest addresses in a normal boot; the code addresses name the recompiled functions (`f_XXXXXXXX`).
Found with `--tex-dump` (glyph quads), `--ram-dump` and `--watch` (which functions read the width tables).

## Font sheets

The main font is a 256x256 texture of 4-bit palette indices, swizzled, loaded at boot to 0x09EC7C60. Its palette
(16 colours, 8888) is at 0x08A2B3C0 in the executable's data; other palettes give the coloured variants. Cells are
13x14 texels, 19 to a row (247 texels used):

| Rows | Contents |
|---|---|
| 0-5 | glyphs 0-95: `A-Z a-z 0-9` and punctuation (see docs/localization.md), glyph 95 the Triangle button |
| 6-11 | the same 96 glyphs in italics (sheet offset 84 = 6 rows) |
| 12-17, and the rest of rows 5 and 11 | empty |

A second, smaller font uses 11x15 cells, 23 to a row, in another sheet (regular, and italics 75 texels lower).

## Font records

The renderer finds fonts through a table of four pointers at 0x089AD85C (font id 0-3): regular, italic, small,
small italic. Each record:

| Offset | Value (regular) | Meaning |
|---|---|---|
| +0 | 96 | glyph count (not checked by the renderer) |
| +4 | 4 | bits per texel |
| +8 | 19 | cells per row |
| +12 | 13 | cell width |
| +16 | 14 | cell height |
| +20 | 0 | vertical offset of the font in its sheet (italics: 84) |
| +24 | 96 x u32 | advance of each glyph in pixels (e.g. `T` 8, `h` 6, space 5) |

Several UI objects keep their own copy of the regular record (26 copies with identical widths in the executable's
BSS, e.g. at 0x089AB1A8), so they measure text from their copy.

## Layout and drawing

- `f_088BADC0` lays a message out: it selects the font record by id, walks the MBD tokens (`0x0080` glyph,
  `0x..40` controls: 2 newline, 6-45 inserted values through a table at 0x08A28D30, 46-54 jump table), adds up each
  line's width from the advances (`record + 24 + 4 * glyph`) and centres lines in 480x272.
- `f_088BB260` emits one sprite per glyph: the whole 13x14 cell, with
  `u = glyph % columns * cell_width`, `v = glyph / columns * cell_height + offset`, advancing the pen by the glyph's
  width.

Neither bounds the glyph index. A glyph index of 96 or more reads its advance past the end of the width array, and
its cell from the free part of the sheet (for the regular font: indices 96-113 are the rest of row 5, 114-227 hit
the italic rows, 228-341 rows 12-17).

## Adding characters (plan)

1. Replace the font sheet (texture replacement, PORT_PLAN item 14) with one that fills the free cells with new
   glyphs (accented Latin, Cyrillic: up to ~150 cells for the regular font).
2. Give the renderer advances for the new indices: replace `f_088BADC0` / `f_088BB260` (and the copies' users) in
   C++, or point the font table at larger records.
3. Let `cctext` map the new characters to those indices (a charset file next to the mod's font).
