// The game's own C library routines, replaced by host implementations (they are hot in profiles).

#include "psprecore/game/game.hpp"

using namespace psprecore::game;

// memcpy(dst, src, n) -> dst. The original copies one byte at a time, front to back; overlapping ranges keep
// that behaviour (a forward copy repeats the source pattern), everything else uses the host memcpy.
GAME_FUNC(08998F70, game_memcpy) {
    const u32 dst = arg(ctx, 0), src = arg(ctx, 1), n = arg(ctx, 2);
    u8* d = host(dst);
    const u8* s = host(src);
    if (d > s && d < s + n) {
        for (u32 i = 0; i < n; ++i) d[i] = s[i];
    } else if (n) {
        std::memmove(d, s, n);
    }
    ret(ctx, dst);
}
