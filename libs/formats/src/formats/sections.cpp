#include "ff7cc/formats/sections.hpp"

#include <cstring>

#include "psprecore/error.hpp"

namespace ff7cc::sections {

namespace {

constexpr std::size_t kShift = 0x1000;  // sections after a resized one move by multiples of this

std::uint32_t read32(std::span<const u8> d, std::size_t at) {
    if (at + 4 > d.size()) throw Exception("sections: truncated");
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

void write32(std::span<u8> d, std::size_t at, std::uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

}  // namespace

nx::vector<Section> parse(std::span<const u8> file) {
    const std::size_t count = read32(file, 0);
    if (count == 0 || 4 + 8 * count > kBase) throw Exception("sections: not a section file");
    nx::vector<Section> out;
    for (std::size_t i = 0; i < count; ++i) {
        const Section s{kBase + read32(file, 4 + 8 * i), read32(file, 8 + 8 * i)};
        if (s.offset + s.size > file.size() || (i && s.offset < out.back().offset + out.back().size))
            throw Exception("sections: bad section list");
        out.push_back(s);
    }
    return out;
}

nx::vector<u8> replace(std::span<const u8> file, std::size_t index, std::span<const u8> data) {
    const nx::vector<Section> list = parse(file);
    if (index >= list.size()) throw Exception("sections: no such section");
    const Section& s = list[index];
    // How far the sections after it move: enough for the new size, in whole kShift steps (never back).
    const std::size_t room = (index + 1 < list.size() ? list[index + 1].offset : file.size()) - s.offset;
    const std::size_t shift = data.size() > room ? (data.size() - room + kShift - 1) / kShift * kShift : 0;
    nx::vector<u8> out(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(s.offset));
    out.insert(out.end(), data.begin(), data.end());
    out.resize(s.offset + room + shift, 0);
    const auto rest = file.subspan(s.offset + room);
    out.insert(out.end(), rest.begin(), rest.end());
    write32(out, 8 + 8 * index, static_cast<std::uint32_t>(data.size()));
    for (std::size_t i = index + 1; i < list.size(); ++i)
        write32(out, 4 + 8 * i, static_cast<std::uint32_t>(list[i].offset + shift - kBase));
    return out;
}

}  // namespace ff7cc::sections
