#pragma once

#include <cstdint>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace cctext {

// RGBA8, R in the low byte.
bool write_png(const nx::string& path, const std::uint32_t* rgba, int width, int height);
bool read_png(const nx::string& path, nx::vector<std::uint32_t>& rgba, int& width, int& height);

}  // namespace cctext
