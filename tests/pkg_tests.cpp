#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "test_main.hpp"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::vector<std::byte> random_bytes(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::byte> out(n);
    for (auto& b : out) b = static_cast<std::byte>(rng() & 0xff);
    return out;
}

// Creates a unique scratch directory and removes it when the test ends.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("ff7cc_test_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

TEST_CASE("index serialize/parse round-trip") {
    const std::vector<pkg::Entry> entries{{0, 3088, 0}, {2, 76672, 0}, {40, 0, 0}, {40, 75008, 0}};
    const auto raw = pkg::serialize_index(entries);
    CHECK(raw.size() == entries.size() * pkg::kEntrySize);
    CHECK(pkg::parse_index(raw) == entries);
    // Layout check: little-endian {sector, size, reserved}.
    CHECK(raw[12] == std::byte{2} && raw[16] == std::byte{0x80} && raw[17] == std::byte{0x2b});
}

TEST_CASE("index with a truncated entry is rejected") {
    const std::vector<std::byte> raw(13);
    CHECK_THROWS_AS(pkg::parse_index(raw), FormatError);
}

TEST_CASE("validate_index detects overlap and overrun") {
    const std::vector<pkg::Entry> ok{{0, 100, 0}, {1, 2048, 0}, {2, 0, 0}};
    pkg::validate_index(ok, 2 * pkg::kSectorSize);
    const std::vector<pkg::Entry> overlap{{0, 4096, 0}, {1, 10, 0}};
    CHECK_THROWS_AS(pkg::validate_index(overlap, 1 << 20), FormatError);
    const std::vector<pkg::Entry> overrun{{0, 4097, 0}};
    CHECK_THROWS_AS(pkg::validate_index(overrun, 4096), FormatError);
}

TEST_CASE("sniff recognises known magics") {
    CHECK(pkg::sniff({}) == pkg::FileType::Empty);
    CHECK(pkg::sniff(bytes("Atel\x01\x03")) == pkg::FileType::Atel);
    CHECK(pkg::sniff(bytes("RIFF....WAVE")) == pkg::FileType::Riff);
    CHECK(pkg::sniff(bytes("PSMF0014")) == pkg::FileType::Psmf);
    CHECK(pkg::sniff(bytes("SSCF")) == pkg::FileType::Sscf);
    CHECK(pkg::sniff(bytes("Ate")) == pkg::FileType::Unknown);
    CHECK(pkg::type_from_name(pkg::type_name(pkg::FileType::Psmf)) == pkg::FileType::Psmf);
}

TEST_CASE("writer output reads back identically") {
    TempDir dir;
    const fs::path game = dir.path / "PSP_GAME" / "USRDIR";
    fs::create_directories(game);

    const std::vector<std::vector<std::byte>> files{
        bytes("Atel header"), random_bytes(pkg::kSectorSize, 1), {}, random_bytes(5000, 2), {}, bytes("x"),
    };
    {
        pkg::Writer writer(pkg::output_paths(game));
        for (const auto& f : files) writer.add(f);
        writer.finish();
    }

    pkg::Reader reader(pkg::locate(dir.path));  // locate() from the disc root
    CHECK(reader.size() == files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        CHECK(reader.read(i) == files[i]);
        CHECK(reader.entry(i).offset() % pkg::kSectorSize == 0);
    }
    // Sector layout: 1 + 1 + 0 + 3 + 0 + 1 sectors.
    CHECK(reader.entry(2).sector == 2 && reader.entry(3).sector == 2);
    CHECK(reader.entry(5).sector == 5);
    CHECK(reader.archive_size() == 6 * pkg::kSectorSize);
    CHECK(reader.sniff(0) == pkg::FileType::Atel);
    CHECK(reader.sniff(2) == pkg::FileType::Empty);

    std::vector<std::byte> mid(10);
    reader.read_at(3, 4000, mid);
    CHECK(std::equal(mid.begin(), mid.end(), files[3].begin() + 4000));
    CHECK_THROWS_AS(reader.read_at(3, 4995, mid), Error);
    CHECK_THROWS_AS(reader.entry(6), Error);
}

TEST_CASE("locate reports a missing archive") {
    TempDir dir;
    CHECK_THROWS_AS(pkg::locate(dir.path), IoError);
}
