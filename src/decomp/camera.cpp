// The field camera, turned freely by the port's camera look (a second stick, a drag on the touch screen).

#include <cmath>

#include "psprecore/game/game.hpp"
#include "psprecore/runtime/hle.hpp"

using namespace psprecore::game;

namespace {

// The game's sight test (f_08901CF8): whether walls hide the eye at camera + 192 from the target at + 208.
bool eye_hidden(Context* ctx, u32 camera, u32 followed) {
    return call(ctx, f_08901CF8, load<u32>(camera + 4) + 16, camera + 208, camera + 192, load<u32>(followed + 72)) != 0;
}

}  // namespace

// Field camera controller, once per game frame: field_camera(camera, manager, followed). `camera` (the camera
// manager + 528) holds the eye at +192 and the target at +208, which is placed +236 above `followed` (+32, its
// position). The original keeps the eye's horizontal offset from the target within a distance band, turns it by
// a fixed step while L or R is held, and when walls hide the eye swings it round to the clear side (up to half a
// turn). A look turn is applied before it runs by swinging the eye around the new target; a turn that would put
// the eye behind a wall is not taken, so the camera stops at walls as it does for L and R.
GAME_FUNC(088FD518, field_camera) {
    const u32 camera = arg(ctx, 0), followed = arg(ctx, 2);
    float yaw = 0, pitch = 0;
    if (followed && psprecore::rt::take_camera_look(yaw, pitch) && yaw != 0) {
        u32 saved[32];
        std::memcpy(saved, ctx->r, sizeof saved);
        const float tx = load<float>(followed + 32), tz = load<float>(followed + 40);
        store<float>(camera + 208, tx);  // the target as the original places it
        store<float>(camera + 212, load<float>(followed + 36) + load<float>(camera + 236));
        store<float>(camera + 216, tz);
        const float ex = load<float>(camera + 192), ez = load<float>(camera + 200);
        const float ox = ex - tx, oz = ez - tz;
        const float c = std::cos(yaw), s = std::sin(yaw);
        const bool was_hidden = eye_hidden(ctx, camera, followed);
        store<float>(camera + 192, tx + ox * c - oz * s);
        store<float>(camera + 200, tz + ox * s + oz * c);
        if (!was_hidden && eye_hidden(ctx, camera, followed)) {
            store<float>(camera + 192, ex);
            store<float>(camera + 200, ez);
        }
        std::memcpy(ctx->r, saved, sizeof saved);
    }
    GAME_ORIGINAL(088FD518);
}
