#pragma once

// The game's code lives in namespace ff7cc and uses the runtime's (psprecore) names as its own: io::, rt::,
// platform::, path_join, Error...
namespace psprecore {}

namespace ff7cc {
using namespace psprecore;
}  // namespace ff7cc
