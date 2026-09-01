#pragma once

// Profiler zones, or nothing at all when the client is not compiled in. Tracy
// streams to an external viewer and records nothing until one connects, so an
// ordinary run pays no measurable price for the zones being present.

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>

#define G3_ZONE(name) ZoneScopedN(name)
#define G3_FRAME_MARK FrameMark
#else
#define G3_ZONE(name)
#define G3_FRAME_MARK
#endif
