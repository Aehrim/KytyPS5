#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, uint32_t* value);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
};

enum class RuntimeValueType { Any, Integer };

// Inputs consumed by one EvaluateRuntimeSources call. An input is structural when another
// instruction used it as an operand (addresses, conditions, arithmetic); otherwise its value only
// reaches output slots and may change without changing the shape of the evaluation.
struct RuntimeInputTrace {
	enum class Kind : uint8_t { UserData, RawMemory, CleanMemory };

	// An address operand is either a constant or the current value of an earlier input.
	struct Operand {
		int32_t  input = -1;
		uint64_t value = 0;
	};

	struct Input {
		Kind     kind       = Kind::UserData;
		bool     structural = false;
		uint32_t value      = 0;
		// User-data index, or the address the word was read from.
		uint64_t location = 0;
		// Memory inputs keep the recipe of their address (low, high, offset, records), so a
		// pointer that only serves as a base address may change between evaluations.
		bool                   buffer    = false;
		int64_t                immediate = 0;
		std::array<Operand, 4> operands {};
	};

	std::vector<Input> inputs;
	bool               cacheable = true;
};

// Stored evaluations of one resource plan. A later call whose structural inputs still hold reuses
// the stored outputs and only re-reads the leaf inputs.
struct RuntimeSourcesMemo {
	static constexpr uint32_t FlatPosition = 0x80000000u;
	static constexpr size_t   MaxEntries   = 64;

	struct Leaf {
		uint32_t input    = 0;
		uint32_t position = 0;
	};

	struct Entry {
		uint64_t shader_base = 0;
		// Every input in evaluation order; only the structural ones have to keep their value.
		std::vector<RuntimeInputTrace::Input> inputs;
		std::vector<Leaf>                     leaves;
		std::vector<DescriptorValue>          results;
		std::vector<uint32_t>                 flat;
		std::vector<uint8_t>                  active;
	};

	std::vector<Entry> entries;
	size_t             next = 0;
};

struct RuntimeSourcesMemoStats {
	uint64_t hits        = 0;
	uint64_t misses      = 0;
	uint64_t uncacheable = 0;
	uint64_t structural  = 0;
	uint64_t leaves      = 0;
};
// Returns the counters accumulated since the previous call.
RuntimeSourcesMemoStats TakeRuntimeSourcesMemoStats();

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                           const SrtRuntime& runtime, std::span<uint32_t> results);

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results);

// Evaluates potentially reachable descriptor sources and the flattened immediate SRT with one
// memoized scalar walk. Inactive descriptors are zero; on failure no destination is changed.
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources,
                            RuntimeSourcesMemo*   memo = nullptr);

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
