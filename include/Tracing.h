#ifndef MYRPCPROJECT_INCLUDE_TRACING_H_
#define MYRPCPROJECT_INCLUDE_TRACING_H_

#include "Protocol.h"

#include <cstdint>
#include <string>

// Minimal distributed tracing (see docs/v2-design-draft.md section 3.5,
// Dapper-style):
//
// - the wire header carries a 64-bit trace_id (flags bit2 marks presence);
// - the framework generates the trace id when the client does not provide one
//   and records one server span per request (method / status / latency);
// - sampling is whole-trace and configurable (default: sample everything, low
//   rates like 1/1024 are supported for production use).
//
// The trace id is carried explicitly in the request context, never through
// thread-local storage, so it survives the V1/V2 cross-thread write-back path
// (worker thread -> weak_ptr -> queueInLoop).
namespace tracing {

struct SpanRecord {
    uint64_t traceId{0};
    uint32_t methodId{0};
    const char* methodName{""};
    uint16_t status{0};
    int64_t latencyMs{0};
    const char* side{"server"};
};

uint64_t generateTraceId();

// Returns true when this trace should be sampled (recorded).
bool shouldSample(uint64_t traceId);

// Emit one span line to the log, e.g.
//   [trace] span trace=1a2b.. side=server method=demo.echo status=0 latency_ms=3
void recordSpan(const SpanRecord& span);

std::string formatTraceId(uint64_t traceId);

}  // namespace tracing

#endif  // MYRPCPROJECT_INCLUDE_TRACING_H_
