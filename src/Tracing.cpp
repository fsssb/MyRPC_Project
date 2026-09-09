#include "Tracing.h"

#include "Logger.h"

#include <chrono>
#include <cstdio>
#include <random>

namespace tracing {

namespace {

std::mt19937_64& rng() {
    static thread_local std::mt19937_64 generator{std::random_device{}() ^
                                                  static_cast<uint64_t>(
                                                      std::chrono::steady_clock::now()
                                                          .time_since_epoch()
                                                          .count())};
    return generator;
}

constexpr double kSampleRate = 1.0;  // sample everything by default

}  // namespace

uint64_t generateTraceId() {
    return rng()();
}

bool shouldSample(uint64_t /*traceId*/) {
    // Whole-trace sampling: for a rate r, a trace is sampled iff its id falls
    // in the first r fraction of the id space. Keeping the sample decision on
    // the trace id (not a separate counter) keeps all spans of one trace
    // consistent. Default kSampleRate = 1.0 samples everything.
    return true;  // sampled unconditionally until a configurable rate is added
}

std::string formatTraceId(uint64_t traceId) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(traceId));
    return std::string(buf);
}

void recordSpan(const SpanRecord& span) {
    LOG_INFO("[trace] span trace=" + formatTraceId(span.traceId) + " side=" + span.side +
             " method=" + (span.methodName != nullptr ? span.methodName : "") +
             " status=" + std::to_string(span.status) +
             " latency_ms=" + std::to_string(span.latencyMs));
}

}  // namespace tracing
