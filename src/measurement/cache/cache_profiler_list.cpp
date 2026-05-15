// measurement/cache/cache_profiler_list.cpp
#include "measurement/cache/cache_profiler_list.hpp"

#include <sstream>

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

std::string CacheProfilerList::build_error_message(const std::string& prefix, size_t value) {
    std::ostringstream stream{};
    stream << prefix << value;
    return stream.str();
}

CacheProfilerList::Element* CacheProfilerList::element_at(size_t index) const noexcept {
    return reinterpret_cast<Element*>(memory_.get() + index * line_size_);
}

void CacheProfilerList::allocate(size_t count) {
    allocated_count_ = count;
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
}

void CacheProfilerList::verify_cycle() const {
    if (first() == nullptr || element_count_ == 0) {
        throw std::runtime_error("Cannot verify an empty profiler list");
    }

    std::vector<bool> visited(element_count_, false);
    Element* current = first();

    for (size_t iteration = 0; iteration < element_count_; ++iteration) {
        const size_t index =
            static_cast<size_t>(reinterpret_cast<char*>(current) - reinterpret_cast<char*>(first())) / line_size_;
        if (index >= element_count_) {
            throw std::runtime_error(
                build_error_message("Cycle verification failed: pointer out of bounds at iteration ", iteration)
            );
        }
        if (visited[index]) {
            throw std::runtime_error(
                build_error_message("Cycle verification failed: duplicate visit at index ", index)
            );
        }

        visited[index] = true;
        current        = current->next;
    }

    if (current != first()) {
        throw std::runtime_error("Cycle verification failed: does not return to the start");
    }
}

CacheProfilerList::CacheProfilerList(size_t cache_line_size, size_t max_count, MemoryType memory_type)
    : line_size_(cache_line_size), element_count_(0), memory_(nullptr, MemoryDeleter{memory_type, 0}),
      memory_type_(memory_type) {
    if (cache_line_size == 0) throw std::invalid_argument("Cache line size cannot be zero");
    if (max_count == 0) throw std::invalid_argument("Element count cannot be zero");

    SPDLOG_DEBUG("Creating cache profiler list: max_count={}, line_size={}, total_size={}",
                 max_count, cache_line_size, max_count * cache_line_size);

    allocate(max_count);
}

CacheProfilerList::~CacheProfilerList() {
    SPDLOG_DEBUG("Destroying cache profiler list with {} elements", element_count_);
}

void CacheProfilerList::prepare(size_t count, unsigned int seed) {
    if (count == 0) throw std::invalid_argument("Cannot prepare cycle with zero elements");
    if (count > allocated_count_) {
        throw std::out_of_range("Requested count exceeds allocated list size");
    }

    // Generate random permutation of indices [0, count-1]
    std::vector<size_t> indices(count);
    for (size_t i = 0; i < count; ++i) indices[i] = i;

    std::mt19937 generator(seed);
    std::shuffle(indices.begin(), indices.end(), generator);

    // Build linked cycle for these indices
    for (size_t i = 0; i + 1 < count; ++i) {
        element_at(indices[i])->next = element_at(indices[i + 1]);
    }
    element_at(indices.back())->next = element_at(indices.front());

    // Set the first element for traversal
    first_element_ = element_at(indices[0]);
    element_count_ = count;
}

CacheProfilerList::Element* CacheProfilerList::first() const noexcept { return first_element_; }

size_t CacheProfilerList::element_count() const noexcept { return element_count_; }

size_t CacheProfilerList::line_size() const noexcept { return line_size_; }

size_t CacheProfilerList::total_size() const noexcept { return element_count_ * line_size_; }

void CacheProfilerList::flush_from_cache() const {
    if (!memory_) return;
    char* start = memory_.get();
    char* end = start + element_count_ * line_size_; 
    for (char* p = start; p < end; p += line_size_) {
        platform::arch::clflush(p);
    }
    platform::arch::flush_complete();
}

}  // namespace silicon_probe::cache
