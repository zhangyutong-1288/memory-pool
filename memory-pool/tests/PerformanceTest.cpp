/**
 * ============================================================
 * PerformanceTest.cpp — 性能测试（内存池 vs new/delete 对比）
 * ============================================================
 *
 * 【文件定位】
 * 量化内存池相比 C++ 标准 new/delete 的性能提升幅度。
 * 每个测试场景都同时跑"内存池"和"new/delete"两个版本，
 * 对比耗时，让你直观看到内存池的价值。
 *
 * 【测试场景】
 * 1. 小对象高频分配（100000次 × 32B）
 * 2. 多线程混合分配（4线程 × 25000次 × 随机大小）
 * 3. 混合大小分配（50000次 × 多种大小）
 *
 * 【为什么内存池更快？看这几个角度】
 * - ThreadCache 无锁 → 少了互斥开销
 * - 批量传输 → 减少跨级通信次数
 * - 嵌入式链表 → 零额外内存、O(1)操作
 * - 预切块 → CentralCache 提前把 Span 切好
 *
 * 【预热（warmup）】
 * 性能测试前先跑 5000 次分配/释放，
 * 让内存池的内部结构初始化到"稳态"。
 * 避免把初始化开销算到测试时间里。
 */

#include "../include/MemoryPool.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <thread>

using namespace Kama_memoryPool;
using namespace std::chrono;

/**
 * Timer — 高精度计时器
 * -------------------------------
 * 使用 std::chrono::high_resolution_clock 计时，
 * 输出单位为毫秒（ms），精度微秒级。
 */
class Timer 
{
    high_resolution_clock::time_point start;
public:
    Timer() : start(high_resolution_clock::now()) {}
    
    double elapsed() 
    {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start).count() / 1000.0; // 微秒 → 毫秒
    }
};

/**
 * PerformanceTest — 性能测试类
 * ------------------------------------
 * 所有测试方法都是 static，可以直接调用。
 * 每个测试都跑两遍：一遍用内存池，一遍用 new/delete。
 */
class PerformanceTest 
{
private:
    struct TestStats 
    {
        double memPoolTime{0.0};
        double systemTime{0.0};
        size_t totalAllocs{0};
        size_t totalBytes{0};
    };

public:
    /**
     * warmup — 预热内存系统
     * -------------------------------
     * 跑 5 × 1000 = 5000 次分配释放，让系统进入稳态。
     *
     * 为什么需要预热？
     * - 第一次分配会触发 PageCache 向 OS 申请内存（mmap/VirtualAlloc）
     * - 第一次分配会初始化 thread_local 单例
     * - 这些一次性开销不应计入性能测试
     */
    static void warmup() 
    {
        std::cout << "Warming up memory systems...\n";
        std::vector<std::pair<void*, size_t>> warmupPtrs;
        
        for (int i = 0; i < 1000; ++i) 
        {
            size_t sizes[] = {32, 64, 128, 256, 512};
            for (size_t s = 0; s < 5; ++s) {
                size_t size = sizes[s];
                void* p = MemoryPool::allocate(size);
                warmupPtrs.push_back(std::make_pair(p, size));
            }
        }
        
        for (size_t i = 0; i < warmupPtrs.size(); ++i) 
        {
            MemoryPool::deallocate(warmupPtrs[i].first, warmupPtrs[i].second);
        }
        
        std::cout << "Warmup complete.\n\n";
    }

    /**
     * testSmallAllocation — 小对象高频分配
     * --------------------------------------------
     * 场景：连续分配 100000 个 32 字节对象，每 4 次释放 1 次。
     * 模拟对象频繁创建/销毁的场景（如游戏粒子、网络包）。
     *
     * 预期：内存池显著快于 new/delete，因为：
     * - ThreadCache 本地链表命中率高
     * - 无锁访问
     * - 操作系统交互少
     */
    static void testSmallAllocation() 
    {
        const size_t NUM_ALLOCS = 100000;
        const size_t SMALL_SIZE = 32;
        
        std::cout << "\nTesting small allocations (" << NUM_ALLOCS << " allocations of " 
                  << SMALL_SIZE << " bytes):" << std::endl;
        
        // === 内存池版本 ===
        {
            Timer t;
            std::vector<void*> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                ptrs.push_back(MemoryPool::allocate(SMALL_SIZE));
                
                // 每 4 次释放 1 次，模拟"有些对象很快销毁"
                if (i % 4 == 0) 
                {
                    MemoryPool::deallocate(ptrs.back(), SMALL_SIZE);
                    ptrs.pop_back();
                }
            }
            
            // 清理剩余的
            for (size_t j = 0; j < ptrs.size(); ++j) 
            {
                MemoryPool::deallocate(ptrs[j], SMALL_SIZE);
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // === new/delete 版本（对比基准） ===
        {
            Timer t;
            std::vector<void*> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                ptrs.push_back(new char[SMALL_SIZE]);
                
                if (i % 4 == 0) 
                {
                    delete[] static_cast<char*>(ptrs.back());
                    ptrs.pop_back();
                }
            }
            
            for (size_t j = 0; j < ptrs.size(); ++j) 
            {
                delete[] static_cast<char*>(ptrs[j]);
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
    
    /**
     * testMultiThreaded — 多线程性能测试
     * -------------------------------------------
     * 4 个线程，每个线程做 25000 次随机大小的分配/释放。
     * 75% 概率立刻释放，模拟高并发场景。
     *
     * 这个测试最能体现 ThreadCache 的 thread_local 优势：
     * 每个线程的 ThreadCache 互不干扰，只有偶尔触发 CentralCache 时才需要锁。
     * 而 new/delete 每次都可能触发全局锁竞争。
     */
    static void testMultiThreaded() 
    {
        const size_t NUM_THREADS = 4;
        const size_t ALLOCS_PER_THREAD = 25000;
        const size_t MAX_SIZE = 256;
        
        std::cout << "\nTesting multi-threaded allocations (" << NUM_THREADS 
                  << " threads, " << ALLOCS_PER_THREAD << " allocations each):" 
                  << std::endl;
        
        // 线程工作函数：随机大小、随机释放（75% 概率）
        auto threadFunc = [](bool useMemPool) 
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(8, MAX_SIZE); // 8~256 字节随机
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(ALLOCS_PER_THREAD);
            
            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i) 
            {
                size_t size = dis(gen);
                void* ptr = useMemPool ? MemoryPool::allocate(size) 
                                     : new char[size];
                ptrs.push_back(std::make_pair(ptr, size));
                
                // 75% 概率随机释放一个
                if (rand() % 100 < 75) 
                {
                    size_t index = rand() % ptrs.size();
                    if (useMemPool) {
                        MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                    } else {
                        delete[] static_cast<char*>(ptrs[index].first);
                    }
                    // swap-pop 技巧：O(1) 删除
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }
            
            // 清空剩余
            for (size_t j = 0; j < ptrs.size(); ++j) 
            {
                if (useMemPool) 
                {
                    MemoryPool::deallocate(ptrs[j].first, ptrs[j].second);
                } 
                else 
                {
                    delete[] static_cast<char*>(ptrs[j].first);
                }
            }
        };
        
        // 内存池版本
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.push_back(std::thread(threadFunc, true));
            }
            
            for (size_t j = 0; j < threads.size(); ++j) 
            {
                threads[j].join();
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // new/delete 版本
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.push_back(std::thread(threadFunc, false));
            }
            
            for (size_t j = 0; j < threads.size(); ++j) 
            {
                threads[j].join();
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
    
    /**
     * testMixedSizes — 混合大小分配测试
     * -----------------------------------------
     * 50000 次分配，8 种不同大小随机选择。
     * 每 100 次释放 20 个。
     *
     * 测试内存池在不同大小类之间的切换效率。
     * 场景：模拟一个"各种大小的对象都在创建"的真实应用。
     */
    static void testMixedSizes() 
    {
        const size_t NUM_ALLOCS = 50000;
        const size_t SIZES[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
        
        std::cout << "\nTesting mixed size allocations (" << NUM_ALLOCS 
                  << " allocations):" << std::endl;
        
        // 内存池版本
        {
            Timer t;
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size = SIZES[rand() % 8];
                void* p = MemoryPool::allocate(size);
                ptrs.push_back(std::make_pair(p, size));
                
                // 每 100 次，释放最后 20 个
                if (i % 100 == 0 && !ptrs.empty()) 
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j) 
                    {
                        MemoryPool::deallocate(ptrs.back().first, ptrs.back().second);
                        ptrs.pop_back();
                    }
                }
            }
            
            for (size_t j = 0; j < ptrs.size(); ++j) 
            {
                MemoryPool::deallocate(ptrs[j].first, ptrs[j].second);
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // new/delete 版本
        {
            Timer t;
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size = SIZES[rand() % 8];
                void* p = new char[size];
                ptrs.push_back(std::make_pair(p, size));
                
                if (i % 100 == 0 && !ptrs.empty()) 
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j) 
                    {
                        delete[] static_cast<char*>(ptrs.back().first);
                        ptrs.pop_back();
                    }
                }
            }
            
            for (size_t j = 0; j < ptrs.size(); ++j) 
            {
                delete[] static_cast<char*>(ptrs[j].first);
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
};

int main() 
{
    std::cout << "Starting performance tests..." << std::endl;
    
    // 先预热，再按顺序跑三个测试场景
    PerformanceTest::warmup();
    PerformanceTest::testSmallAllocation();
    PerformanceTest::testMultiThreaded();
    PerformanceTest::testMixedSizes();
    
    return 0;
}
