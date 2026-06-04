/**
 * ============================================================
 * ThreadCache.cpp — 线程本地缓存实现
 * ============================================================
 *
 * 【文件定位】
 * 三级缓存中离用户代码最近的一级，负责"零售"内存块。
 * 每个线程独立拥有一个 ThreadCache，通过 thread_local 实现无锁访问。
 *
 * 【嵌入式链表 (Intrusive Linked List) — 本项目最核心的技巧】
 * 
 * 传统链表：Node { data; Node* next; }
 *   → 需要额外的 Node 结构体，内存开销大
 * 
 * 嵌入式链表：把 next 指针直接存在空闲内存块的前 8 字节里：
 *   void* ptr = 某空闲块的地址;
 *   void* next = *reinterpret_cast<void**>(ptr);  // 读取前8字节 = 下一个块
 *   *reinterpret_cast<void**>(ptr) = head;        // 写入前8字节 = 链接到链表
 *
 * 好处：
 *   1. 零额外内存开销（复用空闲块本身就占用的空间）
 *   2. O(1) 插入/删除（头插头取）
 *   3. 不需要维护额外的节点结构体
 *
 * 【批量传输策略】
 * ThreadCache 从 CentralCache 一次拿一批（batch），不是一个个拿。
 * 理由：每次跨级通信都有开销（锁、函数调用），批量可以平摊这个开销。
 * 
 * 批量大小不是固定的——小块拿更多（减少通信次数），大块拿更少（避免浪费）。
 * 详见 getBatchNum()。
 */

#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <cstdlib>

namespace Kama_memoryPool
{

/**
 * allocate — 分配内存
 * -----------------------
 * 这是用户调用 MemoryPool::allocate(size) 后最终到达的第一站。
 *
 * 流程：
 * 1. 如果是大对象（>256KB）→ 直接 malloc，不经过内存池
 * 2. 计算大小类 → 从对应链表取一个块
 * 3. 如果链表不为空 → 头节点出队，返回（最快路径，无锁！）
 * 4. 如果链表为空 → 从 CentralCache 批量取一批
 */
void* ThreadCache::allocate(size_t size)
{
    // 处理0大小的分配请求：至少分配 8 字节
    if (size == 0)
    {
        size = ALIGNMENT;
    }
    
    // 大对象（>256KB）直接走系统 malloc，绕过内存池
    if (size > MAX_BYTES)
    {
        return malloc(size);
    }

    // 计算大小类索引：size=15 → index=1 (16字节类)
    size_t index = SizeClass::getIndex(size);

    // 预减计数（先假设能取到，取不到后面会补回来）
    freeListSize_[index]--;

    // 检查本地自由链表是否有可用块
    // 【关键代码】嵌入式链表出队：
    //   1. ptr = 头节点地址
    //   2. freeList_[index] = *ptr（把第二个节点变成新头）
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    // 本地链表为空 → 去中心缓存"批发"一批
    return fetchFromCentralCache(index);
}

/**
 * deallocate — 释放内存
 * -----------------------
 * 归还内存块到本地自由链表。
 *
 * 流程：
 * 1. 大对象 → 直接 free
 * 2. 计算大小类 → 插入到对应链表头部（O(1)）
 * 3. 计数+1
 * 4. 如果链表太长（>64个）→ 批量归还一部分给 CentralCache
 */
void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 【关键代码】嵌入式链表入队（头插法，O(1)）：
    //   1. 把当前头节点地址写入 ptr 的前 8 字节
    //   2. 把 ptr 设为新头
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    freeListSize_[index]++;

    // 链表太长就归还一部分，防止一个线程囤积太多内存
    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

/**
 * shouldReturnToCentralCache — 判断是否需要归还
 * -------------------------------------------------
 * 阈值 = 64 个块。
 * 当一个大小类的空闲块超过 64 个时，
 * 说明本地囤积太多了，应该退一部分给中心缓存让其他线程用。
 */
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    constexpr size_t threshold = 64;
    return (freeListSize_[index] > threshold);
}

/**
 * fetchFromCentralCache — 从 CentralCache 批发内存块
 * -------------------------------------------------------
 *
 * 流程：
 * 1. 计算要批发多少个（getBatchNum）
 * 2. 调用 CentralCache::fetchRange 批量取
 * 3. 取第一个返回给用户，剩余的挂到本地链表
 *
 * 实际上 freeListSize_[index] 记录的是在本地链表中的空闲块数。
 * allocate 开头预减了一个（假设能从本地取到），
 * 现在发现要批量取，需要纠正计数。
 */
void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 还原实际字节数：index=0 → 8B, index=1 → 16B ...
    size_t size = (index + 1) * ALIGNMENT;

    // 根据对象大小计算合理的批量数量
    size_t batchNum = getBatchNum(size);

    // 从中心缓存批量获取
    void* start = CentralCache::getInstance().fetchRange(index, batchNum);
    if (!start) return nullptr;

    // 更新本地链表计数：
    // batchNum 个中取 1 个返回用户，剩下 batchNum-1 个挂本地链表
    freeListSize_[index] += batchNum - 1;

    // 取第一个块返回，其余块挂到本地链表
    void* result = start;
    if (batchNum > 1)
    {
        // start 之后的所有块已经在 CentralCache 里串好了
        freeList_[index] = *reinterpret_cast<void**>(start);
    }
    
    return result;
}

/**
 * returnToCentralCache — 批量归还内存块给 CentralCache
 * ----------------------------------------------------------
 *
 * 归还策略：保留 1/4，归还 3/4
 * 这是"延迟归还"策略——不完全清空，留一点以备下次使用，
 * 避免刚归还又要重新申请。
 *
 * 流程：
 * 1. 计算要保留的数量（keepNum = batchNum / 4）
 * 2. 遍历链表找到分割点
 * 3. 在分割点断开链表
 * 4. 前半段保留在本地，后半段归还给 CentralCache
 */
void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);
    size_t alignedSize = SizeClass::roundUp(size);

    size_t batchNum = freeListSize_[index];// 链表里一共几个块
    if (batchNum <= 1) return; // 只剩一个就不归还了

    // 保留 1/4，归还 3/4
    size_t keepNum = std::max(batchNum / 4, size_t(1)); //保留几个
    size_t returnNum = batchNum - keepNum;//还几个

    // 遍历链表找到"保留段"和"归还段"的分割点
    char* current = static_cast<char*>(start);
    char* splitNode = current;//从块1开始走
    for (size_t i = 0; i < keepNum - 1; ++i) //走多少步
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));//读前8字节 = 走到下一个块
        if (splitNode == nullptr) 
        {
            // 链表比预期短（罕见情况），调整归还数量
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if (splitNode != nullptr) 
    {
        // 断开链表：splitNode 是保留段的最后一个节点
        void* nextNode = *reinterpret_cast<void**>(splitNode); // 归还段的头
        *reinterpret_cast<void**>(splitNode) = nullptr; // 断开！

        // 保留段重新挂到本地链表
        freeList_[index] = start;
        freeListSize_[index] = keepNum;

        // 归还段送回 CentralCache
        if (returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

/**
 * getBatchNum — 计算每次批发的块数
 * ---------------------------------------
 *
 * 【核心策略】小块多拿，大块少拿
 * 
 * 小块（≤32B）：一次拿 64 个  → 每次批发 2KB 左右
 * 大块（>1KB）： 一次拿 1 个   → 不囤积，用多少拿多少
 * 
 * 理由：
 * - 小块用得频繁，多拿减少与 CentralCache 的通信次数
 * - 大块容易浪费，囤积太多会占用内存
 *
 * 同时也受 MAX_BATCH_SIZE = 4KB 的限制：
 * 确保每次批发不超过 4KB（约一个页的大小）
 */
size_t ThreadCache::getBatchNum(size_t size)
{
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 每次最多批发 4KB

    // 根据对象大小确定基准批量数 尽量控制在2KB
    size_t baseNum;
    if (size <= 32)      baseNum = 64;   // 小块：64个，约2KB
    else if (size <= 64)  baseNum = 32;  // 32×64=2KB
    else if (size <= 128) baseNum = 16;  // 16×128=2KB
    else if (size <= 256) baseNum = 8;   // 8×256=2KB
    else if (size <= 512) baseNum = 4;   // 4×512=2KB
    else if (size <= 1024)baseNum = 2;   // 2×1024=2KB
    else                   baseNum = 1;   // 大块只拿1个

    // 但不能超过 4KB 的硬限制
    size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);

    // 取基准值和上限中较小的那个
    return std::max(size_t(1), std::min(maxNum, baseNum));
    //std::min(maxNum, baseNum)：不能同时满足"多拿"和"不超 4KB"时，听 4KB 的
}

} // namespace Kama_memoryPool
