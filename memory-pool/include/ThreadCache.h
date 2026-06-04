/**
 * ============================================================
 * ThreadCache.h — 线程本地缓存（三级缓存的第一级）
 * ============================================================
 * 
 * 【位置】三级缓存：ThreadCache → CentralCache → PageCache → OS
 * 
 * 【核心设计思想 — 为什么需要 ThreadCache？】
 * 
 * 如果所有线程都直接向 CentralCache（全局单例）申请内存，
 * 那么每次分配/释放都需要加锁竞争。
 * ThreadCache 的思路是：
 * 
 *   每个线程拥有自己私有的缓存，大部分分配从自己的缓存取，
 *   不需要加锁！只有当缓存空了/满了才去找 CentralCache。
 * 
 * 这叫做 "Thread-Local Storage" + "Batch Transfer" 模式：
 * - 线程从 CentralCache 一次"批发"一堆内存块
 * - 在自己的 ThreadCache 里"零售"给代码使用
 * - 缓存太多时把多余的"退货"给 CentralCache
 * 
 * 【thread_local 单例】
 * getInstance() 返回的是 thread_local 实例 —
 * 每个线程有独立的 ThreadCache，天然线程安全，无需加锁！
 * 
 * 【数据结构】
 * freeList_[32768]：每个大小类对应一个指针，
 *                  指向该大小类的第一个空闲内存块。
 *                  所有同大小的块通过 reinterpret_cast<void**> 串成链表。
 * freeListSize_[32768]：记录每个链表中空闲块的数量，
 *                       用于判断"太多了需要归还"还是"不够了需要批发"。
 */

#pragma once
#include "Common.h"

namespace Kama_memoryPool 
{

/**
 * ThreadCache — 线程本地缓存
 * -------------------------------
 * 三级缓存的第一级，离用户代码最近，速度最快。
 * 每个线程独享实例，无锁访问。
 */
class ThreadCache
{
public:
    /**
     * getInstance — 获取本线程的 ThreadCache 单例
     * -------------------------------------------------
     * 【关键技术点】static thread_local
     * - thread_local：每个线程有独立的 instance 副本
     * - static：懒初始化，第一次调用时构造
     * - 结果：天然线程安全，无需任何锁
     */
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;
        return &instance;
    }

    /**
     * allocate — 分配内存
     * -------------------------------
     * 1. 小对象（≤256KB）：从线程本地自由链表取
     * 2. 大对象（>256KB）：直接走系统 malloc
     */
    void* allocate(size_t size);

    /**
     * deallocate — 释放内存
     * -------------------------------
     * 1. 小对象：归还到线程本地自由链表
     * 2. 大对象：直接走系统 free
     * 3. 如果某个链表太长，触发批量归还给 CentralCache
     */
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache() = default;

    /**
     * fetchFromCentralCache — 从中心缓存"批发"内存块
     * ----------------------------------------------------
     * 当本地自由链表为空时调用，一次性批量取回多个块
     */
    void* fetchFromCentralCache(size_t index);

    /**
     * returnToCentralCache — 归还内存块给中心缓存
     * ----------------------------------------------------
     * 当本地自由链表太长时，把多余的内存块退回
     */
    void returnToCentralCache(void* start, size_t size);

    /**
     * getBatchNum — 计算每次批发的数量
     * ----------------------------------------------------
     * 块越小批发越多：小块一次64个，大块一次1个
     * 这样小块减少交互次数，大块不浪费
     */
    size_t getBatchNum(size_t size);

    /**
     * shouldReturnToCentralCache — 判断是否需要归还
     * ----------------------------------------------------
     * 当前链表超过阈值（64个块）时触发归还
     */
    bool shouldReturnToCentralCache(size_t index);

private:
    /**
     * freeList_ — 自由链表数组（核心数据结构）
     * -------------------------------------------
     * 索引 = 大小类（0~32767）
     * 值 = 指向该大小类的第一个空闲块
     * 
     * 【关键技巧】嵌入式链表
     * 空闲块本身的内存空间被用来存储"next指针"，
     * 不需要额外的内存开销！这是内存池高效的关键。
     */
    std::array<void*, FREE_LIST_SIZE> freeList_;    

    /**
     * freeListSize_ — 各链表空闲块计数
     * -------------------------------------------
     * 跟踪每个大小类的空闲块数量，
     * 用于判断是否需要批发或退货
     */
    std::array<size_t, FREE_LIST_SIZE> freeListSize_;
};

} // namespace Kama_memoryPool
