#pragma once

#include <plf_colony.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <expected>
#include <thread>
#include <utility> // For std::move

#include "allocator.h"

namespace cryptodd::memory
{
    // Forward-declare the main class. This is crucial.
    template <typename T>
    class ObjectAllocator;


    namespace details
    {
        template <typename T>
        concept PoolableObject = std::default_initializable<T> && std::movable<T>;

        template <typename T>
        struct ObjectPoolAllocator : DefaultAllocator<T>
        {
            // This rebind now refers to the standalone struct, breaking the recursion.
            template <class U>
            struct rebind {
                using other = ObjectPoolAllocator<U>;
            };

            // Provide the necessary constructors for rebinding.
            ObjectPoolAllocator() = default;

            template <class U>
            explicit ObjectPoolAllocator(const ObjectPoolAllocator<U>& other) noexcept
                : DefaultAllocator<T>(other)
            {}
        };
    }

    template <typename T>
    class ObjectAllocator
    {

        using Colony = plf::colony<T, details::ObjectPoolAllocator<T>>;
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
                    typename Colony::iterator it;

                    {
                        std::unique_lock colony_lock{colony_mutex_};
                        it = colony_.emplace();
                        assert(it != colony_.end());
                    }

                    pool_.push_back(it);
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

            while (objects_in_use_ >= burst_capacity_)
            {
                cv_.wait(lock);
            }

            ++objects_in_use_;
            assert(static_cast<std::make_signed_t<size_t>>(objects_in_use_.load(std::memory_order_relaxed)) > 0);
            assert(objects_in_use_.load(std::memory_order_relaxed) <= burst_capacity_);

            std::unique_lock pool_lock{pool_mutex_, std::try_to_lock};
            lock.unlock();
            while (pool_size_ > 0) {
                if (!pool_lock.owns_lock() && !pool_lock.try_lock())
                {
                    continue;
                }
                assert(pool_lock.owns_lock());
                if (!pool_.empty())
                {
                    --pool_size_;
                    auto it = pool_.back();
                    pool_.pop_back();
                    pool_lock.unlock();
                    return create_handle(it);
                }
            }

            // Pool was empty, create a new object in the colony
            typename Colony::iterator it;

            {
                std::unique_lock colony_lock{colony_mutex_};
                it = colony_.emplace();
                assert(it != colony_.end());
            }

            return create_handle(it);
        }

        [[nodiscard]] size_t available() const noexcept
        {
            return pool_size_;
        }
        [[nodiscard]] size_t in_use() const noexcept
        {
            return objects_in_use_;
        }
        [[nodiscard]] size_t capacity() const noexcept
        {
            return base_capacity_;
        }

        static constexpr std::string_view UNEXPECTED_NEGATIVE_POOL_SIZE = "Pool size is negative";
        static constexpr std::string_view UNEXPECTED_POOL_SIZE_EXCEEDS_CAPACITY = "Pool size exceeds base capacity";
        static constexpr std::string_view UNEXPECTED_NEGATIVE_OBJECTS_IN_USE = "Objects in use is negative";
        static constexpr std::string_view UNEXPECTED_OBJECTS_IN_USE_EXCEEDS_BURST_CAPACITY = "Objects in use exceeds burst capacity";
        static constexpr std::string_view UNEXPECTED_POOL_SIZE_MISMATCH = "Internal pool size mismatch";
        static constexpr std::string_view UNEXPECTED_COLONY_SIZE_MISMATCH = "Colony size mismatch";

        [[nodiscard]] std::expected<void, std::string> check_consistency() const noexcept
        {
            if (static_cast<std::make_signed_t<size_t>>(pool_size_) < 0) return std::unexpected(std::string(UNEXPECTED_NEGATIVE_POOL_SIZE));
            if (pool_size_ > base_capacity_) return std::unexpected(std::string(UNEXPECTED_POOL_SIZE_EXCEEDS_CAPACITY));
            if (static_cast<std::make_signed_t<size_t>>(objects_in_use_) < 0) return std::unexpected(std::string(UNEXPECTED_NEGATIVE_OBJECTS_IN_USE));
            if (objects_in_use_ > burst_capacity_) return std::unexpected(std::string(UNEXPECTED_OBJECTS_IN_USE_EXCEEDS_BURST_CAPACITY));

            {
                std::unique_lock pool_lock {pool_mutex_};
                if (pool_.size() != pool_size_) return std::unexpected(std::string(UNEXPECTED_POOL_SIZE_MISMATCH));
            }

            {
                std::unique_lock colony_lock {colony_mutex_};
                if (colony_.size() != pool_size_) return std::unexpected(std::string(UNEXPECTED_COLONY_SIZE_MISMATCH));
            }

            return {};
        }

    private:
        // The move-only deleter functor is the key to correct ownership.
        struct Releaser {
            std::atomic<ObjectAllocator*> allocator;
            Colony::iterator iterator;

            Releaser() = delete;
            explicit Releaser(ObjectAllocator* allocator, Colony::iterator iterator): allocator(allocator), iterator(iterator)
            {
            }

            Releaser(Releaser&& other) noexcept : allocator(), iterator(other.iterator) {
                auto alloc = other.allocator.load();
                while (alloc != nullptr)
                {
                    if (other.allocator.compare_exchange_strong(alloc, nullptr))
                    {
                        break;
                    }
                }
                allocator = alloc;
            }

            Releaser& operator=(Releaser& other) = delete;
            Releaser& operator=(const Releaser& other) = delete;
            Releaser& operator=(Releaser&& other) noexcept {
                if (this != &other) {
                    auto alloc = other.allocator.load();
                    while (alloc != nullptr)
                    {
                        if (other.allocator.compare_exchange_strong(alloc, nullptr))
                        {
                            break;
                        }
                    }
                    allocator = alloc;
                    iterator = other.iterator;
                }
                return *this;
            }

            Releaser(Releaser&) = delete;
            Releaser(const Releaser&) = delete;

            void operator()(void* ptr) {
                auto alloc = allocator.load();
                if (alloc != nullptr && allocator.compare_exchange_strong(alloc, nullptr)) {
                    assert(ptr == std::addressof(*iterator));
                    alloc->release(iterator);
                }
            }
        };

        std::shared_ptr<T> create_handle(Colony::iterator it) {
            return std::shared_ptr<T>(std::addressof(*it), Releaser{this, it});
        }

        // Taking by value is correct for this pattern.
        void release(Colony::iterator it)
        {
            --objects_in_use_;


            bool should_destroy = true;

            if (pool_size_.load(std::memory_order_relaxed) < base_capacity_)
            {
                std::unique_lock pool_lock{pool_mutex_, std::try_to_lock};
                while (pool_size_ < base_capacity_ && should_destroy)
                {
                    if (!pool_lock.owns_lock() && !pool_lock.try_lock())
                    {
                        continue;
                    }

                    assert(pool_lock.owns_lock());

                    if (pool_.size() >= base_capacity_)
                    {
                        break;
                    }
                    ++pool_size_;
                    pool_.push_back(it);
                    assert(pool_.size() == pool_size_);
                    should_destroy = false;
                }

            }

            if (should_destroy)
            {
                std::lock_guard colony_lock{colony_mutex_};
                colony_.erase(it);
            }

            assert(static_cast<std::make_signed_t<size_t>>(objects_in_use_.load(std::memory_order_relaxed)) >= 0);
            assert(objects_in_use_.load(std::memory_order_relaxed) <= burst_capacity_);

            cv_.notify_all();
        }


        size_t base_capacity_;
        size_t burst_capacity_;
        std::list<typename Colony::iterator> pool_;
        mutable std::mutex mutex_;
        mutable std::mutex pool_mutex_;
        mutable std::mutex colony_mutex_;
        std::condition_variable cv_;
        std::atomic<size_t> objects_in_use_{0};
        std::atomic<size_t> pool_size_{0};  // Track pool size atomically

        Colony colony_{};
    };
}