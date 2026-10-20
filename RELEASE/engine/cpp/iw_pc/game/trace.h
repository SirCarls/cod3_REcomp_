// What the movement asks of the world: traces, answered by the game's own trace functions
// (see the Rust version, iw_pc::game::trace). A move's handler byte picks one (0 the server's,
// 1 the client game's); they are the host's (MoveHost).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace iw_pc::game::trace {

/// Bytes of Call of Duty 2's trace_t (no end point: callers compute it from the fraction).
inline constexpr std::size_t kTraceSize = 0x24;
namespace at {
inline constexpr std::size_t kFraction = 0x00, kNormal = 0x04, kSurfaceFlags = 0x10, kContents = 0x14,
                             kEntityNum = 0x1C, kAllSolid = 0x22, kStartSolid = 0x23;
}
/// The contents bit of bodies' boxes (CONTENTS_BODY).
inline constexpr std::int32_t kContentsBody = 0x02000000;

struct Trace {
    std::array<std::uint8_t, kTraceSize> bytes{};
    std::int32_t integer(std::size_t at) const {
        std::int32_t v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    float real(std::size_t at) const {
        float v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    float fraction() const { return real(at::kFraction); }
    std::array<float, 3> normal() const { return {real(at::kNormal), real(at::kNormal + 4), real(at::kNormal + 8)}; }
    std::int32_t contents() const { return integer(at::kContents); }
    std::uint16_t entity_num() const {
        return static_cast<std::uint16_t>(bytes[at::kEntityNum] | bytes[at::kEntityNum + 1] << 8);
    }
    bool all_solid() const { return bytes[at::kAllSolid] != 0; }
    bool start_solid() const { return bytes[at::kStartSolid] != 0; }
};

/// What the movement needs from the rest of the game.
class MoveHost {
public:
    virtual ~MoveHost() = default;
    /// The box (mins, maxs) swept from start to end through what content_mask collides with,
    /// ignoring entity pass_entity, by the game's trace function `handler`.
    virtual Trace trace(std::uint8_t handler, const std::array<float, 3>& start, const std::array<float, 3>& mins,
                        const std::array<float, 3>& maxs, const std::array<float, 3>& end, std::int32_t pass_entity,
                        std::int32_t content_mask) = 0;
};

}  // namespace iw_pc::game::trace
