#pragma once

// Little-endian cursor over an in-memory file. Every read is bounds-checked and
// failure is sticky, so a malformed file yields a failed reader rather than a
// crash or silently wrong data.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace genome
{

class Reader
{
  public:
    Reader(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}
    explicit Reader(const std::vector<std::uint8_t> &bytes) : Reader(bytes.data(), bytes.size()) {}

    bool ok() const { return !m_failed; }
    std::size_t tell() const { return m_at; }
    std::size_t size() const { return m_size; }
    std::size_t remaining() const { return m_at <= m_size ? m_size - m_at : 0; }
    const std::uint8_t *data() const { return m_data; }

    void fail() { m_failed = true; }

    void seek(std::size_t at)
    {
        if (at > m_size)
            m_failed = true;
        else
            m_at = at;
    }

    void skip(std::size_t bytes)
    {
        if (m_at + bytes > m_size)
            m_failed = true;
        else
            m_at += bytes;
    }

    std::uint8_t u8() { return read<std::uint8_t>(); }
    std::uint16_t u16() { return read<std::uint16_t>(); }
    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::uint64_t u64() { return read<std::uint64_t>(); }
    float f32() { return read<float>(); }

    // Length-prefixed, windows-1252, no terminator.
    std::string string16()
    {
        const std::uint16_t length = u16();
        return bytes(length);
    }

    std::string string32()
    {
        const std::uint32_t length = u32();
        return bytes(length);
    }

    bool match(const char *magic, std::size_t length)
    {
        if (m_at + length > m_size)
            return false;
        return std::memcmp(m_data + m_at, magic, length) == 0;
    }

    template <typename T> void array(T *destination, std::size_t count)
    {
        const std::size_t bytes = count * sizeof(T);
        if (m_at + bytes > m_size)
        {
            m_failed = true;
            return;
        }
        std::memcpy(destination, m_data + m_at, bytes);
        m_at += bytes;
    }

  private:
    template <typename T> T read()
    {
        T value{};
        if (m_at + sizeof(T) > m_size)
        {
            m_failed = true;
            return value;
        }
        std::memcpy(&value, m_data + m_at, sizeof(T));
        m_at += sizeof(T);
        return value;
    }

    std::string bytes(std::size_t length)
    {
        if (m_at + length > m_size)
        {
            m_failed = true;
            return {};
        }
        std::string value(reinterpret_cast<const char *>(m_data + m_at), length);
        m_at += length;
        return value;
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_at = 0;
    bool m_failed = false;
};

} // namespace genome
