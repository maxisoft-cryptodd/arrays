#pragma once

#include <concepts>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace cryptodd::memory
{

    namespace details
    {
        template <typename T>
        concept PoolableObject = std::default_initializable<T> && std::movable<T>;
    }


    template <typename T>
    class ObjectAllocator
    {
    public:
        // Constructor with sensible defaults
        explicit ObjectAllocator(const size_t base_capacity = std::max(1u, std::thread::hardware_concurrency()),
                                 const size_t burst_multiplier = 2, const bool reserve = false) :
            base_capacity_{base_capacity}, burst_capacity_{base_capacity * burst_multiplier}
        {
            if (base_capacity_ == 0)
            {
                throw std::invalid_argument("ObjectAllocator base_capacity must be greater than 0");
            }

            if (burst_multiplier < 1)
            {
                throw std::invalid_argument("burst_multiplier must be at least 1");
            }

            // Pre-populate the pool
            if (reserve)
            {
                for (size_t i = 0; i < base_capacity_; ++i)
                {
                    pool_.emplace_back(std::move(std::make_shared<T>()));
                }
            }
        }

        // Delete copy operations (pooled resources shouldn't be copied)
        ObjectAllocator(const ObjectAllocator&) = delete;
        ObjectAllocator& operator=(const ObjectAllocator&) = delete;

        // Default move operations
        ObjectAllocator(ObjectAllocator&&) noexcept = default;
        ObjectAllocator& operator=(ObjectAllocator&&) noexcept = default;

        ~ObjectAllocator() = default;

        // Acquire an object from the pool
        [[nodiscard]] std::shared_ptr<T> acquire()
        {
            std::unique_lock lock{mutex_};

            // Wait until we can provide an object (from pool or via burst)
            cv_.wait(lock, [this] { return !pool_.empty() || objects_in_use_ < burst_capacity_; });

            std::shared_ptr<T> obj = acquire_internal(lock);

            // Return with custom deleter that returns object to pool
            std::shared_ptr<void> guard(nullptr, [this, captured_obj = obj](void*) { release(captured_obj); });
            return {guard, obj.get()};
        }

        // Query current state
        [[nodiscard]] size_t available() const noexcept
        {
            std::scoped_lock lock{mutex_};
            return pool_.size();
        }

        [[nodiscard]] size_t in_use() const noexcept
        {
            return objects_in_use_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] size_t capacity() const noexcept
        {
            return base_capacity_;
        }

    private:
        // Internal acquisition logic (caller must hold lock)
        std::shared_ptr<T> acquire_internal([[maybe_unused]] std::unique_lock<std::mutex>& lock)
        {
            std::shared_ptr<T> obj;

            if (!pool_.empty())
            {
                obj = std::move(pool_.back());
                pool_.pop_back();
            }
            else if (objects_in_use_ < burst_capacity_)
            {
                // Allocate burst object
                obj = std::make_shared<T>();
            }
            else
            {
                // Should never happen due to wait condition
                throw std::runtime_error("Unreachable code reached");
            }

            objects_in_use_.fetch_add(1, std::memory_order_relaxed);
            return obj;
        }

        // Return object to pool
        void release(std::shared_ptr<T> obj)
        {
            std::unique_lock lock{mutex_};

            objects_in_use_.fetch_sub(1, std::memory_order_relaxed);

            // Return to pool only if we haven't exceeded base capacity
            // This naturally handles burst objects (they're simply discarded)
            if (pool_.size() < base_capacity_)
            {
                pool_.push_back(std::move(obj));
            }
            // else: obj goes out of scope and is destroyed

            lock.unlock();
            cv_.notify_one();
        }

        // Configuration
        size_t base_capacity_;
        size_t burst_capacity_;

        // Pool state
        std::deque<std::shared_ptr<T>> pool_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<size_t> objects_in_use_{0};
    };
} // namespace cryptodd::memory
