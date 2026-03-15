#pragma once

#include "arch.hpp"
#include "os.hpp"
#include "logger.hpp"
#include <vector>
#include <random>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <cstring>

class CacheProfilerList {
public:
    enum class MemType {
        ALIGNED,  
        HUGE_PAGE 
    };

    struct Element {
        Element* next;
    };

    struct MemoryDeleter {
        MemType type;
        size_t size;
        
        MemoryDeleter(MemType t, size_t s) : type(t), size(s) {}
        
        void operator()(char* ptr) const {
            if (!ptr) return;
            
            if (type == MemType::HUGE_PAGE) {
                os::huge_free(ptr, size);
            } else {
                os::aligned_free(ptr);
            }
        }
    };

private:
    size_t line_size;
    size_t element_count;
    // Custom deleter for unique_ptr
    std::unique_ptr<char, MemoryDeleter> memory;
    MemType mem_type_;
    Element* elements;

public:
    

    explicit CacheProfilerList(size_t cache_line_size, size_t count, unsigned int seed = 12345,
                               MemType mem_type = MemType::ALIGNED);
    ~CacheProfilerList();

    // Disable copying
    CacheProfilerList(const CacheProfilerList&) = delete;
    CacheProfilerList& operator=(const CacheProfilerList&) = delete;
    
    // Enable moving
    CacheProfilerList(CacheProfilerList&&) = default;
    CacheProfilerList& operator=(CacheProfilerList&&) = default;

    Element* get_first() const;
    size_t get_element_count() const;
    size_t get_line_size() const;
    size_t get_total_size() const;

    void flush_from_cache() const;

private:
    Element* get_element_at_index(size_t index) const;

    static void aligned_deleter(char* ptr);
    static void huge_deleter(char* ptr);
    void allocate(size_t count);
    void setup_random_cycle(unsigned int seed);
    void verify_cycle();
};
