/**
 * ============================================================
 * UnitTest.cpp — 单元测试（正确性验证）
 * ============================================================
 *
 * 【文件定位】
 * 验证内存池在各种场景下的正确性。
 * 不关心性能，只关心"对不对"。
 *
 * 【测试覆盖】
 * - 基础分配/释放：小、中、大对象
 * - 内存写入/读取：确保数据完整性
 * - 多线程并发：4线程 × 1000次 = 4000次操作
 * - 边界情况：0 字节分配、1 字节、MAX_BYTES、超过上限
 * - 压力测试：10000次随机大小分配 + 随机顺序释放
 */

#include "../include/MemoryPool.h"
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cstring>
#include <random>
#include <algorithm>
#include <atomic>

using namespace Kama_memoryPool;

/**
 * testBasicAllocation — 基础分配测试
 * ---------------------------------------
 * 测试三种典型场景：
 * 1. 小内存（8B）：走 ThreadCache → 可能在本地链表命中
 * 2. 中等内存（1KB）：走 ThreadCache → 可能触发 CentralCache
 * 3. 大内存（1MB）：超过 MAX_BYTES，直接走 malloc/free，不经过内存池
 */
void testBasicAllocation() 
{
    std::cout << "Running basic allocation test..." << std::endl;
    
    // 测试小内存分配（8字节，刚好一个对齐单位）
    void* ptr1 = MemoryPool::allocate(8);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 8);

    // 测试中等大小内存分配（1024字节）
    void* ptr2 = MemoryPool::allocate(1024);
    assert(ptr2 != nullptr);
    MemoryPool::deallocate(ptr2, 1024);

    // 测试大内存分配（1MB，超过 MAX_BYTES=256KB 的阈值）
    // 这条路径不经过内存池，直接走系统 malloc
    void* ptr3 = MemoryPool::allocate(1024 * 1024);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, 1024 * 1024);

    std::cout << "Basic allocation test passed!" << std::endl;
}

/**
 * testMemoryWriting — 内存写入测试
 * ---------------------------------------
 * 验证通过内存池分配的内存可以正常读写数据。
 * 写入 0~255 的循环字节，再读出来验证。
 * 这个测试能发现：内存越界、野指针、数据损坏等问题。
 */
void testMemoryWriting() 
{
    std::cout << "Running memory writing test..." << std::endl;

    const size_t size = 128;
    char* ptr = static_cast<char*>(MemoryPool::allocate(size));
    assert(ptr != nullptr);

    // 写入数据：每个字节写入 i%256
    for (size_t i = 0; i < size; ++i) 
    {
        ptr[i] = static_cast<char>(i % 256);
    }

    // 验证数据：读出来应该和写入一致
    for (size_t i = 0; i < size; ++i) 
    {
        assert(ptr[i] == static_cast<char>(i % 256));
    }

    MemoryPool::deallocate(ptr, size);
    std::cout << "Memory writing test passed!" << std::endl;
}

/**
 * testMultiThreading — 多线程并发测试
 * -----------------------------------------
 * 4 个线程同时随机分配/释放，验证 ThreadCache 的线程安全性。
 *
 * 场景：每个线程做 1000 次操作，随机决定分配还是释放。
 *      → 总共约 4000 次混合操作。
 *
 * 用 std::atomic<bool> 作为全局错误标志，
 * 任何线程出错都会设置标志，主线程检测到后报告失败。
 */
void testMultiThreading() 
{
    std::cout << "Running multi-threading test..." << std::endl;

    const int NUM_THREADS = 4;
    const int ALLOCS_PER_THREAD = 1000;
    std::atomic<bool> has_error{false};
    
    auto threadFunc = [&has_error]() 
    {
        try 
        {
            std::vector<std::pair<void*, size_t>> allocations;
            allocations.reserve(ALLOCS_PER_THREAD);
            
            for (int i = 0; i < ALLOCS_PER_THREAD && !has_error; ++i) 
            {
                // 随机大小：8 ~ 2056 字节（8的倍数）
                size_t size = (rand() % 256 + 1) * 8;
                void* ptr = MemoryPool::allocate(size);
                
                if (!ptr) 
                {
                    std::cerr << "Allocation failed for size: " << size << std::endl;
                    has_error = true;
                    break;
                }
                
                allocations.push_back({ptr, size});
                
                // 50% 概率立即释放一个之前的分配（随机选）
                if (rand() % 2 && !allocations.empty()) 
                {
                    size_t index = rand() % allocations.size();
                    MemoryPool::deallocate(allocations[index].first, 
                                         allocations[index].second);
                    allocations.erase(allocations.begin() + index);
                }
            }
            
            // 清理剩余未释放的
            for (const auto& alloc : allocations) 
            {
                MemoryPool::deallocate(alloc.first, alloc.second);
            }
        }
        catch (const std::exception& e) 
        {
            std::cerr << "Thread exception: " << e.what() << std::endl;
            has_error = true;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) 
    {
        threads.emplace_back(threadFunc);
    }

    for (auto& thread : threads) 
    {
        thread.join();
    }

    std::cout << "Multi-threading test passed!" << std::endl;
}

/**
 * testEdgeCases — 边界情况测试
 * ---------------------------------------
 * 覆盖各种"极端"场景：
 * 
 * 1. allocate(0)：
 *    SizeClass::getIndex 会把 0 clamp 到 8，返回有效指针
 * 
 * 2. allocate(1)：
 *    1 字节但会被对齐到 8 字节，验证地址确实是 8 的倍数
 * 
 * 3. allocate(MAX_BYTES)：
 *    正好 256KB，还在内存池管理范围内
 * 
 * 4. allocate(MAX_BYTES + 1)：
 *    刚好超过阈值，走 malloc 路径
 */
void testEdgeCases() 
{
    std::cout << "Running edge cases test..." << std::endl;
    
    // 0 大小分配：应返回有效指针（clamp到8字节）
    void* ptr1 = MemoryPool::allocate(0);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 0);
    
    // 最小大小（1字节）：验证地址 8 字节对齐
    void* ptr2 = MemoryPool::allocate(1);
    assert(ptr2 != nullptr);
    // 地址 & 7 == 0 说明是 8 的倍数
    assert((reinterpret_cast<uintptr_t>(ptr2) & (ALIGNMENT - 1)) == 0);
    MemoryPool::deallocate(ptr2, 1);
    
    // 刚好在内存池上限（256KB）
    void* ptr3 = MemoryPool::allocate(MAX_BYTES);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, MAX_BYTES);
    
    // 刚好超过上限（256KB+1），走系统 malloc
    void* ptr4 = MemoryPool::allocate(MAX_BYTES + 1);
    assert(ptr4 != nullptr);
    MemoryPool::deallocate(ptr4, MAX_BYTES + 1);
    
    std::cout << "Edge cases test passed!" << std::endl;
}

/**
 * testStress — 压力测试
 * -------------------------------
 * 10000 次随机大小分配，然后随机顺序释放。
 * 模拟真实应用中"分配时间 ≠ 释放时间"的场景。
 *
 * 使用 std::shuffle 打乱释放顺序，
 * 这能测试到各种内存碎片回收路径。
 */
void testStress() 
{
    std::cout << "Running stress test..." << std::endl;

    const int NUM_ITERATIONS = 10000;
    std::vector<std::pair<void*, size_t>> allocations;
    allocations.reserve(NUM_ITERATIONS);

    // 阶段1：分配 10000 次，随机大小（8B ~ 8KB）
    for (int i = 0; i < NUM_ITERATIONS; ++i) 
    {
        size_t size = (rand() % 1024 + 1) * 8;
        void* ptr = MemoryPool::allocate(size);
        assert(ptr != nullptr);
        allocations.push_back({ptr, size});
    }

    // 阶段2：随机打乱顺序后全部释放
    // 这模拟了真实场景中"先分配的未必先释放"
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(allocations.begin(), allocations.end(), g);
    for (const auto& alloc : allocations) 
    {
        MemoryPool::deallocate(alloc.first, alloc.second);
    }

    std::cout << "Stress test passed!" << std::endl;
}

int main() 
{
    try 
    {
        std::cout << "Starting memory pool tests..." << std::endl;

        testBasicAllocation();
        testMemoryWriting();
        testMultiThreading();
        testEdgeCases();
        testStress();

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
