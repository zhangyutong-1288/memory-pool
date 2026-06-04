/**
 * ============================================================
 * PageCache.h — 页缓存（三级缓存的第三级，最接近 OS）
 * ============================================================
 *
 * 【位置】三级缓存：ThreadCache → CentralCache → PageCache → OS
 *
 * 【核心设计思想 — PageCache 的独特角色】
 *
 * 前两级缓存 (ThreadCache + CentralCache) 操作的是"内存块"（例如 8B, 16B, 64B...），
 * PageCache 操作的是"页"（page = 4096 字节）。
 *
 * PageCache 不关心你申请多大——它只关心你要几页！
 * 从操作系统要来整页整页的内存，交给 CentralCache 去切块。
 *
 * 【Span — 核心数据结构】
 * Span（跨度）描述一块连续的页：
 *   struct Span { pageAddr, numPages, next }
 * 
 * Span 同时管理两个维度的信息：
 * 1. 页数 → Span 链表（freeSpans_）：按页数分组，实现"找合适的 Span"
 * 2. 地址 → Span 映射（spanMap_）：释放时根据地址快速找到对应的 Span
 *
 * 【为什么需要 spanMap_？】
 * 释放内存时，用户只给一个 void* 指针。
 * 我们需要知道这个地址属于哪个 Span、有几页，
 * 才能正确归还！spanMap_ 就是"地址反查 Span"的字典。
 *
 * 【std::map 的选择】
 * freeSpans_ 用 std::map<size_t, Span*> 而不是 std::unordered_map：
 * 原因：我们需要"找到 ≥ numPages 的最小的 Span"，
 *       std::map 有序的特性正好支持 lower_bound 操作。
 *
 * 【系统内存分配】
 * - Linux/macOS：使用 mmap 直接映射匿名内存页
 * - Windows：使用 VirtualAlloc 预留和提交虚拟内存
 * 都返回页对齐的地址，这是内存管理的基本要求。
 *
 * 【为什么没有"切块"逻辑？】
 * PageCache 只负责给"页"，切块的工作在 CentralCache 里做。
 * 这是"关注点分离"——PageCache 不需要知道上层怎么用这些页。
 */

#pragma once
#include "Common.h"
#include <map>
#include <mutex>

namespace Kama_memoryPool
{

/**
 * PageCache — 页缓存（全局单例）
 * ----------------------------------------
 * 直接与操作系统交互，管理页级别的内存分配与回收。
 * 是三级缓存中最底层、离 OS 最近的一级。
 */
class PageCache
{
public:
    /** 页大小：4096 字节（4KB），这是操作系统内存管理的基本单位 */
    static const size_t PAGE_SIZE = 4096; // 4K页大小

    /**
     * getInstance — 全局单例
     * -------------------------------------------------
     * 全局只有一个 PageCache，所有对 OS 的内存操作都经过它。
     * 用 std::mutex 保护，因为它是所有线程的共享资源。
     */
    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    /**
     * allocateSpan — 分配指定页数的 Span
     * -------------------------------------------------
     * @param numPages 需要多少页（必须 >= 1）
     * @return 返回页对齐的内存地址
     * 
     * 策略：
     * 1. 先在 freeSpans_ 里找合适大小的空闲 Span
     * 2. 找不到就从 OS 申请新的
     * 
     * 【重要】返回的地址是页对齐的（地址是 4096 的倍数），
     * 这个属性是 CentralCache 正确切块的前提
     */
    void* allocateSpan(size_t numPages);

    /**
     * deallocateSpan — 释放 Span
     * -------------------------------------------------
     * @param ptr      要释放的内存起始地址
     * @param numPages 页数
     * 
     * 释放的 Span 会被缓存在 freeSpans_ 中，
     * 供后续分配复用，避免频繁向 OS 申请/归还。
     */
    void deallocateSpan(void* ptr, size_t numPages);

private:
    PageCache() = default;

    /**
     * systemAlloc — 向操作系统申请内存
     * -------------------------------------------------
     * 这是整个内存池项目"最终的内存来源"。
     * 
     * Linux：   mmap(nullptr, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
     * Windows： VirtualAlloc(nullptr, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)
     * 
     * 为什么用 mmap/VirtualAlloc 而不是 malloc？
     * - mmap/VirtualAlloc 保证返回页对齐的地址
     * - 可以精确控制页的数量
     * - 大块内存分配更高效（绕过 malloc 的簿记开销）
     */
    void* systemAlloc(size_t numPages);

private:
    /**
     * Span — 跨度结构体（PageCache 的最小管理单位）
     * -------------------------------------------------
     * pageAddr：页起始地址（页对齐）
     * numPages：包含几页
     * next：    链表指针，同页数的 Span 串在一起
     */
    struct Span
    {
        void*  pageAddr; // 页起始地址（必须是 PAGE_SIZE 的整数倍）
        size_t numPages; // 页数
        Span*  next;     // 指向下一个同页数的 Span（单向链表）
    };

    /**
     * freeSpans_ — 空闲 Span 管理（按页数分组）
     * -------------------------------------------------
     * std::map<size_t, Span*>  —  key = 页数, value = 该页数的 Span 链表头
     * 
     * 为什么用 std::map 而不是 unordered_map？
     *   我们需要 "找到 >= N 页的最小 Span"
     *   → std::map 有序，lower_bound 正好满足这个需求
     * 
     * 例如：
     *   freeSpans_[1] → Span(1页) → Span(1页)
     *   freeSpans_[2] → Span(2页)
     *   freeSpans_[4] → Span(4页) → Span(4页) → Span(4页)
     *   freeSpans_[8] → Span(8页)
     * 
     * 如果你要分配 3 页：
     *   freeSpans_.lower_bound(3) → 找到 key=4
     *   → 拿一个 4 页的 Span，切出 3 页给你，
     *      剩下的 1 页挂回 freeSpans_[1]
     */
    std::map<size_t, Span*> freeSpans_;

    /**
     * spanMap_ — 地址到 Span 的映射
     * -------------------------------------------------
     * key = 内存块首地址，value = 该地址所属的 Span
     * 
     * 用于 deallocateSpan 时快速定位：
     *   用户给了一个 void*，通过 spanMap_ 就能找到它属于哪个 Span
     *   → 知道有多少页 → 正确归还到 freeSpans_
     * 
     * 【为什么可以匹配任意地址？】
     * 因为 allocateSpan 返回的地址就是 pageAddr，
     * 而 Span 内的所有地址都在 pageAddr ~ pageAddr+numPages*PAGE_SIZE 范围内，
     * 所以用 pageAddr 作为 key，能匹配 Span 内任何地址
     */
    std::map<void*, Span*> spanMap_;

    /**
     * mutex_ — 全局互斥锁
     * -------------------------------------------------
     * PageCache 使用 std::mutex 而不是 spin lock，
     * 因为 PageCache 的操作（可能调用 mmap/VirtualAlloc）耗时不确定，
     * 不适合自旋等待，用 mutex 让线程在内核态休眠更合理。
     */
    std::mutex mutex_;
};

} // namespace Kama_memoryPool
