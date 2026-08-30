#pragma once

// Reader for Genome .pak archives (Gothic 3, Risen). Read-only, no engine
// involved: this is the entry point of our own runtime.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace genome
{

struct PakEntry
{
    std::string path;        // as stored, backslashes normalised to '/'
    std::uint64_t offset = 0;
    std::uint64_t storedSize = 0;   // bytes in the archive
    std::uint64_t size = 0;         // bytes after decompression
    std::uint32_t compression = 0;  // 0 = stored, otherwise zlib
    bool deleted = false;           // tombstone: a .p00 overlay removes this file

    bool compressed() const { return compression != 0 || storedSize != size; }
};

// One .pak plus the .p00/.p01 overlays that patch it. Later volumes win, which
// is how the Community Patch replaces individual files without touching the
// base archive.
class PakArchive
{
  public:
    // Opens `path` and every sibling overlay (Foo.pak -> Foo.p00, Foo.p01, ...).
    static std::unique_ptr<PakArchive> open(const std::filesystem::path &path, std::string *error = nullptr);

    const std::vector<PakEntry> &entries() const { return m_entries; }

    // Lookup is case-insensitive; a bare file name matches any directory.
    const PakEntry *find(std::string_view name) const;

    std::vector<std::uint8_t> read(const PakEntry &entry, std::string *error = nullptr) const;
    std::vector<std::uint8_t> read(std::string_view name, std::string *error = nullptr) const;

  private:
    struct Volume
    {
        std::filesystem::path path;
        mutable std::unique_ptr<std::FILE, int (*)(std::FILE *)> file{nullptr, nullptr};
    };

    bool addVolume(const std::filesystem::path &path, std::string *error);

    std::vector<Volume> m_volumes;
    std::vector<PakEntry> m_entries;
    std::vector<std::size_t> m_entryVolume;  // parallel to m_entries
};

} // namespace genome
