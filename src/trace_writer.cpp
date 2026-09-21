#include "trace_writer.hpp"

#include <cctype>
#include <stdexcept>

namespace rtmem::internal {

trace_writer::trace_writer(const char* path, std::uint32_t flags)
    : output_(path, std::ios::out | std::ios::trunc), flags_(flags) {
    if (!output_) {
        throw std::runtime_error("cannot open workload trace output");
    }
    output_ << "RTTRACE 2\n"
            << "# Events are policy-independent and byte addressed.\n";
    if (!output_) {
        throw std::runtime_error("cannot write workload trace header");
    }
}

trace_writer::~trace_writer() {
    std::lock_guard<std::mutex> lock(mutex_);
    output_.flush();
}

std::uint32_t trace_writer::thread_id_locked() {
    const auto current = std::this_thread::get_id();
    const auto found = threads_.find(current);
    if (found != threads_.end()) {
        return found->second;
    }
    const auto assigned = next_thread_id_++;
    threads_.emplace(current, assigned);
    return assigned;
}

void trace_writer::fail_locked(const std::string& message) {
    if (last_error_.empty()) {
        last_error_ = message;
    }
}

bool trace_writer::finish_event_locked() {
    output_ << '\n';
    if ((flags_ & RT_TRACE_FLUSH_EACH_EVENT) != 0) {
        output_.flush();
    }
    if (!output_) {
        fail_locked("cannot write workload trace event");
        return false;
    }
    return true;
}

const char* trace_writer::class_name(rt_retention_class retention_class) {
    switch (retention_class) {
        case RT_CLASS_EPHEMERAL:
            return "EPHEMERAL";
        case RT_CLASS_EPOCH:
            return "EPOCH";
        case RT_CLASS_DURABLE:
            return "DURABLE";
        case RT_CLASS_COUNT:
            break;
    }
    return "UNKNOWN";
}

std::string trace_writer::safe_name(const char* name) {
    if (name == nullptr || *name == '\0') {
        return "unnamed";
    }
    std::string result(name);
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) || std::iscntrl(byte) || character == '#') {
            character = '_';
        }
    }
    return result;
}

bool trace_writer::allocation(std::uint64_t tick,
                              std::uint64_t buffer_id,
                              std::size_t size_bytes,
                              std::size_t alignment,
                              rt_retention_class hint,
                              const std::string& region_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "ALLOC " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << buffer_id << ' ' << size_bytes
            << ' ' << alignment << ' ' << class_name(hint) << ' '
            << safe_name(region_name.c_str());
    return finish_event_locked();
}

bool trace_writer::free(std::uint64_t tick, std::uint64_t buffer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "FREE " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << buffer_id;
    return finish_event_locked();
}

bool trace_writer::hint(std::uint64_t tick,
                        std::uint64_t buffer_id,
                        rt_retention_class retention_class) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "HINT " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << buffer_id << ' '
            << class_name(retention_class);
    return finish_event_locked();
}

bool trace_writer::access(std::uint64_t tick,
                          std::uint32_t stream_id,
                          rt_trace_access_kind kind,
                          std::uint64_t buffer_id,
                          std::size_t offset_bytes,
                          std::size_t size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "ACCESS " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << stream_id << ' '
            << (kind == RT_TRACE_ACCESS_READ ? 'R' : 'W') << ' '
            << buffer_id << ' ' << offset_bytes << ' ' << size_bytes;
    return finish_event_locked();
}

bool trace_writer::compute(std::uint64_t tick,
                           std::uint32_t stream_id,
                           std::uint64_t cycles) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "COMPUTE " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << stream_id << ' ' << cycles;
    return finish_event_locked();
}

bool trace_writer::phase(std::uint64_t tick, const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "PHASE " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << safe_name(name);
    return finish_event_locked();
}

bool trace_writer::barrier(std::uint64_t tick,
                           std::uint32_t stream_id,
                           std::uint64_t barrier_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << "BARRIER " << sequence_++ << ' ' << tick << ' '
            << thread_id_locked() << ' ' << stream_id << ' ' << barrier_id;
    return finish_event_locked();
}

bool trace_writer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    output_.flush();
    if (!output_) {
        fail_locked("cannot flush workload trace");
        return false;
    }
    return last_error_.empty();
}

const char* trace_writer::last_error() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_.c_str();
}

}  // namespace rtmem::internal
