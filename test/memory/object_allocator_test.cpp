#include "gtest/gtest.h"
#include "../../src/memory/object_allocator.h"
#include <vector>
#include <thread>
#include <numeric>
#include <atomic>

// A simple test class to be managed by ObjectAllocator
struct TestObject {
    static std::atomic<int> instance_count;
    int id;

    TestObject() : id(instance_count++) {}
    ~TestObject() { --instance_count; }
};

std::atomic<int> TestObject::instance_count = 0;

namespace cryptodd::memory {

class ObjectAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestObject::instance_count = 0;
    }

    void TearDown() override {
        // The allocator's lifetime is now managed inside each test,
        // so we can verify cleanup at the end of each test instead of here.
        // A final check ensures no objects leaked between tests.
        ASSERT_EQ(TestObject::instance_count.load(), 0);
    }
};

TEST_F(ObjectAllocatorTest, BasicAcquireRelease) {
    {
        ObjectAllocator<TestObject> allocator(1);
        // Pool starts empty with lazy allocation
        ASSERT_EQ(allocator.available(), 0);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), 0);

        // First acquire is a "burst" allocation
        auto obj = allocator.acquire();
        ASSERT_NE(obj, nullptr);
        ASSERT_EQ(allocator.available(), 0);
        ASSERT_EQ(allocator.in_use(), 1);
        ASSERT_EQ(TestObject::instance_count.load(), 1);

        // Releasing it populates the pool
        obj.reset();
        ASSERT_EQ(allocator.available(), 1);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), 1);
    }
    // Allocator is destroyed, cleaning up the pooled object
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, AcquireUpToBaseCapacity) {
    {
        const size_t capacity = 3;
        ObjectAllocator<TestObject> allocator(capacity);
        ASSERT_EQ(allocator.available(), 0);

        // Acquire 'capacity' objects; all are new allocations
        std::vector<std::shared_ptr<TestObject>> objects;
        for (size_t i = 0; i < capacity; ++i) {
            objects.push_back(allocator.acquire());
            ASSERT_EQ(allocator.available(), 0);
            ASSERT_EQ(allocator.in_use(), i + 1);
            ASSERT_EQ(TestObject::instance_count.load(), i + 1);
        }

        // Release all objects, populating the pool
        objects.clear();
        ASSERT_EQ(allocator.available(), capacity);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), capacity);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, AcquireBeyondBaseCapacityBurst) {
    {
        const size_t capacity = 1;
        ObjectAllocator<TestObject> allocator(capacity);
        ASSERT_EQ(allocator.available(), 0);

        // First two acquires are new allocations
        auto obj1 = allocator.acquire();
        auto obj2 = allocator.acquire();
        ASSERT_EQ(allocator.available(), 0);
        ASSERT_EQ(allocator.in_use(), 2);
        ASSERT_EQ(TestObject::instance_count.load(), 2);

        // Releasing obj1 will populate the pool (since capacity is 1)
        obj1.reset();
        ASSERT_EQ(allocator.available(), 1);
        ASSERT_EQ(allocator.in_use(), 1);
        ASSERT_EQ(TestObject::instance_count.load(), 2);

        // Releasing obj2 now finds the pool full, so it's destroyed
        obj2.reset();
        ASSERT_EQ(allocator.available(), 1);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), 1);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, MultiThreadedAcquireRelease) {
    {
        const size_t capacity = 2;
        const size_t num_threads = 4;
        const size_t iterations_per_thread = 100;
        ObjectAllocator<TestObject> allocator(capacity);

        std::vector<std::thread> threads;

        for (size_t k = 0; k < 10; ++k)
        {
            for (size_t i = 0; i < num_threads; ++i) {
                threads.emplace_back([&]() {
                    // Acquire and release in a tight loop to prevent deadlock
                    for (size_t j = 0; j < iterations_per_thread; ++j) {
                        auto obj = allocator.acquire();
                        ASSERT_NE(obj, nullptr);
                        obj.reset();
                    }
                });
            }

            for (auto& t : threads) {
                t.join();
            }

            threads.clear();
            allocator.acquire().reset();
        }


        // Pool should be fully populated by the end
        ASSERT_EQ(allocator.available(), capacity);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), capacity);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, MultiThreadedContention) {
    {
        const size_t capacity = 1;
        const size_t num_threads = 4;
        const size_t iterations_per_thread = 10;
        ObjectAllocator<TestObject> allocator(capacity);

        std::vector<std::thread> threads;
        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([&]() {
                for (size_t j = 0; j < iterations_per_thread; ++j) {
                    auto obj = allocator.acquire();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        ASSERT_EQ(allocator.available(), capacity);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), capacity);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, ConstructorWithReserve) {
    {
        const size_t capacity = 5;
        // This is the one case where we test eager allocation
        ObjectAllocator<TestObject> allocator(capacity, 2, true);
        ASSERT_EQ(allocator.available(), capacity);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), capacity);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

TEST_F(ObjectAllocatorTest, BurstMultiplierOne) {
    {
        const size_t capacity = 2;
        ObjectAllocator<TestObject> allocator(capacity, 1); // burst_capacity is 2
        ASSERT_EQ(allocator.available(), 0);

        // First two acquires are new allocations, maxing out burst capacity
        auto obj1 = allocator.acquire();
        auto obj2 = allocator.acquire();
        ASSERT_EQ(allocator.available(), 0);
        ASSERT_EQ(allocator.in_use(), 2);
        ASSERT_EQ(TestObject::instance_count.load(), 2);

        // Try to acquire a 3rd object. Should block.
        std::atomic<bool> acquired_third = false;
        std::thread t([&]() {
            auto obj3 = allocator.acquire();
            acquired_third = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_FALSE(acquired_third.load());

        // Release an object, allowing the waiting thread to proceed.
        obj1.reset(); // obj1 goes into the pool
        t.join();
        ASSERT_TRUE(acquired_third.load());

        // After t.join(), the obj3 it acquired goes out of scope and is released.
        // Since obj1 is in the pool, obj3 populates the second slot.
        // obj2 is still held.
        ASSERT_EQ(allocator.available(), 1); // obj3 went back to the pool, then was acquired by obj2's release
        ASSERT_EQ(allocator.in_use(), 1); // obj2 is still held

        obj2.reset();
        ASSERT_EQ(allocator.available(), 2);
        ASSERT_EQ(allocator.in_use(), 0);
        ASSERT_EQ(TestObject::instance_count.load(), 2);
    }
    ASSERT_EQ(TestObject::instance_count.load(), 0);
}

} // namespace cryptodd::memory
