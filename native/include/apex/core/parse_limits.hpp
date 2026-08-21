#pragma once

#include <cstddef>
#include <cstdint>

namespace apex::core {

// Limits are deliberately explicit.  They prevent values read from an asset
// file from becoming allocation sizes without a policy decision first.
struct ParseLimits {
    std::size_t maxInputBytes = 256u * 1024u * 1024u;
    std::size_t maxOutputBytes = 256u * 1024u * 1024u;
    std::size_t maxStringBytes = 1024u * 1024u;
    std::size_t maxTracks = 1'000'000;
    std::size_t maxFramesPerTrack = 10'000'000;
    std::size_t maxTotalFrames = 50'000'000;
};

}
