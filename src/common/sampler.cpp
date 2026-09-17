#include "common/sampler.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep
#pragma comment(lib, "winmm.lib")
#endif

namespace Common::Sampler {

#ifdef _WIN32

namespace {

constexpr size_t   MaxDepth   = 48;
constexpr size_t   MaxSamples = 30000;
constexpr uint32_t Seconds    = 20;

using Sample = std::array<uint64_t, MaxDepth>;

HANDLE            g_thread = nullptr;
std::atomic<bool> g_recording {false};
std::atomic<bool> g_stop {false};

// Nothing between SuspendThread and ResumeThread may allocate or take a CRT lock: the
// suspended thread might hold it.
void Capture(Sample& sample) {
	sample.fill(0);
	if (SuspendThread(g_thread) == static_cast<DWORD>(-1)) {
		return;
	}
	CONTEXT context {};
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (GetThreadContext(g_thread, &context) != 0) {
		for (size_t depth = 0; depth < MaxDepth && context.Rip != 0; depth++) {
			sample[depth]       = context.Rip;
			DWORD64     image   = 0;
			const auto* entry   = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
			const auto  old_rsp = context.Rsp;
			if (entry == nullptr) {
				// Leaf function (or code without unwind data): the return address is on top.
				if (IsBadReadPtr(reinterpret_cast<const void*>(context.Rsp), sizeof(uint64_t)) !=
				    0) {
					break;
				}
				context.Rip = *reinterpret_cast<const uint64_t*>(context.Rsp);
				context.Rsp += sizeof(uint64_t);
			} else {
				void*   handler_data = nullptr;
				DWORD64 establisher  = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip,
				                 const_cast<PRUNTIME_FUNCTION>(entry), &context, &handler_data,
				                 &establisher, nullptr);
			}
			if (context.Rsp <= old_rsp) {
				break;
			}
		}
	}
	ResumeThread(g_thread);
}

void Record() {
	auto       samples = std::make_unique<Sample[]>(MaxSamples);
	size_t     count   = 0;
	const auto end     = GetTickCount64() + Seconds * 1000ull;
	timeBeginPeriod(1);
	while (count < MaxSamples && GetTickCount64() < end && !g_stop.load()) {
		Capture(samples[count++]);
		Sleep(1);
	}
	timeEndPeriod(1);
	const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
	if (FILE* file = std::fopen("kyty_samples.txt", "w"); file != nullptr) {
		std::fprintf(file, "base %llx\n", static_cast<unsigned long long>(base));
		for (size_t index = 0; index < count; index++) {
			for (const auto address: samples[index]) {
				if (address == 0) {
					break;
				}
				std::fprintf(file, "%llx ", static_cast<unsigned long long>(address));
			}
			std::fprintf(file, "\n");
		}
		std::fclose(file);
	}
	std::fprintf(stderr, "sampler: wrote %zu samples to kyty_samples.txt\n", count);
	g_recording.store(false);
}

} // namespace

void RegisterCurrentThread() {
	if (std::getenv("KYTY_SAMPLER") == nullptr || g_thread != nullptr) {
		return;
	}
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
	                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
	                0);
	std::fprintf(stderr, "sampler: thread registered, F4 records %u seconds\n", Seconds);
}

void Toggle() {
	if (g_thread == nullptr) {
		return;
	}
	if (g_recording.exchange(true)) {
		g_stop.store(true);
		return;
	}
	g_stop.store(false);
	std::fprintf(stderr, "sampler: recording\n");
	std::thread(Record).detach();
}

#else

void RegisterCurrentThread() {}
void Toggle() {}

#endif

} // namespace Common::Sampler
