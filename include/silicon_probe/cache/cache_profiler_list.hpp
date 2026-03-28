#pragma once

#include <cstddef>
#include <memory>

namespace silicon_probe::cache {

class CacheProfilerList {
public:
    enum class MemoryType {
        aligned,
        huge_page,
    };

    struct Element {
        Element* next = nullptr;
    };

    struct MemoryDeleter {
        MemoryType type = MemoryType::aligned;
        size_t size = 0;

        void operator()(char* ptr) const;
    };

    explicit CacheProfilerList(size_t cache_line_size,
                               size_t count,
                               unsigned int seed = 12345,
                               MemoryType memory_type = MemoryType::aligned);
    ~CacheProfilerList();

    CacheProfilerList(const CacheProfilerList&) = delete;
    CacheProfilerList& operator=(const CacheProfilerList&) = delete;
    CacheProfilerList(CacheProfilerList&&) noexcept = default;
    CacheProfilerList& operator=(CacheProfilerList&&) noexcept = default;

    Element* first() const noexcept;
    size_t element_count() const noexcept;
    size_t line_size() const noexcept;
    size_t total_size() const noexcept;

    void flush_from_cache() const;

private:
    Element* element_at(size_t index) const noexcept;
    void allocate(size_t count);
    void setup_random_cycle(unsigned int seed);
    void verify_cycle() const;

    size_t line_size_ = 0;
    size_t element_count_ = 0;
    std::unique_ptr<char, MemoryDeleter> memory_;
    MemoryType memory_type_ = MemoryType::aligned;
    Element* elements_ = nullptr;
};

} // namespace silicon_probe::cache
