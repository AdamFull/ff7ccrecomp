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
ccatel unpack  file.atel dir/           # module.txt + names.bin + code.bin + res/
ccatel pack    dir/ file.atel           # rebuild (names re-encrypted unless flags bit 0 is set)
ccatel verify  extracted/pkg/files      # check every module rebuilds byte for byte
```

Licensed under the MIT License. *Crisis Core: Final Fantasy VII* is a trademark of Square Enix;
this project is not affiliated with or endorsed by Square Enix.
