#include "silicon_probe/cache/cache_profiler_list.hpp"

#include "silicon_probe/infra/logging.hpp"
#include "silicon_probe/platform/arch.hpp"
#include "silicon_probe/platform/os.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::string build_error_message(const std::string& prefix, size_t value) {
    std::ostringstream stream{};
    stream << prefix << value;
    return stream.str();
}

} // namespace

namespace silicon_probe::cache {

void CacheProfilerList::MemoryDeleter::operator()(char* ptr) const {
    if (ptr == nullptr) {
        return;
    }

    if (type == MemoryType::huge_page) {
        platform::huge_free(ptr, size);
    } else {
        platform::aligned_free(ptr);
    }
}

CacheProfilerList::CacheProfilerList(size_t cache_line_size,
                                     size_t count,
                                     unsigned int seed,
                                     MemoryType memory_type)
    : line_size_(cache_line_size)
    , memory_(nullptr, MemoryDeleter{memory_type, 0})
    , memory_type_(memory_type) {
    if (cache_line_size == 0) {
        throw std::invalid_argument("Cache line size cannot be zero");
    }
    if (count == 0) {
        throw std::invalid_argument("Element count cannot be zero");
    }

    SPDLOG_DEBUG("Creating cache profiler list: count={}, line_size={}, total_size={}",
                 count,
                 cache_line_size,
                 count * cache_line_size);

    allocate(count);
    setup_random_cycle(seed);
    verify_cycle();
}

CacheProfilerList::~CacheProfilerList() {
    SPDLOG_DEBUG("Destroying cache profiler list with {} elements", element_count_);
}

CacheProfilerList::Element* CacheProfilerList::first() const noexcept {
    return elements_;
}

size_t CacheProfilerList::element_count() const noexcept {
    return element_count_;
}

size_t CacheProfilerList::line_size() const noexcept {
    return line_size_;
}

size_t CacheProfilerList::total_size() const noexcept {
    return element_count_ * line_size_;
}

void CacheProfilerList::flush_from_cache() const {
    char* current = reinterpret_cast<char*>(elements_);
    char* end = current + total_size();

    while (current < end) {
        platform::arch::clflush(current);
        current += line_size_;
    }
    platform::arch::flush_complete();
}

CacheProfilerList::Element* CacheProfilerList::element_at(size_t index) const noexcept {
    return reinterpret_cast<Element*>(reinterpret_cast<char*>(elements_) + index * line_size_);
}

void CacheProfilerList::allocate(size_t count) {
    element_count_ = count;
    const size_t bytes = count * line_size_;

    void* raw_memory = nullptr;
    if (memory_type_ == MemoryType::huge_page) {
        raw_memory = platform::huge_alloc(bytes);
        if (raw_memory == nullptr) {
            throw std::bad_alloc();
        }
    } else {
        raw_memory = platform::aligned_alloc(line_size_, bytes);
    }

    memory_ = std::unique_ptr<char, MemoryDeleter>(static_cast<char*>(raw_memory), MemoryDeleter{memory_type_, bytes});
    elements_ = static_cast<Element*>(raw_memory);
}

void CacheProfilerList::setup_random_cycle(unsigned int seed) {
    std::vector<size_t> indices(element_count_);
    for (size_t index = 0; index < element_count_; ++index) {
        indices[index] = index;
    }

    std::mt19937 generator{seed};
    std::shuffle(indices.begin(), indices.end(), generator);

    for (size_t index = 0; index + 1 < element_count_; ++index) {
        element_at(indices[index])->next = element_at(indices[index + 1]);
    }
    element_at(indices.back())->next = element_at(indices.front());
}

void CacheProfilerList::verify_cycle() const {
    if (elements_ == nullptr || element_count_ == 0) {
        throw std::runtime_error("Cannot verify an empty profiler list");
    }

    std::vector<bool> visited(element_count_, false);
    Element* current = elements_;

    for (size_t iteration = 0; iteration < element_count_; ++iteration) {
        const size_t index = static_cast<size_t>(reinterpret_cast<char*>(current) - reinterpret_cast<char*>(elements_)) / line_size_;
        if (index >= element_count_) {
            throw std::runtime_error(build_error_message("Cycle verification failed: pointer out of bounds at iteration ", iteration));
        }
        if (visited[index]) {
            throw std::runtime_error(build_error_message("Cycle verification failed: duplicate visit at index ", index));
        }

        visited[index] = true;
        current = current->next;
    }

    if (current != elements_) {
        throw std::runtime_error("Cycle verification failed: does not return to the start");
    }
}

} // namespace silicon_probe::cache
