#include "ff7cc/formats/mbd.hpp"

#include <cstring>

#include "ff7cc/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace ff7cc::mbd {

namespace {

std::uint32_t read32(std::span<const std::byte> d, std::size_t at) {
    if (at + 4 > d.size()) throw FormatError("MBD: truncated");
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

void put32(nx::vector<std::byte>& out, std::uint32_t v) {
    const auto* b = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), b, b + 4);
}

std::string hex(unsigned v, int digits) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string s(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i, v >>= 4) s[static_cast<std::size_t>(i)] = kDigits[v & 15];
    return s;
}

unsigned parse_number(std::string_view s, int base) {
    if (s.empty()) throw FormatError("MBD text: empty number in a tag");
    unsigned v = 0;
    for (const char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        if (d < 0) throw FormatError("MBD text: bad number '" + std::string(s) + "' in a tag");
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        if (v > 0xFFFF) throw FormatError("MBD text: number out of range in a tag");
    }
    return v;
}

}  // namespace

bool is_mbd(std::span<const std::byte> data) {
    return data.size() >= 4 && std::memcmp(data.data(), "MBD\0", 4) == 0;
}

nx::vector<Message> parse(std::span<const std::byte> data) {
    if (!is_mbd(data)) throw FormatError("MBD: bad magic");
    nx::vector<Message> out;
    std::size_t p = 4;
    while (p < data.size()) {
        const std::uint32_t page_count = read32(data, p);
        if (page_count == 0 || page_count > 64) throw FormatError("MBD: bad page count");
        Message m;
        std::size_t expected = 4 + 8 * static_cast<std::size_t>(page_count), end = 0;
        for (std::uint32_t i = 0; i < page_count; ++i) {
            const std::uint32_t length = read32(data, p + 4 + 8 * i), offset = read32(data, p + 8 + 8 * i);
            if (offset != expected) throw FormatError("MBD: pages are not contiguous");
            nx::vector<Token> page(length);
            for (std::uint32_t t = 0; t < length; ++t) {
                const std::uint32_t w = read32(data, p + offset + 4 * t);
                page[t] = {static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(w >> 16)};
            }
            const std::uint8_t terminator = i + 1 == page_count ? kMessageEnd : kPageEnd;
            if (page.empty() || !page.back().is_control() || page.back().control() != terminator || page.back().arg != 0)
                throw FormatError("MBD: page does not end with its terminator");
            expected = offset + 4 * static_cast<std::size_t>(length);
            end = p + expected;
            m.pages.push_back(std::move(page));
        }
        out.push_back(std::move(m));
        p = end;
    }
    if (p != data.size()) throw FormatError("MBD: trailing bytes");
    return out;
}

nx::vector<std::byte> serialize(const nx::vector<Message>& messages) {
    nx::vector<std::byte> out;
    for (const char c : {'M', 'B', 'D', '\0'}) out.push_back(static_cast<std::byte>(c));
    for (const auto& m : messages) {
        put32(out, static_cast<std::uint32_t>(m.pages.size()));
        std::uint32_t offset = 4 + 8 * static_cast<std::uint32_t>(m.pages.size());
        for (const auto& page : m.pages) {
            put32(out, static_cast<std::uint32_t>(page.size()));
            put32(out, offset);
            offset += 4 * static_cast<std::uint32_t>(page.size());
        }
        for (const auto& page : m.pages)
            for (const Token& t : page) put32(out, static_cast<std::uint32_t>(t.code) | (static_cast<std::uint32_t>(t.arg) << 16));
    }
    return out;
}

nx::vector<std::byte> patch(std::span<const std::byte> original, const nx::map<std::size_t, Message>& changes) {
    const auto messages = parse(original);
    // Start offset and size of every message in the original table.
    nx::vector<std::size_t> starts, sizes;
    for (std::size_t p = 4; p < original.size();) {
        const std::uint32_t pages = read32(original, p);
        const std::uint32_t last_length = read32(original, p + 4 + 8 * (pages - 1));
        const std::uint32_t last_offset = read32(original, p + 8 + 8 * (pages - 1));
        starts.push_back(p);
        sizes.push_back(last_offset + 4 * static_cast<std::size_t>(last_length));
        p += sizes.back();
    }
    nx::vector<std::byte> out(original.begin(), original.end());
    const auto set32 = [&](std::size_t at, std::size_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        std::memcpy(out.data() + at, &u, 4);
    };
    for (const auto& [index, message] : changes) {
        if (index >= messages.size()) throw Error("MBD: message " + std::to_string(index) + " does not exist");
        if (message.pages.empty()) throw Error("MBD: a message needs at least one page");
        const auto bytes = serialize({message});  // "MBD\0" + the message, contiguous
        const std::size_t length = bytes.size() - 4, start = starts[index], space = sizes[index];
        if (length <= space) {
            std::memcpy(out.data() + start, bytes.data() + 4, length);
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(start + length),
                      out.begin() + static_cast<std::ptrdiff_t>(start + space), std::byte{0});
            continue;
        }
        const std::size_t header = 4 + 8 * message.pages.size();
        if (header > space)
            throw FormatError("MBD: message " + std::to_string(index) + " has too many pages for its original place (" +
                              std::to_string(message.pages.size()) + ")");
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(start), out.begin() + static_cast<std::ptrdiff_t>(start + space),
                  std::byte{0});
        set32(start, message.pages.size());
        for (std::size_t pg = 0; pg < message.pages.size(); ++pg) {
            set32(start + 4 + 8 * pg, message.pages[pg].size());
            set32(start + 8 + 8 * pg, out.size() - start);  // the page data goes to the end of the table
            for (const Token& t : message.pages[pg]) {
                const std::uint32_t w = static_cast<std::uint32_t>(t.code) | (static_cast<std::uint32_t>(t.arg) << 16);
                const auto* b = reinterpret_cast<const std::byte*>(&w);
                out.insert(out.end(), b, b + 4);
            }
        }
    }
    return out;
}

Charset Charset::european() {
    Charset c;
    for (int i = 0; i < 26; ++i) {
        c.glyphs[static_cast<std::size_t>(i)] = std::string(1, static_cast<char>('A' + i));
        c.glyphs[static_cast<std::size_t>(26 + i)] = std::string(1, static_cast<char>('a' + i));
    }
    for (int i = 0; i < 10; ++i) c.glyphs[static_cast<std::size_t>(52 + i)] = std::string(1, static_cast<char>('0' + i));
    const std::pair<int, const char*> punctuation[] = {
        {62, "!"}, {63, "?"}, {64, " "}, {65, "\""}, {66, "'"}, {67, ","}, {68, "."}, {69, ":"},
        {71, "-"}, {72, "/"}, {73, "("}, {74, ")"}, {78, "%"}, {83, "#"}, {89, "*"},
    };
    for (const auto& [i, s] : punctuation) c.glyphs[static_cast<std::size_t>(i)] = s;
    return c;
}

std::string to_text(const Message& message, const Charset& charset) {
    std::string out;
    for (std::size_t p = 0; p < message.pages.size(); ++p) {
        if (p) out += "{PAGE}";
        const auto& page = message.pages[p];
        for (std::size_t i = 0; i + 1 < page.size(); ++i) {  // the last token is the page's terminator
            const Token& t = page[i];
            if (t.is_glyph() && t.arg < charset.glyphs.size() && !charset.glyphs[t.arg].empty()) {
                out += charset.glyphs[t.arg];
            } else if (t.is_glyph()) {
                out += "{G:" + std::to_string(t.arg) + "}";
            } else if (t.is_control() && t.control() == kNewline && t.arg == 0) {
                out += '\n';
            } else if (t.is_control() && t.control() == kChoice) {
                out += "{CHOICE:" + hex(t.arg, t.arg > 0xFFF ? 4 : 3) + "}";
            } else if (t.is_control()) {
                out += "{C" + hex(t.control(), 2) + (t.arg ? ":" + hex(t.arg, 4) : "") + "}";
            } else {
                out += "{T:" + hex(t.code, 4) + ":" + hex(t.arg, 4) + "}";  // not seen in the game's data
            }
        }
    }
    return out;
}

Message from_text(std::string_view text, const Charset& charset) {
    nx::map<std::string, std::uint16_t, std::less<>> encode;
    std::size_t longest = 1;
    for (std::size_t i = 0; i < charset.glyphs.size(); ++i)
        if (!charset.glyphs[i].empty()) {
            encode.emplace(charset.glyphs[i], static_cast<std::uint16_t>(i));
            longest = std::max(longest, charset.glyphs[i].size());
        }

    Message m;
    m.pages.emplace_back();
    for (std::size_t i = 0; i < text.size();) {
        auto& page = m.pages.back();
        if (text[i] == '\n') {
            page.push_back(Token::make_control(kNewline));
            ++i;
            continue;
        }
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close == std::string_view::npos) throw FormatError("MBD text: unterminated tag");
            const std::string_view tag = text.substr(i + 1, close - i - 1);
            const std::size_t colon = tag.find(':');
            const std::string_view name = tag.substr(0, colon);
            const std::string_view value = colon == std::string_view::npos ? std::string_view{} : tag.substr(colon + 1);
            if (tag == "PAGE") {
                page.push_back(Token::make_control(kPageEnd));
                m.pages.emplace_back();
            } else if (name == "CHOICE") {
                page.push_back(Token::make_control(kChoice, static_cast<std::uint16_t>(parse_number(value, 16))));
            } else if (name == "G") {
                page.push_back({kGlyph, static_cast<std::uint16_t>(parse_number(value, 10))});
            } else if (name.size() == 3 && name[0] == 'C') {
                const unsigned number = parse_number(name.substr(1), 16);
                page.push_back(Token::make_control(static_cast<std::uint8_t>(number),
                                                   static_cast<std::uint16_t>(value.empty() ? 0 : parse_number(value, 16))));
            } else if (name == "T") {
                const std::size_t second = value.find(':');
                if (second == std::string_view::npos) throw FormatError("MBD text: bad {T:code:arg} tag");
                page.push_back({static_cast<std::uint16_t>(parse_number(value.substr(0, second), 16)),
                                static_cast<std::uint16_t>(parse_number(value.substr(second + 1), 16))});
            } else {
                throw FormatError("MBD text: unknown tag {" + std::string(tag) + "}");
            }
            i = close + 1;
            continue;
        }
        bool matched = false;
        for (std::size_t len = std::min(longest, text.size() - i); len > 0 && !matched; --len) {
            const auto it = encode.find(text.substr(i, len));
            if (it == encode.end()) continue;
            page.push_back({kGlyph, it->second});
            i += len;
            matched = true;
        }
        if (!matched) {
            std::size_t len = 1;  // report the whole UTF-8 character
            while (i + len < text.size() && (static_cast<unsigned char>(text[i + len]) & 0xC0) == 0x80) ++len;
            throw FormatError("MBD text: the font has no glyph for '" + std::string(text.substr(i, len)) + "'");
        }
    }
    m.pages.back().push_back(Token::make_control(kMessageEnd));
    return m;
}

}  // namespace ff7cc::mbd
