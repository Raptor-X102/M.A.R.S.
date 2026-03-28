#include "cache_profiler_list.hpp"
#include <random>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sstream>

// Helper to construct error messages
static std::string build_error_msg(const std::string& prefix, size_t val) {
    std::ostringstream oss;
    oss << prefix << val;
    return oss.str();
}

CacheProfilerList::CacheProfilerList(size_t cache_line_size, size_t count, 
                                     unsigned int seed, MemType mem_type)
    : line_size(cache_line_size),
      element_count(0),
      memory(nullptr, MemoryDeleter(mem_type, 0)),  // tmp deleter, will be updated in allocate
      mem_type_(mem_type),
      elements(nullptr)
{
    if (cache_line_size == 0) {
        std::string msg = "Cache line size cannot be zero";
        LOG_ERROR_STREAM << msg;
        throw std::invalid_argument(msg);
    }
    
    if (count == 0) {
        std::string msg = "Element count cannot be zero";
        LOG_ERROR_STREAM << msg;
        throw std::invalid_argument(msg);
    }

    LOG_DEBUG_STREAM << "Creating CacheProfilerList: " << count 
                    << " elements, " << cache_line_size << " bytes per line, "
                    << "total: " << (count * cache_line_size) << " bytes";

    allocate(count);
    setup_random_cycle(seed);
    verify_cycle();
    
    LOG_DEBUG_STREAM << "CacheProfilerList initialized successfully";
}

CacheProfilerList::~CacheProfilerList() {
    LOG_DEBUG_STREAM << "Destroying CacheProfilerList: " << element_count << " elements";
}

CacheProfilerList::Element* CacheProfilerList::get_element_at_index(size_t index) const {
    return reinterpret_cast<Element*>(
        reinterpret_cast<char*>(elements) + index * line_size);
}

void CacheProfilerList::allocate(size_t count) {
    element_count = count;
    size_t total_size = count * line_size;
    
    LOG_DEBUG_STREAM << "Allocating " << total_size << " bytes aligned to " << line_size;
    
    void* raw_mem = nullptr;
    
    if (mem_type_ == MemType::HUGE_PAGE) {
        raw_mem = os::huge_alloc(total_size);
        if (!raw_mem) {
            LOG_ERROR_STREAM << "Huge page allocation failed";
            throw std::bad_alloc();
        }

        memory = std::unique_ptr<char, MemoryDeleter>(
            static_cast<char*>(raw_mem), 
            MemoryDeleter(MemType::HUGE_PAGE, total_size)
        );
    } else {
        raw_mem = os::aligned_alloc(line_size, total_size);
        if (!raw_mem) {
            LOG_ERROR_STREAM << "Aligned allocation failed";
            throw std::bad_alloc();
        }
        memory = std::unique_ptr<char, MemoryDeleter>(
            static_cast<char*>(raw_mem), 
            MemoryDeleter(MemType::ALIGNED, total_size)
        );
    }
    
    elements = static_cast<Element*>(raw_mem);
    std::memset(elements, 0, total_size);
    
    LOG_DEBUG_STREAM << "Memory allocated at: " << static_cast<void*>(elements);
}

void CacheProfilerList::setup_random_cycle(unsigned int seed) {
    LOG_DEBUG_STREAM << "Setting up random cycle with seed: " << seed;

    std::vector<size_t> indices(element_count);
    for (size_t i = 0; i < element_count; ++i) {
        indices[i] = i;
    }
    
    std::mt19937 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);

    for (size_t i = 0; i < element_count - 1; ++i) {
        Element* current = get_element_at_index(indices[i]);
        Element* next = get_element_at_index(indices[i + 1]);
        current->next = next;
    }

    Element* last = get_element_at_index(indices[element_count - 1]);
    Element* first = get_element_at_index(indices[0]);
    last->next = first;

    LOG_DEBUG_STREAM << "Random cycle established: " 
                     << static_cast<void*>(first) << " -> ... -> " 
                     << static_cast<void*>(last) << " -> " 
                     << static_cast<void*>(first);
}

void CacheProfilerList::verify_cycle() {
    if (!elements || element_count == 0) {
        throw std::runtime_error("Cannot verify cycle: list is empty");
    }

    std::vector<bool> visited(element_count, false);
    Element* current = elements;
    size_t count = 0;

    while (count < element_count) {
        size_t index = (reinterpret_cast<char*>(current) - 
                       reinterpret_cast<char*>(elements)) / line_size;
        
        if (index >= element_count) {
            std::string msg = build_error_msg(
                "Cycle verification failed: pointer out of bounds at iteration ", 
                count);
            LOG_ERROR_STREAM << msg;
            throw std::runtime_error(msg);
        }
        
        if (visited[index]) {
            std::string msg = build_error_msg(
                "Cycle verification failed: duplicate visit at index ", 
                index) + " at iteration " + std::to_string(count);
            LOG_ERROR_STREAM << msg;
            throw std::runtime_error(msg);
        }
        
        visited[index] = true;
        current = current->next;
        ++count;
    }

    if (current != elements) {
        std::string msg = "Cycle verification failed: does not return to start";
        LOG_ERROR_STREAM << msg;
        throw std::runtime_error(msg);
    }

    LOG_DEBUG_STREAM << "Cycle verification passed: all " << element_count 
                     << " elements visited exactly once";
}

CacheProfilerList::Element* CacheProfilerList::get_first() const {
    return elements;
}

size_t CacheProfilerList::get_element_count() const {
    return element_count;
}

size_t CacheProfilerList::get_line_size() const {
    return line_size;
}

size_t CacheProfilerList::get_total_size() const {
    return element_count * line_size;
}

void CacheProfilerList::flush_from_cache() const {
    char* ptr = reinterpret_cast<char*>(elements);
    char* end = ptr + (element_count * line_size);
    
    while (ptr < end) {
        arch::clflush(ptr);
        ptr += line_size;
    }
    arch::flush_complete();
    
    LOG_DEBUG_STREAM << "Flushed " << element_count << " elements from cache";
}
