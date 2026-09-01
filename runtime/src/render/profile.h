#pragma once

// Profiler zones, or nothing at all when the client is not compiled in. Tracy
// streams to an external viewer and records nothing until one connects, so an
// ordinary run pays no measurable price for the zones being present.

#include <vulkan/vulkan.h>

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#define G3_ZONE(name) ZoneScopedN(name)
#define G3_FRAME_MARK FrameMark

// GPU zones ask the card for timestamps around the work and collect them a
// frame or two later. That is the difference that matters here: a wall clock in
// the loop cannot tell drawing from waiting on the display, and the frame timer
// reading a flat 10 ms was exactly that.
#define G3_GPU_ZONE(context, command, name) TracyVkZone(context, command, name)

using GpuContext = TracyVkCtx;

inline GpuContext gpuContextCreate(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                   VkCommandBuffer command)
{
    return TracyVkContext(physical, device, queue, command);
}

inline void gpuContextDestroy(GpuContext &context)
{
    if (context)
        TracyVkDestroy(context);
    context = nullptr;
}

inline void gpuCollect(GpuContext context, VkCommandBuffer command)
{
    if (context)
        TracyVkCollect(context, command);
}

#else

#define G3_ZONE(name)
#define G3_FRAME_MARK
#define G3_GPU_ZONE(context, command, name)

using GpuContext = void *;

inline GpuContext gpuContextCreate(VkPhysicalDevice, VkDevice, VkQueue, VkCommandBuffer) { return nullptr; }
inline void gpuContextDestroy(GpuContext &context) { context = nullptr; }
inline void gpuCollect(GpuContext, VkCommandBuffer) {}

#endif
