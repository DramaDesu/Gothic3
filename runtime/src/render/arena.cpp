#include "arena.h"

#include <algorithm>
#include <cstring>

namespace render
{

bool GpuArena::create(Device &device, VkDeviceSize stride, std::size_t capacity, VkBufferUsageFlags usage,
                      std::string *error)
{
    m_stride = stride;
    m_capacity = capacity;
    m_used = 0;
    m_highWater = 0;
    m_free.clear();
    m_free.push_back({0, capacity});

    m_buffer = device.createBuffer(stride * capacity, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, error);
    return m_buffer.handle != VK_NULL_HANDLE;
}

void GpuArena::destroy(Device &device)
{
    device.destroyBuffer(m_buffer);
    m_free.clear();
    m_capacity = m_used = m_highWater = 0;
}

std::size_t GpuArena::allocate(std::size_t count)
{
    if (count == 0)
        return 0;

    for (std::size_t index = 0; index < m_free.size(); ++index)
    {
        Block &block = m_free[index];
        if (block.count < count)
            continue;

        const std::size_t offset = block.offset;
        block.offset += count;
        block.count -= count;
        if (block.count == 0)
            m_free.erase(m_free.begin() + std::ptrdiff_t(index));

        m_used += count;
        m_highWater = std::max(m_highWater, m_used);
        return offset;
    }
    return npos;
}

void GpuArena::release(std::size_t offset, std::size_t count)
{
    if (count == 0)
        return;
    m_used -= std::min(m_used, count);

    // Put it back in order, then join it to whichever neighbours it touches, so
    // that a sector which leaves gives back exactly what it took and the arena
    // does not slowly fill with holes the size of a mesh.
    const auto after = std::lower_bound(m_free.begin(), m_free.end(), offset,
                                        [](const Block &block, std::size_t at) { return block.offset < at; });
    const auto inserted = m_free.insert(after, {offset, count});

    const std::size_t index = std::size_t(inserted - m_free.begin());
    if (index + 1 < m_free.size() && m_free[index].offset + m_free[index].count == m_free[index + 1].offset)
    {
        m_free[index].count += m_free[index + 1].count;
        m_free.erase(m_free.begin() + std::ptrdiff_t(index) + 1);
    }
    if (index > 0 && m_free[index - 1].offset + m_free[index - 1].count == m_free[index].offset)
    {
        m_free[index - 1].count += m_free[index].count;
        m_free.erase(m_free.begin() + std::ptrdiff_t(index));
    }
}

std::size_t GpuArena::largestFree() const
{
    std::size_t largest = 0;
    for (const Block &block : m_free)
        largest = std::max(largest, block.count);
    return largest;
}

bool ArenaUploader::create(Device &device, VkDeviceSize stagingBytes, std::string *error)
{
    (void)device;
    (void)error;
    m_capacity = stagingBytes;
    m_submits = 0;
    m_scratch.clear();
    return true;
}

void ArenaUploader::destroy(Device &device)
{
    (void)device;
    m_pending.clear();
    m_scratch.clear();
    m_scratch.shrink_to_fit();
    m_capacity = 0;
}

bool ArenaUploader::write(Device &device, GpuArena &arena, std::size_t offset, const void *data, std::size_t count,
                          std::string *error)
{
    if (count == 0)
        return true;

    const VkDeviceSize bytes = arena.stride() * count;

    // Flush before adding, not after, so that one huge write still goes in a
    // batch of its own rather than being refused.
    if (!m_scratch.empty() && VkDeviceSize(m_scratch.size()) + bytes > m_capacity && !flush(device, error))
        return false;

    const VkDeviceSize at = m_scratch.size();
    m_scratch.resize(std::size_t(at + bytes));
    std::memcpy(m_scratch.data() + at, data, std::size_t(bytes));
    m_pending.push_back({arena.handle(), at, arena.stride() * offset, bytes});
    return true;
}

bool ArenaUploader::flush(Device &device, std::string *error)
{
    if (m_pending.empty())
    {
        m_scratch.clear();
        return true;
    }

    // Sized to what is actually staged. A sector arriving brings a couple of
    // megabytes; the whole world brings ninety.
    Buffer staging = device.createBuffer(VkDeviceSize(m_scratch.size()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                                         error);
    if (!staging.handle)
        return false;
    std::memcpy(staging.mapped, m_scratch.data(), m_scratch.size());

    VkCommandBuffer command = device.beginOneShot();

    // Copies to the same buffer go in one command. They arrive grouped already,
    // since a mesh writes its vertices and then its indices, so this is a walk
    // rather than a sort.
    std::vector<VkBufferCopy> regions;
    for (std::size_t index = 0; index < m_pending.size();)
    {
        const VkBuffer destination = m_pending[index].destination;
        regions.clear();
        while (index < m_pending.size() && m_pending[index].destination == destination)
        {
            regions.push_back({m_pending[index].sourceOffset, m_pending[index].destinationOffset,
                               m_pending[index].bytes});
            ++index;
        }
        vkCmdCopyBuffer(command, staging.handle, destination, std::uint32_t(regions.size()), regions.data());
    }

    // Not waited for: the barrier the device records makes these writes visible
    // to everything submitted after, and the staging buffer is freed when the
    // copy has really finished.
    device.endOneShotAsync(command, {staging});
    ++m_submits;
    m_pending.clear();
    m_scratch.clear();
    return true;
}

} // namespace render
