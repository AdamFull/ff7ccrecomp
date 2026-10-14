#include "mods.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "ff7cc/formats/pkg.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "psprecore/runtime/io.hpp"
#include "psprecore/text.hpp"
#include "core/foundation/containers/map.h"

namespace ff7cc::game {

namespace {

constexpr std::uint64_t kSector = 2048;
constexpr const char* kPkg = "PSP_GAME/USRDIR/discimg.pkg";
constexpr const char* kFse = "PSP_GAME/USRDIR/discimg.fse";

// A virtual archive with every entry in its original order, as a repacked archive would have them (the original
// is laid out contiguously, so without mods the layout is identical). Entries after a resized one move; the game
// finds files only through discimg.fse, which it is given patched. Keeping the order matters: the game treats
// files by position (moving a module past the movies changes how it is loaded).
class ModOverlay final : public rt::DiscOverlay {
public:
    struct Entry {
        std::uint64_t sector = 0, sectors = 0;  // virtual position, relative to the start of discimg.pkg
        std::uint64_t source_sector = 0;        // original position (unmodified entries)
        nx::string mod;                         // replacement file, if any
    };
    nx::vector<Entry> entries;
    nx::vector<char> fse;  // the patched index

    void read_sectors(std::uint64_t first, std::uint64_t count, char* out) override {
        auto it = std::upper_bound(entries.begin(), entries.end(), first,
                                   [](std::uint64_t s, const Entry& e) { return s < e.sector; });
        if (it != entries.begin()) --it;
        for (std::uint64_t s = first, end = first + count; s < end && it != entries.end(); ++it) {
            if (it->sectors == 0 || it->sector + it->sectors <= s) continue;
            if (it->sector >= end) break;
            const std::uint64_t from = std::max(s, it->sector), to = std::min(end, it->sector + it->sectors);
            char* dst = out + (from - first) * kSector;
            if (it->mod.empty()) {
                rt::read_disc_sectors(it->source_sector + from - it->sector, to - from, dst);
            } else if (auto mod = nx::fs::open_file(it->mod); mod.has_value()) {
                if (mod.value()->seek(static_cast<std::int64_t>((from - it->sector) * kSector)))
                    (void)mod.value()->read(dst, static_cast<std::size_t>((to - from) * kSector));  // last sector: zero-padded
            }
            s = to;
        }
    }

    const nx::vector<char>* file(nx::string_view path) override { return path == "psp_game/usrdir/discimg.fse" ? &fse : nullptr; }
};

}  // namespace

void install_archive() { rt::map_disc_file(kPkg, kPkgLbn); }

void set_mods(nx::string_view mods_dir) {
    rt::set_disc_overlay(nullptr);
    if (!is_directory(mods_dir)) return;
    nx::vector<nx::string> mods;
    if (auto list = nx::fs::directory_entries(mods_dir); list.has_value())
        for (const auto& d : list.value())
            if (d.type == nx::fs::file_type::directory && is_directory(path_join(d.path, "pkg"))) mods.push_back(d.path);
    std::sort(mods.begin(), mods.end());
    nx::map<std::size_t, nx::string> replaced;  // entry -> file (later mods win)
    for (const auto& m : mods) {
        auto files = nx::fs::directory_entries(path_join(m, "pkg"));
        if (!files.has_value()) continue;
        for (const auto& f : files.value()) {
            const nx::string stem = file_stem(f.path);
            if (f.type != nx::fs::file_type::regular || stem.empty() ||
                stem.view().find_first_not_of("0123456789") != nx::string_view::npos)
                continue;
            replaced[static_cast<std::size_t>(parse_u64(stem))] = f.path;
        }
    }
    if (replaced.empty()) return;

    const auto index = io::read_all(path_join(rt::game_dir(), kFse));
    auto entries = pkg::parse_index(index);
    for (const auto& [i, path] : replaced)
        if (i >= entries.size())
            std::fprintf(stderr, "mods: %s replaces entry %zu, but the archive has %zu entries\n", path.c_str(), i,
                         entries.size());
    auto overlay = std::make_unique<ModOverlay>();
    std::uint64_t next = 0;
    overlay->entries.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        ModOverlay::Entry v;
        v.sector = next;
        v.source_sector = entries[i].sector;
        if (const auto it = replaced.find(i); it != replaced.end()) {
            v.mod = it->second;
            entries[i].size = static_cast<std::uint32_t>(nx::fs::stat_file(v.mod).size);
        }
        v.sectors = (std::uint64_t{entries[i].size} + kSector - 1) / kSector;
        entries[i].sector = static_cast<std::uint32_t>(next);
        next += v.sectors;
        overlay->entries.push_back(std::move(v));
    }
    const auto patched = pkg::serialize_index(entries);
    overlay->fse.assign(reinterpret_cast<const char*>(patched.data()), reinterpret_cast<const char*>(patched.data()) + patched.size());
    std::fprintf(stderr, "mods: %zu archive entries replaced from %zu mod folders in %s\n", replaced.size(), mods.size(),
                 nx::string(mods_dir).c_str());
    rt::set_disc_overlay(std::move(overlay));
}

}  // namespace ff7cc::game
