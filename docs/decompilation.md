# Editing the game in C++

Every function of BOOT.BIN is recompiled to C (`data/generated/`, see
[recompiler.md](../psprecore/docs/recompiler.md)). Any of them can be replaced by hand-written C++ in `src/decomp/`: the replacement is called wherever the game calls the original,
including through function pointers, and the recompiled original stays available.

## Writing a replacement

```cpp
#include "psprecore/game/game.hpp"

using namespace ff7cc::game;

// memcpy(dst, src, n) -> dst
GAME_FUNC(08998F70, game_memcpy) {
    const u32 dst = arg(ctx, 0), src = arg(ctx, 1), n = arg(ctx, 2);
    std::memmove(host(dst), host(src), n);
    ret(ctx, dst);
}
```

- `GAME_FUNC(XXXXXXXX, name)` defines the replacement for `f_XXXXXXXX` (8 hex digits, no `0x`). `ctx` is the
  guest CPU state: `ctx->r[n]` (GPRs), `ctx->f[n]` (FPU), `ctx->v[n]` (VFPU).
- `GAME_ORIGINAL(XXXXXXXX)` runs the recompiled original with the current registers, which makes wrappers easy
  (log, patch the arguments or the result, then call the original).
- Calling convention (PSP EABI): integer arguments in a0–a3 and t0–t3 (`arg(ctx, i)`), floats in f12–f19
  (`farg`), results in v0/v1 (`ret`) or f0 (`fret`). Like a MIPS function, a replacement may clobber the
  argument and temporary registers; it must preserve s0–s8, sp and fp if it changes them.
- Guest memory: `load<T>(addr)`, `store<T>(addr, value)`, `host(addr)` (a host pointer), `c_str(addr)`.
- Calling game code: `call(ctx, f_XXXXXXXX, args...)` or `call_address(ctx, addr, args...)` for function
  pointers the game holds; both return v0.

## Regenerating

The generator has to know which functions are replaced, so that it renames them to `f_XXXXXXXX_orig`:

```
psprecomp data/original/PSP_GAME/SYSDIR/BOOT.BIN data/generated --overrides src/decomp
```

Run it after adding or removing a `GAME_FUNC`. CMake checks this when configuring: if `data/generated/` does not know a
replacement yet, it warns and builds without `src/decomp/` until the code is regenerated.

## Checking a replacement

Replacements should keep the game's behaviour unless they mean to change it. A recorded play session checks
that exactly:

```
ff7cc <game> --headless --replay session.txt --state-hash 500 > before.txt   # before the change
ff7cc <game> --headless --replay session.txt --state-hash 500 > after.txt    # after it
```

The hashes cover the game's RAM; identical lists mean identical game state. For the `memcpy` replacement above,
all 38 checkpoints through the opening battles match.

## What to replace first

From the plan (PORT_PLAN.md, Phase 8): the file/archive layer, text and font rendering (localization), menu and
UI layout, camera and input (right stick), the main loop and frame pacing, the render front end, then battle,
DMW and materia logic. Profiles (`simpleperf` on Android, `--perf`) also point at hot routines such as the game's
own `memcpy`, which are easy first wins.
