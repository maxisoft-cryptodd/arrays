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
                for (size_t i = 0; i < base_capacity_; ++i)
                {
                    pool_.emplace_back(std::move(std::make_shared<T>()));
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
            cv_.wait(lock, [this] { return !pool_.empty() || objects_in_use_ < burst_capacity_; });
            std::shared_ptr<T> obj = acquire_internal(lock);
            std::shared_ptr<void> guard(nullptr, [this, captured_obj = obj](void*) { release(captured_obj); });
            return {guard, obj.get()};
        }
        [[nodiscard]] size_t available() const noexcept
        {
            std::scoped_lock lock{mutex_};
            return pool_.size();
        }
        [[nodiscard]] size_t in_use() const noexcept
        {
            return objects_in_use_.load(std::memory_order_acquire);
        }
        [[nodiscard]] size_t capacity() const noexcept
        {
            return base_capacity_;
        }

    private:
        std::shared_ptr<T> acquire_internal([[maybe_unused]] std::unique_lock<std::mutex>& lock)
        {
            std::shared_ptr<T> obj;
#ifndef NDEBUG
            if (!lock.owns_lock())
            {
                std::unreachable();
            }
#endif
            if (!pool_.empty())
            {
                obj = std::move(pool_.back());
                pool_.pop_back();
            }
            else if (objects_in_use_ < burst_capacity_)
            {
                obj = std::make_shared<T>();
            }
            else
            {
                throw std::runtime_error("Unreachable code reached");
            }
            objects_in_use_.fetch_add(1, std::memory_order_release);
            return obj;
        }

        void release(std::shared_ptr<T> obj)
        {
            std::unique_lock lock{mutex_};

            // Decrement with proper memory ordering
            objects_in_use_.fetch_sub(1, std::memory_order_acq_rel);

            // Simple first-come-first-served policy: if pool has room, keep the object
            // This check is safe because it's performed under mutex lock
            // Objects that arrive when pool is full are discarded (burst cleanup)
            if (pool_.size() < base_capacity_)
            {
                pool_.push_back(std::move(obj));
            }

            obj.reset();
            // else: burst object discarded (this is correct behavior)

            lock.unlock();
            cv_.notify_one();
        }
        size_t base_capacity_;
        size_t burst_capacity_;
        std::deque<std::shared_ptr<T>> pool_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<size_t> objects_in_use_{0};
    };
} // namespace cryptodd::memory
