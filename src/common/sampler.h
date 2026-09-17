#ifndef COMMON_SAMPLER_H_
#define COMMON_SAMPLER_H_

// Minimal sampling profiler for one thread (KYTY_SAMPLER=1, Windows only). The registered
// thread is suspended about once per millisecond and its call stack is recorded; F4 starts a
// 20 second recording that is written to kyty_samples.txt. Resolve it with
// tools/sampler_report.py. Zones tell how long a step takes, samples tell where inside it.
namespace Common::Sampler {

// Makes the calling thread the sampled thread. No-op unless KYTY_SAMPLER is set.
void RegisterCurrentThread();
// Starts a recording, or stops the running one early.
void Toggle();

} // namespace Common::Sampler

#endif /* COMMON_SAMPLER_H_ */
