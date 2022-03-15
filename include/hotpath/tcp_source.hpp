#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <string_view>

#include "hotpath/event.hpp"

namespace hotpath {

/// Parses one telemetry line: ``host_id,f0,f1,...,f7`` (kFeatures floats). Returns nullopt if the
/// line is malformed. id and ingest_ns are left zero for the caller to stamp.
std::optional<Event> parse_event_line(std::string_view line);

/// Newline-delimited TCP ingest. Multiplexes all client connections on the calling thread with
/// poll(), so the downstream queue keeps a single producer. Returns when `stop` becomes true.
/// `ready` is invoked with the bound port once listening (port 0 => ephemeral).
void run_tcp_source(int port, const std::atomic<bool>& stop,
                    const std::function<void(const Event&)>& emit,
                    const std::function<void()>& on_malformed,
                    const std::function<void(int)>& ready = nullptr);

}  // namespace hotpath
