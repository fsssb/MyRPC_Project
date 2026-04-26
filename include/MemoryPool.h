#ifndef MYRPCPROJECT_INCLUDE_MEMORYPOOL_H_
#define MYRPCPROJECT_INCLUDE_MEMORYPOOL_H_

#include "Buffer.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(std::size_t initialSize = 1024) {
        freeList_.reserve(initialSize);
        for (std::size_t i = 0; i < initialSize; ++i) {
            auto obj = std::make_unique<T>();
            freeList_.push_back(obj.get());
            storage_.push_back(std::move(obj));
        }
    }

    virtual ~MemoryPool() = default;

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    std::shared_ptr<T> acquire() {
        T* ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (freeList_.empty()) {
                auto obj = std::make_unique<T>();
                ptr = obj.get();
                storage_.push_back(std::move(obj));
            } else {
                ptr = freeList_.back();
                freeList_.pop_back();
            }
        }

        return std::shared_ptr<T>(ptr, [this](T* p) {
            p->reset();
            std::lock_guard<std::mutex> lock(mutex_);
            freeList_.push_back(p);
        });
    }

    std::size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return freeList_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<T>> storage_;
    std::vector<T*> freeList_;
};

class BufferMemoryPool {
public:
    static BufferMemoryPool& instance() {
        static BufferMemoryPool pool;
        return pool;
    }

    std::shared_ptr<Buffer> acquire() {
        return pool_.acquire();
    }

    std::size_t available() const {
        return pool_.available();
    }

private:
    BufferMemoryPool() : pool_(4096) {}

private:
    MemoryPool<Buffer> pool_;
};

#endif  // MYRPCPROJECT_INCLUDE_MEMORYPOOL_H_
