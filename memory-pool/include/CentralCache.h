/**
 * ============================================================
 * CentralCache.h — 中心缓存（三级缓存的第二级）
 * ============================================================
 * 
 * 【位置】三级缓存：ThreadCache → CentralCache → PageCache → OS
 * 
 * 【核心设计思想 — 为什么需要 CentralCache？】
 * 
 * 问题：ThreadCache 是"零售"的（一个个内存块分发给线程），
 *       它的货从哪里来？不能直接找 OS（太慢太浪费）。
 * 
 * 答案：CentralCache 充当"批发总代理"——
 * 
 *   - 向上（PageCache）：把连续的页（Span）切成等大小的内存块
 *   - 向下（ThreadCache）：批量供给/回收内存块
 * 
 * 【自旋锁（spin lock）设计 — 关键细节！】
 * 
 * 这是本文件最精妙的设计点：
 * 
 * 传统做法：一个全局大锁保护整个 CentralCache。
 *   问题：所有线程都要抢同一把锁，高并发下严重争抢。
 * 
 * 本项目的做法：为每个大小类分配一把独立的自旋锁！
 *   好处：
 *   - 线程A申请16字节、线程B申请256字节 → 锁的是不同大小类 → 完全不冲突
 *   - 只有两个线程申请同一大小类时才需要竞争
 *   - 大大减少锁竞争，提升并发性能
 * 
 * 【为什么用 std::atomic_flag 而不是 std::mutex？】
 *   - spin lock 用于保护极短的关键区（就几个指针操作）
 *   - mutex 会让线程进入内核态休眠，唤醒开销大
 *   - spin lock 在用户态忙等，适合"锁持有时间极短"的场景
 *   - 对于内存分配这种高频操作，spin lock 比 mutex 效率高很多
 * 
 * 【数据结构】
 *   centralFreeList_[i]：大小类 i 的空闲块链表（原子指针，线程安全）
 *   locks_[i]：大小类 i 的自旋锁
 */

#pragma once
#include "Common.h"
#include <mutex>

namespace Kama_memoryPool
{

/**
 * CentralCache — 中心缓存（全局单例）
 * ----------------------------------------
 * 是 ThreadCache 和 PageCache 之间的桥梁：
 * - 从 PageCache 拿 Span（连续页）
 * - 把 Span 切成固定大小的小块
 * - 批量供给给 ThreadCache
 */
class CentralCache
{
public:
    /**
     * getInstance — 全局单例（不是 thread_local！）
     * -------------------------------------------------
     * 注意：这里用 static（不是 static thread_local）！
     * ThreadCache 是 thread_local（每线程独立），
     * CentralCache 是全局唯一（所有线程共享），需要加锁保护。
     */
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    /**
     * fetchRange — 批量取出内存块给 ThreadCache
     * -------------------------------------------------
     * @param index    大小类索引
     * @param batchNum [输入/输出] 想要的块数 → 实际拿到的块数
     * @return 返回链表的头指针（所有块已串好），不够时从 PageCache 补充
     * 
     * 线程安全的批量获取：用锁保护，防止多线程同时操作同一大小类
     */
    void* fetchRange(size_t index, size_t &batchNum);

    /**
     * returnRange — ThreadCache 批量归还多余的内存块
     * -------------------------------------------------
     * @param start 归还链表头指针
     * @param size  每个块的大小
     * @param bytes 总字节数（用于计算块数）
     * 
     * 归还的块重新挂回 centralFreeList_，供其他线程使用
     */
    void returnRange(void* start, size_t size, size_t bytes);

private:
    /**
     * 构造函数 — 初始化自由链表和自旋锁
     * -------------------------------------------
     * 两个初始化步骤：
     * 1. 所有原子指针设为 nullptr
     * 2. 所有自旋锁设为 clear（未锁定状态）
     */
    CentralCache()
    {
        for (auto& ptr : centralFreeList_)
        {
            ptr.store(nullptr, std::memory_order_relaxed);
        }
        // 初始化所有自旋锁为未锁定状态
        for (auto& lock : locks_)
        {
            lock.clear();
        }
    }

    /**
     * fetchFromPageCache — 从 PageCache 获取新 Span
     * -------------------------------------------------
     * 当 centralFreeList_[index] 不够用时调用，
     * 从 PageCache 拿连续的页，切成小块后返回
     */
    void* fetchFromPageCache(size_t size);

private:
    /**
     * centralFreeList_ — 中心自由链表数组
     * -------------------------------------------
     * 【关键】用 std::atomic<void*> 而不是普通 void*
     * 原子操作保证指针操作的线程安全性，避免 data race
     * 
     * 索引 = 大小类（0~32767），值 = 指向第一个空闲块的原子指针
     */
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    /**
     * locks_ — 每大小类的自旋锁
     * -------------------------------------------
     * 【核心设计】细粒度锁 — 每个大小类独立一把锁！
     * 
     * std::atomic_flag 是 C++ 标准库提供的唯一保证无锁的原子类型，
     * 用 test_and_set / clear 来实现自旋锁：
     *   - 加锁：while (flag.test_and_set()) {}  // 忙等直到成功
     *   - 解锁：flag.clear()
     * 
     * 为什么不用 std::mutex？
     *   关键区极短（几个指针操作），自旋等待比内核态切换更快
     */
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
};

} // namespace Kama_memoryPool
