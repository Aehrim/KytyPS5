#ifndef GRAPHICS_HOST_GPU_RENDERER_DRAWSTATS_H_
#define GRAPHICS_HOST_GPU_RENDERER_DRAWSTATS_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Diagnostic for the per-draw fast path (KYTY_DRAW_STATS=1): how often does a part of the draw
// state repeat the state of the previous draw? Everything here runs on the GPU thread only.
namespace Libs::Graphics::DrawStats {

enum class Part : uint32_t {
	GuestContextRegs, // no context register write since the previous draw
	GuestShaderRegs,  // no shader register write (user data, shader binds) since the previous draw
	GuestUconfigRegs,
	StateEpoch, // nothing but draw parameter packets since the previous draw
	Programs,
	Targets,
	Pipeline,
	VertexBuffers,
	IndexBuffer,
	Buffers,
	Images,
	Samplers,
	ShaderData,
	AllDescriptors,
	Everything,
	Count
};

struct GuestWrites {
	uint64_t context = 0;
	uint64_t shader  = 0;
	uint64_t uconfig = 0;
};

inline GuestWrites g_guest_writes;

[[nodiscard]] bool Enabled() noexcept;

// Renderer fast paths can be switched at run time for A/B measurements in one scene:
// F3 toggles them, KYTY_DRAW_STATS=ab flips them with every report.
[[nodiscard]] inline std::atomic<bool>& FastPaths() noexcept {
	static std::atomic<bool> enabled {true};
	return enabled;
}

// Wall time of one draw call, accumulated for the report. Free when the stats are off.
class DrawTimer {
public:
	DrawTimer() noexcept;
	~DrawTimer();
	DrawTimer(const DrawTimer&)            = delete;
	DrawTimer& operator=(const DrawTimer&) = delete;

private:
	int64_t m_start = -1;
};

class Fingerprint {
public:
	void Add(uint64_t value) noexcept {
		m_hash ^= value + 0x9e3779b97f4a7c15ull + (m_hash << 6u) + (m_hash >> 2u);
	}
	void AddBytes(const void* data, size_t size) noexcept;

	[[nodiscard]] uint64_t Value() const noexcept { return m_hash; }

private:
	uint64_t m_hash = 0x51ed270b7a3c19e5ull;
};

using Parts = std::array<uint64_t, static_cast<size_t>(Part::Count)>;

// Compares with the previous draw, accumulates and prints a summary once per second.
void Record(Parts parts);

} // namespace Libs::Graphics::DrawStats

#endif /* GRAPHICS_HOST_GPU_RENDERER_DRAWSTATS_H_ */
