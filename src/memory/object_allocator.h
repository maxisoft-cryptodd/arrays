#pragma once

#include "plf_colony.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility> // For std::move

#include "allocator.h"

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
#ifdef USE_MIMALLOC
        struct Allocator: std::allocator_traits<::mi_stl_allocator<T>>::template rebind_alloc<T>
        {
        };
#else
        struct Allocator: std::allocator_traits<std::allocator<T>>::template rebind_alloc<T>
        {
        };
#endif

        using Colony = plf::colony<T, Allocator>;
    public:
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
            if (reserve)
            {
                std::unique_lock pool_lock{pool_mutex_};
                for (size_t i = 0; i < base_capacity_; ++i)
                {
                    typename Colony::iterator it = colony_.emplace();
                    pool_.emplace_back(std::move(std::shared_ptr<T>(std::addressof(*it), [](void*) {})), it);
                    ++pool_size_;
                }
            }
        }
        ObjectAllocator(const ObjectAllocator&) = delete;
        ObjectAllocator& operator=(const ObjectAllocator&) = delete;
        ObjectAllocator(ObjectAllocator&&) noexcept = default;
        ObjectAllocator& operator=(ObjectAllocator&&) noexcept = default;
        ~ObjectAllocator() = default;

        [[nodiscard]] std::shared_ptr<T> acquire()
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [this] {
                std::lock_guard pool_lock{pool_mutex_}; // this lock guard is mandatory
                return !pool_.empty() || objects_in_use_ < burst_capacity_;
            });

            objects_in_use_.fetch_add(1, std::memory_order_relaxed);

            if (pool_size_.load(std::memory_order_acquire) > 0) {
                std::unique_lock pool_lock{pool_mutex_};
                lock.unlock();
                if (!pool_.empty())
                {
                    pool_size_.fetch_sub(1, std::memory_order_release);
                    auto [obj, it] = std::move(pool_.back());
                    pool_.pop_back();
                    return create_handle(it);
                }
            }

            // Pool was empty, create a new object in the colony
            std::unique_lock colony_lock{colony_mutex_};
            auto it = colony_.emplace();
            return create_handle(it);
        }

        [[nodiscard]] size_t available() const noexcept
        {
            return pool_size_.load(std::memory_order_consume);
        }
        [[nodiscard]] size_t in_use() const noexcept
        {
            return objects_in_use_.load(std::memory_order_consume);
        }
        [[nodiscard]] size_t capacity() const noexcept
        {
            return base_capacity_;
        }

    private:
        // The move-only deleter functor is the key to correct ownership.
        struct Releaser {
            ObjectAllocator* allocator;
            Colony::iterator iterator;

            Releaser() = delete;
            explicit Releaser(ObjectAllocator* allocator, Colony::iterator iterator): allocator(allocator), iterator(iterator)
            {
            }

            Releaser(Releaser&& other) noexcept : allocator(other.allocator), iterator(other.iterator) {
                other.allocator = nullptr;
            }
            Releaser& operator=(Releaser& other) = delete;
            Releaser& operator=(const Releaser& other) = delete;
            Releaser& operator=(Releaser&& other) noexcept {
                if (this != &other) {
                    allocator = other.allocator;
                    iterator = other.iterator;
                    other.allocator = nullptr;
                }
                return *this;
            }

            Releaser(Releaser&) = delete;
            Releaser(const Releaser&) = delete;

            void operator()(void* ptr) {
                if (allocator != nullptr) {
                    assert(ptr == std::addressof(*iterator));
                    allocator->release(iterator);
                    allocator = nullptr;
                }
            }
        };

        std::shared_ptr<T> create_handle(typename Colony::iterator it) {
            return std::shared_ptr<T>(&(*it), Releaser{this, it});
        }

        // Taking by value is correct for this pattern.
        void release(typename Colony::iterator it)
        {
            std::lock_guard lock{mutex_};

            {
                std::lock_guard pool_lock{pool_mutex_};
                if (pool_.size() < base_capacity_) {
                    pool_size_.fetch_add(1, std::memory_order_release);
                    pool_.emplace_back(std::shared_ptr<T>(&(*it), [](void*){}), it);
                    objects_in_use_.fetch_sub(1, std::memory_order_relaxed);
                    cv_.notify_one();
                    return;
                }
            }

            // Pool is full, erase from colony
            {
                std::lock_guard colony_lock{colony_mutex_};
                colony_.erase(it);
            }

            objects_in_use_.fetch_sub(1, std::memory_order_relaxed);
            cv_.notify_one();
        }


        size_t base_capacity_;
        size_t burst_capacity_;
        std::list<std::pair<std::shared_ptr<T>, typename Colony::iterator>> pool_;
        mutable std::mutex mutex_;
        mutable std::mutex pool_mutex_;
        mutable std::mutex colony_mutex_;
        std::condition_variable cv_;
        std::atomic<size_t> objects_in_use_{0};
        std::atomic<size_t> pool_size_{0};  // Track pool size atomically

        Colony colony_{};
    };
}