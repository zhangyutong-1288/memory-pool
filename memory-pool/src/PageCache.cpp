/**
 * ============================================================
 * PageCache.cpp — 页缓存实现（三级缓存最底层）
 * ============================================================
 *
 * 【文件定位】
 * 整个内存池系统"最终的内存来源"。
 * 所有内存最终都从这里调用 OS 接口（mmap/VirtualAlloc）获取。
 *
 * 【Span 管理核心逻辑】
 * PageCache 不关心"块大小"——它只管理"多少个页"。
 * 通过 Span 结构体管理每一段连续页：
 *   - freeSpans_：按页数分组的空闲 Span 链表
 *   - spanMap_：地址 → Span 映射（释放时反查用）
 *
 * 【Span 分割（split）——关键优化】
 * 假设 freeSpans_ 里有一个 8 页的 Span，但你只需要 3 页：
 *   切出来：前 3 页给你
 *   余料：后 5 页挂回 freeSpans_[5]，下次可能用上
 * 这就叫 "buddy allocation" 的思想——
 * 拿大拆小，剩余回收，避免浪费。
 *
 * 【相邻 Span 合并（merge）——释放时的关键优化】
 * 释放一个 Span 时，检查它的"邻居"Span 是否也在空闲列表中。
 * 如果是，两个合并成一个大 Span！不断合并直到邻居不可用为止。
 * 这可以防止"内存碎片化"——很多小 Span 分散导致无法满足大请求。
 *
 * 【平台兼容】
 * Windows：VirtualAlloc / Linux&macOS：mmap
 * 用 #ifdef 宏实现跨平台编译。
 */

#include "PageCache.h"
#include <cstring>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace Kama_memoryPool
{

/**
 * allocateSpan — 分配 numPages 个连续页
 * --------------------------------------------
 *
 * 策略（按优先级）：
 * 1. 在 freeSpans_ 中找 ≥ numPages 的最小空闲 Span
 * 2. 如果找到的 Span 比需要的多 → 切分，余料回收
 * 3. 如果找不到合适的 → 直接向 OS 申请
 *
 * 【为什么用 std::map 不用 unordered_map？】
 * std::map::lower_bound(numPages) 返回第一个 ≥ numPages 的 entry，
 * 正好满足"找最合适的 Span"的需求。
 * unordered_map 没有这个能力。
 */
void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找第一个 ≥ numPages 的空闲 Span
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 从空闲链表中移除这个 Span
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        // 【Span 分割】如果 Span 比需要的多，切出多余部分
        // 例如：有 8 页，只要 3 页 → 切出 5 页放回
        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            // 新 Span 的起始地址 = 原地址 + 已分配的页
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 余料挂回 freeSpans_（头插法）
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            // 原 Span 的页数更新为实际分配的
            span->numPages = numPages;
        }

        // 记录 Span 信息到 spanMap_，用于后续 deallocateSpan 反查
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的空闲 Span，向操作系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的 Span 记录
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    spanMap_[memory] = span;
    return memory;
}

/**
 * deallocateSpan — 释放一个 Span
 * ------------------------------------
 * 
 * 把 Span 放回 freeSpans_，并尝试与相邻的 Span 合并。
 *
 * 【合并逻辑】
 * 假设释放地址为 A、页数为 3 的 Span：
 * 1. 计算 A 的"下一个地址" = A + 3 * 4096
 * 2. 在 spanMap_ 里查这个地址有没有对应的 Span
 * 3. 如果有且空闲 → 合并！大 Span 的 numPages = 3 + neighbor.numPages
 * 4. 删除被合并的 Span 的记录
 *
 * 合并的意义：
 * - 防止碎片化：很多小 Span 散落，无法满足大请求
 * - 提升效率：大 Span 可以被灵活切分
 */
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 反查 Span 信息
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return; // 不是 PageCache 分配的内存，忽略

    Span* span = it->second;

    // 尝试与右侧（地址更大）的邻居合并
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    
    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;
        
        // 检查 nextSpan 是否在空闲列表中
        // （只有空闲的才能合并——如果正在被使用就不能动）
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];
        
        if (nextList == nextSpan)
        {
            // 情况1：nextSpan 是链表头
            nextList = nextSpan->next;
            found = true;
        }
        else if (nextList)
        {
            // 情况2：遍历链表找到 nextSpan
            Span* prev = nextList;
            while (prev->next)
            {
                if (prev->next == nextSpan)
                {   
                    prev->next = nextSpan->next; // 从链表中移除
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        if (found)
        {
            // 合并！
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);  // 删除被合并的 Span 的记录
            delete nextSpan;           // 释放 Span 结构体
        }
    }

    // 将（合并后的）Span 通过头插法放入空闲链表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

/**
 * systemAlloc — 向操作系统直接申请内存
 * -----------------------------------------
 *
 * 这是整个项目"最终的内存来源"。
 *
 * Windows：VirtualAlloc
 *   - MEM_COMMIT | MEM_RESERVE：先预留地址空间，再提交物理内存
 *   - PAGE_READWRITE：可读写
 *   - 返回页对齐的地址
 *
 * Linux/macOS：mmap
 *   - MAP_PRIVATE | MAP_ANONYMOUS：匿名映射（不关联文件）
 *   - PROT_READ | PROT_WRITE：可读写
 *   - 返回页对齐的地址
 *
 * 分配后 memset 为 0，确保内存初始状态干净。
 */
void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

#ifdef _WIN32
    // Windows：VirtualAlloc 预留并提交虚拟内存
    void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ptr) return nullptr;
#else
    // Linux/macOS：mmap 匿名内存映射
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;
#endif

    // 清零——避免脏数据导致 bug
    memset(ptr, 0, size);
    return ptr;
}

} // namespace Kama_memoryPool
