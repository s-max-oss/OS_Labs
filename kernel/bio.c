// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"


#define NBUCKET 31

struct bucket {
    struct spinlock lock;         // 只保护本桶链表及缓存块状态
    struct buf head;              // 循环双向链表的哨兵节点
    struct buf *oldest;           // 当前已知的桶内最老空闲缓存块
    int oldest_valid;             // 为 0 时需在持桶锁状态下重新计算 oldest
};

struct {
    // 仍只保留指导书规定的一份静态缓存数组，哈希桶仅重新组织这些缓存块。
    struct buf buf[NBUF];
    struct bucket bucket[NBUCKET];
    struct spinlock evict_lock;   // 串行化未命中后的跨桶替换
    uint64 use_clock;             // 全局释放序号，用于比较不同桶中的新旧程度
    uint64 lastuse[NBUF];         // 缓存块最近一次变为空闲时的释放序号
} bcache;

static int
bufindex(struct buf *b)
{
    return b - bcache.buf;
}

static int
bucketid(uint dev, uint blockno)
{
    (void)dev;
    return blockno % NBUCKET;
}

// 调用者必须持有 bucket[id].lock。只在候选失效时扫描一次本桶，
// 避免每次全局淘汰都遍历全部 NBUF 个缓存块。
static void
refresh_oldest_locked(int id)
{
    struct bucket *bucket = &bcache.bucket[id];
    struct buf *oldest = 0;

    for (struct buf *b = bucket->head.prev;
         b != &bucket->head;
         b = b->prev) {
        if (b->refcnt == 0) {
            oldest = b;
            break;
        }
    }
    __atomic_store_n(&bucket->oldest, oldest, __ATOMIC_RELAXED);
    __atomic_store_n(&bucket->oldest_valid, 1, __ATOMIC_RELAXED);
}

// 记录缓存块变为空闲的时刻，并将它移到桶内链表头部（最近释放端）。
// 调用者必须持有本桶锁。
static void
mark_unused_locked(int id, struct buf *b, uint64 age)
{
    struct bucket *bucket = &bcache.bucket[id];

    __atomic_store_n(&bcache.lastuse[bufindex(b)], age, __ATOMIC_RELAXED);

    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;

    // 新释放的块是桶内最新块。只有桶中原本没有空闲块时，它才同时是最老块。
    // 若 b 曾是候选但刚经历了一次重新使用，则旧的先后关系已经失效，延迟到
    // 真正需要淘汰时再扫描本桶修复候选。
    if (__atomic_load_n(&bucket->oldest_valid, __ATOMIC_RELAXED) &&
        __atomic_load_n(&bucket->oldest, __ATOMIC_RELAXED) == b) {
        __atomic_store_n(&bucket->oldest_valid, 0, __ATOMIC_RELAXED);
    } else if (__atomic_load_n(&bucket->oldest_valid, __ATOMIC_RELAXED) &&
        __atomic_load_n(&bucket->oldest, __ATOMIC_RELAXED) == 0)
        __atomic_store_n(&bucket->oldest, b, __ATOMIC_RELAXED);
}

// 增加引用计数。调用者持有桶锁，因此缓存块不会在此时被跨桶迁移。
static void
mark_used_locked(struct buf *b)
{
    b->refcnt++;
}

void
binit(void)
{
    initlock(&bcache.evict_lock, "bcache.evict");
    for (int i = 0; i < NBUCKET; i++) {
        // 所有缓存相关锁均以 bcache 开头，便于 bcachetest 统计竞争。
        initlock(&bcache.bucket[i].lock, "bcache.bucket");
        bcache.bucket[i].head.next = &bcache.bucket[i].head;
        bcache.bucket[i].head.prev = &bcache.bucket[i].head;
        __atomic_store_n(&bcache.bucket[i].oldest, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&bcache.bucket[i].oldest_valid, 1,
                         __ATOMIC_RELAXED);
    }

    // 启动时所有缓存块尚未绑定有效磁盘块，先统一挂入 0 号桶。
    // 后续首次分配时，bget 会按 blockno 将它们迁移到目标桶。
    struct buf *head = &bcache.bucket[0].head;
    for (struct buf *b = bcache.buf;b < bcache.buf + NBUF;b++) {
        initsleeplock(&b->lock, "buffer");
        b->next = head->next;
        b->prev = head;
        head->next->prev = b;
        head->next = b;
        __atomic_store_n(&bcache.lastuse[bufindex(b)], ++bcache.use_clock,
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&bcache.bucket[0].oldest, head->prev, __ATOMIC_RELAXED);
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
    struct buf *b;
    struct buf *victim;
    uint64 victim_age;
    int victim_id;
    int id = bucketid(dev, blockno);

    // 命中路径只获取目标桶的一把锁，不经过全局淘汰锁，因此不同桶可并行查找。
    acquire(&bcache.bucket[id].lock);
    for (b = bcache.bucket[id].head.next;
         b != &bcache.bucket[id].head;
         b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            mark_used_locked(b);
            release(&bcache.bucket[id].lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    release(&bcache.bucket[id].lock);

    // 未命中后先获取全局淘汰锁，保证同一时刻只有一个 CPU 能改变缓存块身份
    // 或执行跨桶迁移。锁顺序固定为 evict_lock -> 目标桶 -> 来源桶。
    acquire(&bcache.evict_lock);
    acquire(&bcache.bucket[id].lock);

    // 等待 evict_lock 期间，其他 CPU 可能已经创建了相同缓存，必须二次查找，
    // 从而保证每个 (dev, blockno) 在缓存中最多只有一份。
    for (b = bcache.bucket[id].head.next;
         b != &bcache.bucket[id].head;
         b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            mark_used_locked(b);
            release(&bcache.bucket[id].lock);
            release(&bcache.evict_lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

retry_victim:
    victim = 0;
    victim_age = (uint64)-1;
    victim_id = -1;

    // 乐观读取每个桶的 oldest，只比较最多 NBUCKET 个候选，即可选出全局释放
    // 序号最小的空闲块。候选元数据使用原子访问；候选失效时才持桶锁扫描该桶。
    // 并发释放时这是近似全局 LRU，最终选中的块还会在持锁后重新校验。
    for (int j = 0; j < NBUCKET; j++) {
        struct bucket *bucket = &bcache.bucket[j];
        uint64 age = 0;

retry_bucket:
        if (!__atomic_load_n(&bucket->oldest_valid, __ATOMIC_RELAXED)) {
            if (j != id)
                acquire(&bucket->lock);
            if (!__atomic_load_n(&bucket->oldest_valid, __ATOMIC_RELAXED))
                refresh_oldest_locked(j);
            b = __atomic_load_n(&bucket->oldest, __ATOMIC_RELAXED);
            if (b != 0)
                age = __atomic_load_n(&bcache.lastuse[bufindex(b)],
                                      __ATOMIC_RELAXED);
            if (j != id)
                release(&bucket->lock);
        } else {
            b = __atomic_load_n(&bucket->oldest, __ATOMIC_RELAXED);
            if (b != 0)
                age = __atomic_load_n(&bcache.lastuse[bufindex(b)],
                                      __ATOMIC_RELAXED);
            if (!__atomic_load_n(&bucket->oldest_valid, __ATOMIC_RELAXED))
                goto retry_bucket;
        }

        if (b != 0 && age < victim_age) {
            victim = b;
            victim_age = age;
            victim_id = j;
        }
    }

    if (victim == 0) {
        int found = 0;

        // 乐观扫描可能恰好错过刚变为空闲的块。只有找不到候选时才按桶编号
        // 顺序获取全部桶锁，形成一致快照；其他路径至多持有一个桶锁，因此
        // 这里不会形成反向锁序。确认所有块都在使用后才允许 panic。
        for (int j = 0; j < NBUCKET; j++) {
            if (j != id)
                acquire(&bcache.bucket[j].lock);
        }
        for (int j = 0; j < NBUCKET; j++) {
            if (!__atomic_load_n(&bcache.bucket[j].oldest_valid,
                                 __ATOMIC_RELAXED))
                refresh_oldest_locked(j);
            if (__atomic_load_n(&bcache.bucket[j].oldest,
                                __ATOMIC_RELAXED) != 0)
                found = 1;
        }
        for (int j = NBUCKET - 1; j >= 0; j--) {
            if (j != id)
                release(&bcache.bucket[j].lock);
        }
        if (found)
            goto retry_victim;

        release(&bcache.bucket[id].lock);
        release(&bcache.evict_lock);
        panic("bget: no buffers");
    }

    // 命中路径不获取 evict_lock，所以候选在乐观扫描后可能已被其他 CPU 使用。
    // 持有来源桶锁后重新验证指针、引用计数和释放序号，验证失败就重新选择。
    if (victim_id != id)
        acquire(&bcache.bucket[victim_id].lock);
    if (!__atomic_load_n(&bcache.bucket[victim_id].oldest_valid,
                         __ATOMIC_RELAXED) ||
        __atomic_load_n(&bcache.bucket[victim_id].oldest,
                        __ATOMIC_RELAXED) != victim ||
        victim->refcnt != 0 ||
        __atomic_load_n(&bcache.lastuse[bufindex(victim)],
                        __ATOMIC_RELAXED) != victim_age) {
        if (__atomic_load_n(&bcache.bucket[victim_id].oldest,
                            __ATOMIC_RELAXED) == victim)
            __atomic_store_n(&bcache.bucket[victim_id].oldest_valid, 0,
                             __ATOMIC_RELAXED);
        if (victim_id != id)
            release(&bcache.bucket[victim_id].lock);
        goto retry_victim;
    }

    __atomic_store_n(&bcache.bucket[victim_id].oldest_valid, 0,
                     __ATOMIC_RELAXED);

    if (victim_id != id) {
        victim->prev->next = victim->next;
        victim->next->prev = victim->prev;

        struct buf *head = &bcache.bucket[id].head;
        victim->next = head->next;
        victim->prev = head;
        head->next->prev = victim;
        head->next = victim;
    }

    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;

    if (victim_id != id)
        release(&bcache.bucket[victim_id].lock);
    release(&bcache.bucket[id].lock);
    release(&bcache.evict_lock);

    acquiresleep(&victim->lock);
    return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个持有睡眠锁的缓存块。引用计数归零时记录全局释放顺序，
// 供下一次未命中选择近似全局最老的空闲块。
void
brelse(struct buf *b)
{
    uint64 age;

    if(!holdingsleep(&b->lock)) panic("brelse");

    releasesleep(&b->lock);
    // 在获取桶锁前分配全局序号，避免共享计数器延长桶锁临界区。
    age = __sync_add_and_fetch(&bcache.use_clock, 1);

    int id = bucketid(b->dev, b->blockno);
    struct bucket *bucket = &bcache.bucket[id];

    acquire(&bucket->lock);
    b->refcnt--;
    if(b->refcnt == 0)
        mark_unused_locked(id, b, age);

    release(&bucket->lock);
}

void
bpin(struct buf *b)
{
  // refcnt 及块所在链表都由其哈希桶锁保护。
  int id = bucketid(b->dev, b->blockno);
  acquire(&bcache.bucket[id].lock);
  mark_used_locked(b);
  release(&bcache.bucket[id].lock);
}

void
bunpin(struct buf *b)
{
  // bunpin 也可能使引用计数归零，因此必须和 brelse 一样更新时间戳及候选。
  uint64 age = __sync_add_and_fetch(&bcache.use_clock, 1);
  int id = bucketid(b->dev, b->blockno);
  acquire(&bcache.bucket[id].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    mark_unused_locked(id, b, age);
  release(&bcache.bucket[id].lock);
}

