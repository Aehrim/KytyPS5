#ifndef GRAPHICS_GUEST_GPU_GPUSTATEEPOCH_H_
#define GRAPHICS_GUEST_GPU_GPUSTATEEPOCH_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// Advances whenever the command processor handles anything that may change guest GPU state:
// every PM4 packet except draws and their parameter packets, every queued GPU-thread command
// and every new PM4 execution. Two draws that see the same epoch ran back to back with
// identical registers, so the renderer may reuse what it derived from them.
inline std::atomic<uint64_t>& GpuStateEpoch() noexcept {
	static std::atomic<uint64_t> epoch {1};
	return epoch;
}

inline void AdvanceGpuStateEpoch() noexcept {
	GpuStateEpoch().fetch_add(1, std::memory_order_relaxed);
}

} // namespace Libs::Graphics

#endif /* GRAPHICS_GUEST_GPU_GPUSTATEEPOCH_H_ */
