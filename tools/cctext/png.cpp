// PNG files for the font commands (stb_image, stb_image_write).

#include "png.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <cstring>

namespace cctext {

bool write_png(const nx::string& path, const std::uint32_t* rgba, int width, int height) {
    return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
}

bool read_png(const nx::string& path, nx::vector<std::uint32_t>& rgba, int& width, int& height) {
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) return false;
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::memcpy(rgba.data(), pixels, rgba.size() * 4);
    stbi_image_free(pixels);
    return true;
}

}  // namespace cctext
