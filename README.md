# FF7CCRECOMP

A native PC (Windows/Linux) and Android port of the PSP game *Crisis Core: Final Fantasy VII*,
built by static recompilation and progressive decompilation.

**This repository contains no game code or assets.** You need your own copy of the game
(EU, ULES-01044). Everything derived from it is generated locally and never committed.

- Work plan and ROM analysis: [PORT_PLAN.md](PORT_PLAN.md)
- Throwaway analysis scripts: [tools/analysis/](tools/analysis/)

## Building

Requirements: CMake ≥ 3.25, Ninja, LLVM/Clang. On Windows, also Visual Studio 2022 (for the Windows SDK and
CRT used by `clang-cl`). On Windows, `scripts\build.ps1` sets up that environment itself; otherwise run the
CMake commands from an *x64 Native Tools / Developer* prompt.

```
scripts\build.ps1                     # Windows: configure, build, run tests (-Preset, -NoTest)
cmake --preset windows-x64-debug      # or linux-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug
```

## Tools

`ccpkg` works on the game's main data archive (`PSP_GAME/USRDIR/discimg.fse` + `discimg.pkg`):

```
ccpkg info    <game>                    # summary by file type
ccpkg list    <game>                    # index, sector, size, type for every file
ccpkg extract <game> extracted/pkg      # files/NNNNN.<ext> + manifest.tsv
ccpkg pack    extracted/pkg out/        # rebuild (files may be edited or resized)
ccpkg verify  <game> out/               # compare two archives file by file
```

`<game>` can be the extracted disc root, `PSP_GAME`, `USRDIR`, or any folder with the two archive files.

`ccatel` works on the `Atel` script modules found inside the archive (`*.atel` after extraction):

```
ccatel info    file.atel                # objects, methods, entry points, resources
ccatel strings file.atel                # decrypted name/string pool
ccatel disasm  file.atel [out.txt]      # bytecode listing (labels, resolved names, native calls)
ccatel unpack  file.atel dir/           # module.txt + names.bin + code.asm (editable listing) + res/
ccatel pack    dir/ file.atel           # assemble code.asm and rebuild the module
ccatel verify  extracted/pkg/files      # check every module rebuilds byte for byte
```

`ccprx` works on the game executable (`PSP_GAME/SYSDIR/BOOT.BIN`, an unencrypted PRX):

```
ccprx info    BOOT.BIN                  # module info, segments, imports/exports, relocation stats
ccprx nids    "PSP SDK/pspsdk-MINPSPW/psp/sdk/lib" build/nids.tsv   # NID names from local SDK stubs
ccprx imports BOOT.BIN build/nids.tsv   # all 197 imported system functions with names
ccprx image   BOOT.BIN 0x08804000 build/boot.bin   # relocated memory image (PPSSPP's load base)
ccprx disasm  BOOT.BIN                  # objdump-compatible listing (scripts/check_allegrex.ps1 diffs it)
ccprx symbols BOOT.BIN build/nids.tsv build/symbols.tsv 0x08804000 build/ghidra_symbols.txt
```

`ccrecomp` translates BOOT.BIN into C (see [docs/recompiler.md](docs/recompiler.md)); the output goes to the
git-ignored `generated/` folder and is picked up by the build automatically:

```
ccrecomp "Crisis Core - Final Fantasy VII EU/PSP_GAME/SYSDIR/BOOT.BIN" generated
```

`cccook` converts the game's music, voice clips, and movies into modern formats. The output is git-ignored,
like everything else derived from the game:
- ATRAC3/ATRAC3plus audio becomes Ogg Vorbis.
- PSMF movies become AV1 (IVF) video plus Ogg Vorbis audio.

It needs the FFmpeg command-line tool, at cook time only. Runs can be resumed. The game looks for `cooked/` in
the working directory (or use `--cooked DIR`). Without it, music and movie audio play as silence.

```
cccook "Crisis Core - Final Fantasy VII EU" cooked
```

Running the game:

```
ff7cc "Crisis Core - Final Fantasy VII EU"            # window: arrows/WASD, K/L/J/I = ×/○/□/△, Q/E, Enter/Backspace
ff7cc <game> --headless --vblanks 3600 --screenshot-every 300 shots/f --press 900 start   # automated runs
```

The NID database is generated locally and not committed. `tools/analysis/nids_from_ppsspp.py` fills the
names the homebrew SDK lacks (e.g. `sceSasCore`) from a PPSSPP debug log.

Licensed under the MIT License. *Crisis Core: Final Fantasy VII* is a trademark of Square Enix;
this project is not affiliated with or endorsed by Square Enix.
