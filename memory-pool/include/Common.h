/**
 * ============================================================
 * Common.h — 内存池全局定义和基础设施
 * ============================================================
 * 
 * 【文件定位】
 * 这是整个内存池项目的"根基"文件，被所有其他模块引用。
 * 定义了对齐规则、内存块元数据、以及"大小类"计算工具。
 * 
 * 【为什么需要大小类？】
 * 内存池不能为每个字节大小都维护一个链表（太多太碎）。
 * 所以把所有请求"归类"到有限个大小级别中。
 * 比如：请求7字节 → 归到8字节类；请求15字节 → 归到16字节类。
 * 
 * 【核心概念】
 * - ALIGNMENT=8：最小对齐单位，也是指针大小
 * - MAX_BYTES=256KB：内存池能管理的最大内存块
 * - FREE_LIST_SIZE=MAX_BYTES/ALIGNMENT=32768：总共有这么多大小类
 * - SizeClass：把任意字节数"映射"为大小类索引的工具
 */

#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace Kama_memoryPool 
{

/**
 * ALIGNMENT = 8 字节
 * -------------------
 * 为什么是8？
 * 1. 64位系统指针大小为8字节，最小对齐要求就是8
 * 2. 所有分配的内存地址必须是8的倍数
 * 3. 内存池最小分配单元就是8字节
 */
constexpr size_t ALIGNMENT = 8;

/**
 * MAX_BYTES = 256KB
 * -------------------
 * 内存池管理的最大单次分配大小。
 * 超过这个大小的申请会直接走系统 malloc/free，
 * 不经过内存池的三级缓存。
 */
constexpr size_t MAX_BYTES = 256 * 1024; // 256KB

/**
 * FREE_LIST_SIZE = 32768
 * -------------------
 * 自由链表数组的大小 = MAX_BYTES / ALIGNMENT
 * 每个链表对应一个"大小类"。
 * 索引0 → 8字节（1×8），索引1 → 16字节（2×8），……，索引32767 → 256KB
 */
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;

/**
 * BlockHeader — 内存块头部信息
 * --------------------------------
 * 描述一个内存块的元数据。
 * 而是通过外部 Span 来管理块信息。这里保留作为接口约定。
 */
struct BlockHeader
{
    size_t size;      // 该内存块的大小
    bool   inUse;      // 是否正在被使用
    BlockHeader* next; // 指向下一个内存块（自由链表用）
};

/**
 * SizeClass — 大小类管理
 * --------------------------------
 * 【核心作用】把任意字节数映射为数组索引
 * 
 * 举例：
 * 请求7字节  → roundUp(7)=8  → getIndex(7)=0  → 使用freeList_[0]（8字节链表）
 * 请求8字节  → roundUp(8)=8  → getIndex(8)=0  → 使用freeList_[0]（8字节链表）
 * 请求9字节  → roundUp(9)=16 → getIndex(9)=1  → 使用freeList_[1]（16字节链表）
 * 请求256KB → getIndex=32767 → 使用freeList_[32767]
 * 
 * getIndex 公式推导：
 * index = (roundUp(bytes) / ALIGNMENT) - 1
 *       = ((bytes + 7) / 8) - 1
 * 
 * 为什么减1？因为数组从0开始，大小为8对应index=0
 */
class SizeClass 
{
public:
    /**
     * roundUp — 向上对齐到 ALIGNMENT 的倍数
     * -------------------------------------------
     * 位运算技巧：(x + 7) & ~7  等价于 将x向上取整到8的倍数
     * // 向上对齐到 8 的倍数（等效于 ceil(x/8)*8）
     * 例：7→8, 8→8, 9→16, 15→16, 16→16
     */
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    /**
     * getIndex — 获取大小类索引
     * -------------------------------------------
     * 先将字节数对齐到8，再除以8减1得到数组下标
     * 最小返回0（对应8字节），最大返回32767（对应256KB）
     */
    static size_t getIndex(size_t bytes)
    {   
        // 确保至少为ALIGNMENT，防止传入0导致负数索引
        bytes = std::max(bytes, ALIGNMENT);
        // (对齐后的字节数 / 8) - 1 = 数组索引
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace Kama_memoryPool
