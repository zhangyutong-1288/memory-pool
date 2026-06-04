/**
 * ============================================================
 * MemoryPool.h — 用户接口层（对外统一入口）
 * ============================================================
 *
 * 【文件定位】
 * 这是整个内存池项目对外的"门面" —— 用户代码只需要
 * #include "MemoryPool.h"，然后调用两个静态方法即可。
 *
 * 【设计模式：门面模式 (Facade Pattern)】
 * 内部有三层复杂结构（ThreadCache → CentralCache → PageCache），
 * 但对外只需要两个函数：
 *   MemoryPool::allocate(size)   — 申请内存
 *   MemoryPool::deallocate(ptr, size) — 释放内存
 *
 * 所有内部复杂性都对用户隐藏了。
 *
 * 【为什么 deallocate 也需要传 size？】
 * 这是本项目的一个重要设计决策。
 * C 语言的 free(ptr) 不需要 size（因为 malloc 在块前面记录了大小），
 * 但这个内存池选择让调用者提供 size，好处：
 *   1. 不用在每个块里额外存储大小信息（节省内存）
 *   2. 可以直接计算大小类索引，O(1) 定位到正确的链表
 *   3. 简化了内存块的结构
 *
 * 【与标准 malloc/free 的区别】
 *   malloc(0) 的行为未定义 → allocate(0) 会被 clamp 到 8 字节
 *   free(nullptr) 是安全空操作 → deallocate(nullptr, ...) 同理
 */

#pragma once
#include "ThreadCache.h"

namespace Kama_memoryPool
{

/**
 * MemoryPool — 对外统一接口
 * --------------------------------
 * 纯静态方法，无需实例化。
 * 内部自动路由到 ThreadCache（进而到 CentralCache、PageCache）。
 */
class MemoryPool
{
public:
    /**
     * allocate — 分配指定大小的内存
     * -------------------------------------------------
     * @param size 需要的字节数
     * @return 指向分配内存的指针（至少 8 字节对齐）
     * 
     * 内部流程（对用户透明）：
     *   size ≤ 256KB → ThreadCache → (不够) CentralCache → (不够) PageCache → OS
     *   size > 256KB → 直接 malloc
     */
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    /**
     * deallocate — 释放之前分配的内存
     * -------------------------------------------------
     * @param ptr  要释放的内存指针
     * @param size 当初分配时的大小（必须与 allocate 时一致！）
     * 
     * 【注意】传错 size 会导致内存错误！
     * 正确的 size 用于计算大小类索引，进而找到正确的链表来归还。
     */
    static void deallocate(void* ptr, size_t size)
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace Kama_memoryPool
