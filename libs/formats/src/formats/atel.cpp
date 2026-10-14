#include "ff7cc/formats/atel.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

#include "ff7cc/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace ff7cc::atel {

namespace {

constexpr char kMagic[4] = {'A', 't', 'e', 'l'};

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> data) : data_(data) {}

    std::size_t pos() const { return pos_; }
    void seek(std::size_t pos) {
        require(pos, 0);
        pos_ = pos;
    }

    template <typename T>
    T read() {
        require(pos_, sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    nx::vector<std::byte> bytes(std::size_t count) {
        require(pos_, count);
        nx::vector<std::byte> out(data_.begin() + pos_, data_.begin() + pos_ + count);
        pos_ += count;
        return out;
    }

private:
    void require(std::size_t at, std::size_t count) const {
        if (at > data_.size() || count > data_.size() - at)
            throw FormatError("Atel module truncated at offset " + std::to_string(at));
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

class Builder {
public:
    template <typename T>
    void put_value(T v) {
        static_assert(std::is_integral_v<T>, "put_value writes a single little-endian integer");
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        out_.insert(out_.end(), p, p + sizeof(T));
    }
    void put_bytes(std::span<const std::byte> bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }
    void zero_fill_to_multiple(std::size_t base, std::size_t alignment) {
        while ((out_.size() - base) % alignment != 0) out_.push_back(std::byte{0});
    }
    void patch_u32(std::size_t at, std::uint32_t v) { std::memcpy(out_.data() + at, &v, sizeof v); }
    std::size_t size() const { return out_.size(); }
    nx::vector<std::byte> take() { return std::move(out_); }

private:
    nx::vector<std::byte> out_;
};

std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

std::uint32_t checked_u32(std::size_t v, const char* what) {
    if (v > 0xFFFFFFFFu) throw FormatError(std::string(what) + " exceeds 4 GiB");
    return static_cast<std::uint32_t>(v);
}

}  // namespace

std::string_view Module::name_at(std::uint32_t offset) const {
    if (offset >= names.size()) throw FormatError("name offset " + std::to_string(offset) + " out of range");
    const auto* begin = reinterpret_cast<const char*>(names.data()) + offset;
    const auto* end = reinterpret_cast<const char*>(names.data()) + names.size();
    return std::string_view(begin, static_cast<std::size_t>(std::find(begin, end, '\0') - begin));
}

bool is_atel(std::span<const std::byte> data) {
    return data.size() >= sizeof kMagic && std::memcmp(data.data(), kMagic, sizeof kMagic) == 0;
}

void crypt_names(std::span<std::byte> names, std::uint32_t seed) {
    std::uint32_t state = seed;
    for (auto& b : names) {
        state = state * 0x41C64E6Du + 1234u;
        b ^= static_cast<std::byte>((state >> 16) & 0xFF);
    }
}

Module parse(std::span<const std::byte> data) {
    if (!is_atel(data)) throw FormatError("not an Atel module (bad magic)");
    Cursor in(data);
    in.seek(4);

    Module m;
    m.version_major = in.read<std::uint8_t>();
    m.version_minor = in.read<std::uint8_t>();
    m.reserved = in.read<std::uint8_t>();
    m.flags = in.read<std::uint8_t>();
    if (m.version_major != 1 || m.version_minor != 3)
        throw FormatError("unsupported Atel version " + std::to_string(m.version_major) + "." +
                          std::to_string(m.version_minor));
    const auto object_count = in.read<std::uint32_t>();
    const auto names_size = in.read<std::uint32_t>();
    const auto code_size = in.read<std::uint32_t>();
    m.seed = in.read<std::uint32_t>();

    if (object_count > data.size() / 12) throw FormatError("implausible Atel object count");
    m.objects.resize(object_count);
    for (auto& obj : m.objects) {
        const auto method_count = in.read<std::uint16_t>();
        obj.attributes = in.read<std::uint16_t>();
        obj.name = in.read<std::uint32_t>();
        obj.unknown = in.read<std::uint32_t>();
        obj.methods.resize(method_count);
        for (auto& method : obj.methods) {
            method.name = in.read<std::uint32_t>();
            method.code_offset = in.read<std::uint32_t>();
        }
    }

    m.names = in.bytes(names_size);
    if (!(m.flags & kFlagPlainNames)) crypt_names(m.names, m.seed);
    m.code = in.bytes(code_size);

    const std::size_t table = align_up(in.pos(), kResourceAlignment);
    m.code_padding = in.bytes(table - in.pos());

    const auto resource_count = in.read<std::uint32_t>();
    if (in.read<std::uint32_t>() != resource_count) throw FormatError("Atel resource counts disagree");
    if (resource_count > data.size() / 8) throw FormatError("implausible Atel resource count");
    struct Span {
        std::uint32_t offset, size;
    };
    nx::vector<Span> spans(resource_count);
    for (auto& s : spans) {
        s.offset = in.read<std::uint32_t>();
        s.size = in.read<std::uint32_t>();
    }

    const std::size_t base = in.pos();
    m.resources.resize(resource_count);
    for (std::size_t i = 0; i < resource_count; ++i) {
        const std::size_t begin = base + spans[i].offset;
        const std::size_t end = begin + spans[i].size;
        const std::size_t next = i + 1 < resource_count ? base + spans[i + 1].offset : data.size();
        if (spans[i].offset % kResourceAlignment != 0 || end > next || next > data.size())
            throw FormatError("Atel resource " + std::to_string(i) + " has an invalid offset or size");
        in.seek(begin);
        m.resources[i].data = in.bytes(spans[i].size);
        m.resources[i].slack = in.bytes(next - end);
    }

    for (const auto& obj : m.objects) {
        (void)m.name_at(obj.name);
        for (const auto& method : obj.methods) {
            (void)m.name_at(method.name);
            if (method.code_offset >= m.code.size()) throw FormatError("Atel method entry point out of range");
        }
    }
    return m;
}

nx::vector<std::byte> serialize(const Module& m) {
    Builder out;
    out.put_bytes(std::span(reinterpret_cast<const std::byte*>(kMagic), sizeof kMagic));
    out.put_value(m.version_major);
    out.put_value(m.version_minor);
    out.put_value(m.reserved);
    out.put_value(m.flags);
    out.put_value(checked_u32(m.objects.size(), "object count"));
    out.put_value(checked_u32(m.names.size(), "name table"));
    out.put_value(checked_u32(m.code.size(), "code"));
    out.put_value(m.seed);

    for (const auto& obj : m.objects) {
        if (obj.methods.size() > 0xFFFF) throw FormatError("too many methods in one Atel object");
        out.put_value(static_cast<std::uint16_t>(obj.methods.size()));
        out.put_value(obj.attributes);
        out.put_value(obj.name);
        out.put_value(obj.unknown);
        for (const auto& method : obj.methods) {
            out.put_value(method.name);
            out.put_value(method.code_offset);
        }
    }

    nx::vector<std::byte> names = m.names;
    if (!(m.flags & kFlagPlainNames)) crypt_names(names, m.seed);
    out.put_bytes(names);
    out.put_bytes(m.code);

    // Keep the original filler when it still lands on the boundary; otherwise zero-fill.
    if ((out.size() + m.code_padding.size()) % kResourceAlignment == 0 && m.code_padding.size() < kResourceAlignment)
        out.put_bytes(m.code_padding);
    else
        out.zero_fill_to_multiple(0, kResourceAlignment);

    const auto count = checked_u32(m.resources.size(), "resource count");
    out.put_value(count);
    out.put_value(count);
    const std::size_t table = out.size();
    for (std::size_t i = 0; i < m.resources.size(); ++i) {
        out.put_value(std::uint32_t{0});
        out.put_value(std::uint32_t{0});
    }

    const std::size_t base = out.size();
    for (std::size_t i = 0; i < m.resources.size(); ++i) {
        const auto& r = m.resources[i];
        out.zero_fill_to_multiple(base, kResourceAlignment);
        out.patch_u32(table + i * 8, checked_u32(out.size() - base, "resource offset"));
        out.patch_u32(table + i * 8 + 4, checked_u32(r.data.size(), "resource size"));
        out.put_bytes(r.data);
        out.put_bytes(r.slack);
    }
    out.zero_fill_to_multiple(0, kResourceAlignment);
    return out.take();
}

std::string_view resource_type(std::span<const std::byte> data) {
    struct Tag {
        std::string_view magic, type;
    };
    static constexpr Tag kTags[] = {{"MBD\0", "mbd"}, {"SSCF", "sscf"}, {"RIFF", "at3"}, {"LEAF", "leaf"}};
    for (const auto& tag : kTags)
        if (data.size() >= tag.magic.size() && std::memcmp(data.data(), tag.magic.data(), tag.magic.size()) == 0)
            return tag.type;
    return "bin";
}

nx::vector<std::byte> replace_resources(std::span<const std::byte> module,
                                         const nx::map<std::size_t, nx::vector<std::byte>>& replacements) {
    const Module m = parse(module);  // validates the layout
    std::size_t table = kHeaderSize;
    for (const auto& obj : m.objects) table += 12 + 8 * obj.methods.size();
    table = align_up(table + m.names.size() + m.code.size(), kResourceAlignment);
    const std::size_t count = m.resources.size(), base = table + 8 + 8 * count;

    nx::vector<std::byte> out(module.begin(), module.end());
    const auto read32 = [&](std::size_t at) {
        std::uint32_t v;
        std::memcpy(&v, out.data() + at, 4);
        return v;
    };
    const auto write32 = [&](std::size_t at, std::size_t v) {
        const auto u = checked_u32(v, "resource table entry");
        std::memcpy(out.data() + at, &u, 4);
    };
    for (const auto& [index, data] : replacements) {
        if (index >= count) throw Error("resource " + std::to_string(index) + " does not exist");
        const std::size_t entry = table + 8 + 8 * index;
        const std::size_t offset = read32(entry);
        const std::size_t space = (index + 1 < count ? read32(entry + 8) : module.size() - base) - offset;
        if (data.size() <= space) {
            std::memcpy(out.data() + base + offset, data.data(), data.size());
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(base + offset + data.size()),
                      out.begin() + static_cast<std::ptrdiff_t>(base + offset + space), std::byte{0});
        } else {
            out.resize(align_up(out.size(), kResourceAlignment));
            write32(entry, out.size() - base);
            out.insert(out.end(), data.begin(), data.end());
            out.resize(align_up(out.size(), kResourceAlignment));
        }
        write32(entry + 4, data.size());
    }
    return out;
}

}  // namespace ff7cc::atel
