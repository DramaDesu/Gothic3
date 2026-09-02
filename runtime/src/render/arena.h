#pragma once

// A device-local buffer of fixed size that hands out pieces of itself.
//
// Streaming needs geometry to arrive and to leave, and the obvious way to do
// that is to grow a buffer and rewrite whatever points at it. That is the one
// thing you cannot do while frames are in flight without stalling the card. So
// the buffer never changes: it is created once at its full size, and pieces of
// it are handed out and taken back. Vertices and indices are addressed by
// element offset in the draw command itself, and the lighting buffers are read
// as buffer[base + index], so nothing here ever needs a descriptor rewritten -
// not on arrival, not on departure, not ever.
//
// Fixed capacity is a decision, not a shortcut. A buffer that cannot grow tells
// you at load time that the budget is wrong, instead of telling you at run time
// by hitching.

#include "vulkan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace render
{

class GpuArena
{
  public:
    static constexpr std::size_t npos = std::size_t(-1);

    // Counts are in elements, not bytes: a vertex arena is created with the
    // size of a vertex, and everything after that speaks in vertices, which is
    // also what vkCmdDrawIndexed speaks in.
    bool create(Device &device, VkDeviceSize stride, std::size_t capacity, VkBufferUsageFlags usage,
                std::string *error);
    void destroy(Device &device);

    // First fit. npos when there is no run of that many elements left, which is
    // a budget that needs raising rather than an error to paper over.
    std::size_t allocate(std::size_t count);
    void release(std::size_t offset, std::size_t count);

    VkBuffer handle() const { return m_buffer.handle; }
    VkDeviceSize stride() const { return m_stride; }
    std::size_t capacity() const { return m_capacity; }
    std::size_t used() const { return m_used; }
    std::size_t highWater() const { return m_highWater; }
    std::size_t largestFree() const;
    std::size_t freeBlocks() const { return m_free.size(); }

  private:
    struct Block
    {
        std::size_t offset = 0;
        std::size_t count = 0;
    };

    Buffer m_buffer{};
    VkDeviceSize m_stride = 0;
    std::size_t m_capacity = 0;
    std::size_t m_used = 0;
    std::size_t m_highWater = 0;
    // Sorted by offset and never touching: release coalesces, so two adjacent
    // free blocks cannot exist and a run that fits is always found.
    std::vector<Block> m_free;
};

// Copies pile up here and cross the bus together.
//
// A one-shot submit waits for the queue to go idle, so uploading two and a half
// thousand meshes one at a time is two and a half thousand round trips to the
// card. Staging them into one buffer and submitting the copies as a batch is
// the difference between a load you can measure and a load you can go and make
// tea during.
class ArenaUploader
{
  public:
    bool create(Device &device, VkDeviceSize stagingBytes, std::string *error);
    void destroy(Device &device);

    // Stages count elements for the arena at offset. Flushes by itself when the
    // staging buffer fills, so callers can write as much as they like.
    bool write(Device &device, GpuArena &arena, std::size_t offset, const void *data, std::size_t count,
               std::string *error);
    bool flush(Device &device, std::string *error);

    std::size_t submits() const { return m_submits; }

  private:
    struct Copy
    {
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize sourceOffset = 0;
        VkDeviceSize destinationOffset = 0;
        VkDeviceSize bytes = 0;
    };

    Buffer m_staging{};
    VkDeviceSize m_capacity = 0;
    VkDeviceSize m_at = 0;
    std::vector<Copy> m_pending;
    std::size_t m_submits = 0;
};

} // namespace render
