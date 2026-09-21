#pragma once

#include "rtmem/rtmem.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace rtmem::internal {

class trace_writer {
public:
    trace_writer(const char* path, std::uint32_t flags);
    ~trace_writer();

    trace_writer(const trace_writer&) = delete;
    trace_writer& operator=(const trace_writer&) = delete;

    bool allocation(std::uint64_t tick,
                    std::uint64_t buffer_id,
                    std::size_t size_bytes,
                    std::size_t alignment,
                    rt_retention_class hint,
                    const std::string& region_name);
    bool free(std::uint64_t tick, std::uint64_t buffer_id);
    bool hint(std::uint64_t tick,
              std::uint64_t buffer_id,
              rt_retention_class retention_class);
    bool access(std::uint64_t tick,
                std::uint32_t stream_id,
                rt_trace_access_kind kind,
                std::uint64_t buffer_id,
                std::size_t offset_bytes,
                std::size_t size_bytes);
    bool compute(std::uint64_t tick,
                 std::uint32_t stream_id,
                 std::uint64_t cycles);
    bool phase(std::uint64_t tick, const char* name);
    bool barrier(std::uint64_t tick,
                 std::uint32_t stream_id,
                 std::uint64_t barrier_id);
    bool flush();

    const char* last_error() const noexcept;

private:
    std::uint32_t thread_id_locked();
    bool finish_event_locked();
    void fail_locked(const std::string& message);
    static const char* class_name(rt_retention_class retention_class);
    static std::string safe_name(const char* name);

    mutable std::mutex mutex_;
    std::ofstream output_;
    std::unordered_map<std::thread::id, std::uint32_t> threads_;
    std::uint64_t sequence_ = 0;
    std::uint32_t next_thread_id_ = 0;
    std::uint32_t flags_ = 0;
    std::string last_error_;
};

}  // namespace rtmem::internal
