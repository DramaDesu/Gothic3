#include "pak.h"

#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace genome
{
namespace
{

constexpr std::uint32_t c_DirectoryAttribute = 0x10;

std::string toLower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string normalise(std::string_view text)
{
    std::string out = toLower(text);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// The archive is walked with a cursor rather than seeks: the file table is one
// depth-first stream of entries and reading it sequentially keeps it simple.
class Cursor
{
  public:
    Cursor(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}

    bool ok() const { return !m_failed; }
    std::size_t position() const { return m_at; }
    void seek(std::size_t at) { m_at = at; }

    void skip(std::size_t bytes)
    {
        if (m_at + bytes > m_size)
            m_failed = true;
        else
            m_at += bytes;
    }

    std::uint32_t u32()
    {
        std::uint32_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    std::uint64_t u64()
    {
        std::uint64_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    // Length-prefixed, and the stored bytes are followed by a terminator that is
    // not counted in the length.
    std::string str()
    {
        const std::uint32_t length = u32();
        if (length == 0 || !ok())
            return {};
        if (m_at + length + 1 > m_size)
        {
            m_failed = true;
            return {};
        }
        std::string value(reinterpret_cast<const char *>(m_data + m_at), length);
        m_at += length + 1;
        return value;
    }

  private:
    void read(void *destination, std::size_t bytes)
    {
        if (m_at + bytes > m_size)
        {
            m_failed = true;
            std::memset(destination, 0, bytes);
            return;
        }
        std::memcpy(destination, m_data + m_at, bytes);
        m_at += bytes;
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_at = 0;
    bool m_failed = false;
};

std::vector<std::uint8_t> readWholeFile(const std::filesystem::path &path)
{
    std::vector<std::uint8_t> bytes;
    std::FILE *file = nullptr;
#ifdef _WIN32
    _wfopen_s(&file, path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file)
        return bytes;

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0)
    {
        bytes.resize(static_cast<std::size_t>(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
            bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

} // namespace

std::unique_ptr<PakArchive> PakArchive::open(const std::filesystem::path &path, std::string *error)
{
    auto archive = std::unique_ptr<PakArchive>(new PakArchive());
    if (!archive->addVolume(path, error))
        return nullptr;

    // Overlays patch the base archive in order, so .p01 wins over .p00.
    for (int index = 0; index < 100; ++index)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".p%02d", index);
        std::filesystem::path overlay = path;
        overlay.replace_extension(suffix);
        if (std::filesystem::exists(overlay))
            archive->addVolume(overlay, nullptr);
    }
    return archive;
}

bool PakArchive::addVolume(const std::filesystem::path &path, std::string *error)
{
    const std::vector<std::uint8_t> bytes = readWholeFile(path);
    if (bytes.size() < 48)
    {
        if (error)
            *error = "cannot read " + path.string();
        return false;
    }

    Cursor cursor(bytes.data(), bytes.size());
    cursor.skip(24); // version, product, revision, encryption, compression, reserved
    const std::uint64_t fileTableOffset = cursor.u64();
    cursor.u64(); // folder table, not needed: the file table is already a tree
    cursor.u64(); // volume table

    if (fileTableOffset >= bytes.size())
    {
        if (error)
            *error = "file table out of range in " + path.string();
        return false;
    }

    const std::size_t volumeIndex = m_volumes.size();
    m_volumes.push_back(Volume{path, {nullptr, nullptr}});

    cursor.seek(static_cast<std::size_t>(fileTableOffset));

    // Entries nest: a directory carries its child directories and then its files.
    std::vector<std::string> directories;
    const auto readEntry = [&](auto &&self) -> void {
        if (!cursor.ok())
            return;

        cursor.skip(24); // creation/access/write times
        cursor.skip(8);  // size high/low, superseded by the 64-bit fields below
        const std::uint32_t attributes = cursor.u32();

        if (attributes & c_DirectoryAttribute)
        {
            directories.push_back(cursor.str());
            const std::uint32_t directoryCount = cursor.u32();
            for (std::uint32_t index = 0; index < directoryCount && cursor.ok(); ++index)
                self(self);
            const std::uint32_t fileCount = cursor.u32();
            for (std::uint32_t index = 0; index < fileCount && cursor.ok(); ++index)
                self(self);
            directories.pop_back();
            return;
        }

        PakEntry entry;
        entry.offset = cursor.u64();
        entry.storedSize = cursor.u64();
        entry.size = cursor.u64();
        cursor.u32(); // encryption
        entry.compression = cursor.u32();
        entry.path = normalise(cursor.str());
        cursor.str(); // comment

        if (entry.path.empty())
            return;

        // An overlay replaces an entry of the same name; a zero-sized entry in an
        // overlay is how a patch deletes a file.
        entry.deleted = entry.size == 0 && entry.storedSize == 0 && volumeIndex > 0;
        const auto existing = std::find_if(m_entries.begin(), m_entries.end(),
                                           [&](const PakEntry &e) { return e.path == entry.path; });
        if (existing != m_entries.end())
        {
            const std::size_t at = static_cast<std::size_t>(existing - m_entries.begin());
            *existing = entry;
            m_entryVolume[at] = volumeIndex;
        }
        else
        {
            m_entries.push_back(entry);
            m_entryVolume.push_back(volumeIndex);
        }
    };
    readEntry(readEntry);

    if (!cursor.ok() && error)
        *error = "truncated file table in " + path.string();
    return true;
}

const PakEntry *PakArchive::find(std::string_view name) const
{
    const std::string wanted = normalise(name);
    for (const PakEntry &entry : m_entries)
    {
        if (entry.deleted)
            continue;
        if (entry.path == wanted)
            return &entry;
    }
    // Fall back to a name-only match so callers can ask for "wolf.xact".
    for (const PakEntry &entry : m_entries)
    {
        if (entry.deleted)
            continue;
        const std::size_t slash = entry.path.find_last_of('/');
        const std::string_view leaf =
            slash == std::string::npos ? std::string_view(entry.path) : std::string_view(entry.path).substr(slash + 1);
        if (leaf == wanted)
            return &entry;
    }
    return nullptr;
}

std::vector<std::uint8_t> PakArchive::read(const PakEntry &entry, std::string *error) const
{
    std::vector<std::uint8_t> result;
    const std::size_t index = static_cast<std::size_t>(&entry - m_entries.data());
    if (index >= m_entryVolume.size())
    {
        if (error)
            *error = "entry does not belong to this archive";
        return result;
    }

    const Volume &volume = m_volumes[m_entryVolume[index]];
    if (!volume.file)
    {
        std::FILE *handle = nullptr;
#ifdef _WIN32
        _wfopen_s(&handle, volume.path.c_str(), L"rb");
#else
        handle = std::fopen(volume.path.c_str(), "rb");
#endif
        if (!handle)
        {
            if (error)
                *error = "cannot open " + volume.path.string();
            return result;
        }
        volume.file = std::unique_ptr<std::FILE, int (*)(std::FILE *)>(handle, &std::fclose);
    }

    std::vector<std::uint8_t> stored(static_cast<std::size_t>(entry.storedSize));
#ifdef _WIN32
    _fseeki64(volume.file.get(), static_cast<std::int64_t>(entry.offset), SEEK_SET);
#else
    std::fseek(volume.file.get(), static_cast<long>(entry.offset), SEEK_SET);
#endif
    if (std::fread(stored.data(), 1, stored.size(), volume.file.get()) != stored.size())
    {
        if (error)
            *error = "short read for " + entry.path;
        return result;
    }

    if (!entry.compressed())
        return stored;

    result.resize(static_cast<std::size_t>(entry.size));
    mz_ulong produced = static_cast<mz_ulong>(result.size());
    const int status = mz_uncompress(result.data(), &produced, stored.data(), static_cast<mz_ulong>(stored.size()));
    if (status != MZ_OK)
    {
        if (error)
            *error = "inflate failed for " + entry.path;
        result.clear();
        return result;
    }
    result.resize(produced);
    return result;
}

std::vector<std::uint8_t> PakArchive::read(std::string_view name, std::string *error) const
{
    if (const PakEntry *entry = find(name))
        return read(*entry, error);
    if (error)
        *error = "not found: " + std::string(name);
    return {};
}

} // namespace genome
