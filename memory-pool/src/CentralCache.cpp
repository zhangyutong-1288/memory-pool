/**
 * ============================================================
 * CentralCache.cpp — 中心缓存实现
 * ============================================================
 *
 * 【文件定位】
 * 二级缓存，ThreadCache 和 PageCache 的桥梁。
 * 从 PageCache 拿 Span（连续页），切成小块，批量供给 ThreadCache。
 *
 * 【Span 切块——本文件最核心的逻辑】
 * PageCache 给了一个 Span（比如 8 页 = 32KB），
 * CentralCache 把它切成等大小的小块（比如 64 字节 → 512 块），
 * 用嵌入式链表串起来，供 ThreadCache 批量取用。
 *
 * 切块示意（Span = 32KB, 块大小 = 64B）：
 *   [块0][块1][块2]...[块511]
 *    ↓    ↓    ↓        ↓
 *    next→next→next→...→nullptr
 *
 * 【self-spin lock 实现】
 * 每次访问 centralFreeList_[index] 前先自旋获取 locks_[index]，
 * 操作完立即释放。关键区只有几个指针操作，所以自旋比 mutex 高效。
 */

#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>

namespace Kama_memoryPool
{

/**
 * SPAN_PAGES = 8 页 = 32KB
 * ---------------------------
 * 每次从 PageCache 要内存时默认申请 8 页。
 * 这个值是一个平衡点：
 * - 太小 → 频繁向 PageCache 申请，锁开销大
 * - 太大 → 内存浪费（小块分配可能用不满）
 * 
 * 对于大块（>32KB），则按实际需求申请页数。
 */
static const size_t SPAN_PAGES = 8;

/**
 * fetchRange — 批量取出内存块给 ThreadCache
 * ------------------------------------------------
 * 
 * 核心流程分两种情况：
 * 
 * 【情况 A】centralFreeList_[index] 有货：
 *   直接从头取 batchNum 个，断开链表，返回给 ThreadCache
 * 
 * 【情况 B】centralFreeList_[index] 为空：
 *   1. 从 PageCache 拿一个 Span（8 页）
 *   2. 把 Span 切成等大小的小块
 *   3. 前面 allocBlocks 个给 ThreadCache
 *   4. 后面的块留存在 centralFreeList_[index] 供后续使用
 *
 * 【自旋锁保护】
 * 整个操作在获取 locks_[index] 后进行，确保线程安全。
 * 锁的粒度是按大小类的——不同大小类可以并发操作。
 */
void* CentralCache::fetchRange(size_t index, size_t &batchNum)
{
    if (index >= FREE_LIST_SIZE || batchNum == 0) 
        return nullptr;

    // 自旋锁加锁：test_and_set 返回旧值
    // 如果旧值是 true（已被锁），就自旋等待
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        // yield 让出 CPU，避免 100% 占用核心空转
        std::this_thread::yield();
    }

    void* result = nullptr;
    try 
    {
        // 读当前链表头（relaxed 足够，因为已经持有锁）
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // === 情况 B：中心缓存为空，从 PageCache 拿新内存 ===

            size_t size = (index + 1) * ALIGNMENT;

            // 从页缓存获取 Span（连续内存）
            result = fetchFromPageCache(size);
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // 【核心逻辑】把 Span 切成等大小的小块并串成链表
            char* start = static_cast<char*>(result);

            // totalBlocks：这个 Span 能切出多少个块
            size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;

            // allocBlocks：这次给 ThreadCache 多少个块（不超过总量）
            size_t allocBlocks = std::min(batchNum, totalBlocks);
            
            // 构建"给 ThreadCache"的链表（前 allocBlocks 个块）
            if (allocBlocks > 1) 
            {  
                for (size_t i = 1; i < allocBlocks; ++i) 
                {
                    void* current = start + (i - 1) * size;//第i-1块
                    void* next = start + i * size;//第i块
                    // 嵌入式链表：在当前块的前8字节写入下一块的地址
                    *reinterpret_cast<void**>(current) = next;// 第 i-1 块的前8字节 = next
                }
                // 最后一个块的 next = nullptr
                *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;
            }

            // 如果 Span 还有剩余块，串成链表留在 CentralCache 里
            if (totalBlocks > allocBlocks)
            {
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;//余料的第一个块地址
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr;

                // 原子写入——其他线程可能同时读这个链表
                centralFreeList_[index].store(remainStart, std::memory_order_release);//存回 CentralCache
            }
        } 
        else 
        {
            // === 情况 A：中心缓存有货，直接取 batchNum 个 ===

            void* current = result;// 遍历指针，初始指向链表头（块A）
            void* prev = nullptr; // 前驱指针，记住"上一个"块
            size_t count = 0;//计数器

            // 顺着链表走 batchNum 步
            while (current && count < batchNum)// current 不为空 且 还没取够
            {
                prev = current;                           // 记下当前块地址
                current = *reinterpret_cast<void**>(current); // 读前8字节，跳到下一块
                count++;                                  // 计数+1
            }

            // 实际拿到的数量（可能少于 batchNum）
            batchNum = count;

            if (prev) 
            {
                // 在 prev 处断开链表：prev 之前归 ThreadCache，之后留在 CentralCache
                *reinterpret_cast<void**>(prev) = nullptr;
            }

            // 剩余部分存回 centralFreeList_
            centralFreeList_[index].store(current, std::memory_order_release);
        }
    }
    catch (...) 
    {
        // 异常安全：确保锁被释放
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 释放自旋锁
    locks_[index].clear(std::memory_order_release);
    return result;
}

/**
 * returnRange — 批量接收 ThreadCache 归还的内存块
 * -----------------------------------------------------
 *
 * 把 ThreadCache 退还的链表头插到 centralFreeList_[index] 的头部。
 * 这样其他线程的 ThreadCache 需要这个大小的块时就能直接取到。
 *
 * 【头插法 vs 尾插法】
 * 选择头插法（O(1)）而不是尾插法（O(n)），
 * 因为归还操作本身不应该花费遍历链表的开销。
 */
void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE) 
        return;

    // 获取自旋锁
    while (locks_[index].test_and_set(std::memory_order_acquire)) 
    {
        std::this_thread::yield();
    }

    try 
    {
        // 找到归还链表的最后一个节点
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < size) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        // 【头插法】把归还链表插到中心链表头部：
        //   旧: centralFreeList → A → B → C
        //   归还: start → X → Y → Z
        //   新: centralFreeList → start → ... → Z → A → B → C
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);// 记下旧头 A
        *reinterpret_cast<void**>(end) = current;  // Z.next = A
        centralFreeList_[index].store(start, std::memory_order_release); // head = start
    }
    catch (...) 
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

/**
 * fetchFromPageCache — 从 PageCache 获取内存
 * ------------------------------------------------
 *
 * 策略：
 * - 小对象（≤32KB = 8页）：固定申请 8 页（SPAN_PAGES）
 *   理由：每次都重新申请成本高，一次多拿点再慢慢用
 * - 大对象（>32KB）：按实际需求申请页数
 *   理由：大对象本身就需要多页，按需分配不浪费
 */
void* CentralCache::fetchFromPageCache(size_t size)
{   
    // 计算实际需要的页数（向上取整）
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) 
    {
        // 小对象：固定要 8 页
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    } 
    else 
    {
        // 大对象：按需分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

} // namespace Kama_memoryPool
