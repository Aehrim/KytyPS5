#include "graphics/host_gpu/renderer/drawStats.h"

#include "graphics/guest_gpu/gpuStateEpoch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::DrawStats {

namespace {

constexpr std::array<const char*, static_cast<size_t>(Part::Count)> PartNames {
    "guest-ctx-regs", "guest-sh-regs", "guest-uc-regs", "state-epoch", "programs",
    "targets",        "pipeline",      "vertex-bufs",   "index-buf",   "buffers",
    "images",         "samplers",      "shader-data",   "descriptors", "everything"};

struct State {
	Parts       previous {};
	GuestWrites writes {};
	Parts       same {};
	uint64_t    draws = 0;
	// Length of the current run of draws that keep programs, targets and pipeline.
	uint64_t                              runs        = 0;
	uint64_t                              timed       = 0;
	int64_t                               timed_ns    = 0;
	std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
};

State g_state;

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DRAW_STATS");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

static int64_t NowNs() noexcept {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

DrawTimer::DrawTimer() noexcept: m_start(Enabled() ? NowNs() : -1) {}

DrawTimer::~DrawTimer() {
	if (m_start >= 0) {
		g_state.timed_ns += NowNs() - m_start;
		g_state.timed++;
	}
}

void Fingerprint::AddBytes(const void* data, size_t size) noexcept {
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size >= sizeof(uint64_t)) {
		uint64_t word = 0;
		std::memcpy(&word, bytes, sizeof(word));
		Add(word);
		bytes += sizeof(word);
		size -= sizeof(word);
	}
	if (size != 0) {
		uint64_t word = 0;
		std::memcpy(&word, bytes, size);
		Add(word);
	}
	Add(size);
}

void Record(Parts parts) {
	auto&      state = g_state;
	const auto index = [](Part part) { return static_cast<size_t>(part); };

	// The guest parts are counters of register writes, the others are fingerprints.
	parts[index(Part::GuestContextRegs)] = g_guest_writes.context;
	parts[index(Part::GuestShaderRegs)]  = g_guest_writes.shader;
	parts[index(Part::GuestUconfigRegs)] = g_guest_writes.uconfig;
	parts[index(Part::StateEpoch)]       = GpuStateEpoch().load(std::memory_order_relaxed);

	Fingerprint everything;
	for (size_t part = index(Part::Programs); part < index(Part::Everything); part++) {
		everything.Add(parts[part]);
	}
	parts[index(Part::Everything)] = everything.Value();

	if (state.draws != 0) {
		for (size_t part = 0; part < parts.size(); part++) {
			state.same[part] += parts[part] == state.previous[part] ? 1u : 0u;
		}
		if (parts[index(Part::Programs)] != state.previous[index(Part::Programs)] ||
		    parts[index(Part::Targets)] != state.previous[index(Part::Targets)] ||
		    parts[index(Part::Pipeline)] != state.previous[index(Part::Pipeline)]) {
			state.runs++;
		}
	}
	state.previous = parts;
	state.draws++;

	const auto now = std::chrono::steady_clock::now();
	if (now - state.last_report < std::chrono::seconds(5) || state.draws < 2) {
		return;
	}
	std::fprintf(stderr,
	             "draw-stats: fast-paths=%s draw-mean=%.2fus timed=%llu draws=%llu "
	             "runs(program+targets+pipeline)=%llu same-as-previous:",
	             FastPaths().load(std::memory_order_relaxed) ? "on" : "off",
	             state.timed != 0 ? static_cast<double>(state.timed_ns) / 1000.0 /
	                                    static_cast<double>(state.timed)
	                              : 0.0,
	             static_cast<unsigned long long>(state.timed),
	             static_cast<unsigned long long>(state.draws),
	             static_cast<unsigned long long>(state.runs + 1u));
	for (size_t part = 0; part < parts.size(); part++) {
		std::fprintf(stderr, " %s=%.1f%%", PartNames[part],
		             100.0 * static_cast<double>(state.same[part]) /
		                 static_cast<double>(state.draws - 1u));
	}
	std::fprintf(stderr, "\n");
	static const bool ab = [] {
		const char* value = std::getenv("KYTY_DRAW_STATS");
		return value != nullptr && std::strcmp(value, "ab") == 0;
	}();
	if (ab) {
		FastPaths().store(!FastPaths().load(std::memory_order_relaxed), std::memory_order_relaxed);
	}
	state.timed       = 0;
	state.timed_ns    = 0;
	state.same        = {};
	state.draws       = 0;
	state.runs        = 0;
	state.last_report = now;
}

} // namespace Libs::Graphics::DrawStats
