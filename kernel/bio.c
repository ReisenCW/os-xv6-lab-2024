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

#define NBUCKETS 13 // 哈希表大小
#define HASH(blockno) ((blockno) % NBUCKETS)

struct bucket
{
  struct spinlock lock; // 每个bucket自己的锁
  struct buf *head;     // 链表头
  char name[16];
};

struct {
  struct spinlock lock;
  struct bucket buckets[NBUCKETS]; // 哈希表
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;
  initlock(&bcache.lock, "bcache"); // 初始化全局块缓存锁
  // 为每个哈希桶创建独立的锁
  for(int i = 0; i < NBUCKETS; ++i){
    snprintf(bcache.buckets[i].name, 16, "bcache_bucket%d", i);
    initlock(&bcache.buckets[i].lock, bcache.buckets[i].name);
  }
  // 每个缓冲区有独立的sleeplock，允许进程在等待 I/O 时休眠,所有缓冲区初始时都链入第一个哈希桶
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.buckets[0].head;
    bcache.buckets[0].head = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  // cache中是否有
  int key = HASH(blockno); // 找到对应哈希索引
  acquire((&bcache.buckets[key].lock)); // 锁定对应的哈希桶
  for(b = bcache.buckets[key].head; b; b = b->next){ // 遍历哈希桶链表中的缓冲区
    if (b->dev == dev && b->blockno == blockno) // 是否是要找的数据
    {
      b->refcnt++; // 引用+1
      release(&bcache.buckets[key].lock);
      acquiresleep(&b->lock);
      return b; // 返回找到的缓冲区
    }
  }
  release(&bcache.buckets[key].lock); // 没找到, 释放哈希桶锁
  int i = key;
  struct buf *prev;
  do // 遍历其它桶
  {
    acquire(&bcache.buckets[i].lock);
    b = bcache.buckets[i].head;
    while (b) // 遍历当前哈希桶的链表
    {
      if (b->refcnt == 0) {// 如果该缓冲区空, 先初始化
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0; // 数据无效
        b->refcnt = 1; // 唯一引用
        if (i != key) // 找到的缓冲区不在原哈希桶, 那么需要将该缓冲区从该哈希桶链表中移除并加入原哈希桶
        {
          // 移除
          if (b != bcache.buckets[i].head)
            prev->next = b->next;
          else
            bcache.buckets[i].head = b->next;
          release(&bcache.buckets[i].lock);
          // 插入(头插法)
          acquire(&bcache.buckets[key].lock);
          b->next = bcache.buckets[key].head;
          bcache.buckets[key].head = b;
          release(&bcache.buckets[key].lock);
        }
        else // 如果在原哈希桶,那么就是直接替换了数据,无需其它操作
        {
          release(&bcache.buckets[i].lock);
        }
        acquiresleep(&b->lock);
        return b;
      }
      prev = b; // 保存前一个缓冲区指针
      b = b->next; // 继续遍历下一个缓冲区
    }
    release(&bcache.buckets[i].lock);
    i = HASH(i + 1); // 没找到可替换的缓冲区, 遍历下一个桶
  } while (i != key);
  panic("bget: no buffers");
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

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int key = HASH(b->blockno);
  acquire(&bcache.buckets[key].lock);
  b->refcnt--;
  release(&bcache.buckets[key].lock);
}

void
bpin(struct buf *b) {
  int key = HASH(b->blockno);
  acquire(&bcache.buckets[key].lock);
  b->refcnt++;
  release(&bcache.buckets[key].lock);
}

void
bunpin(struct buf *b) {
  int key = HASH(b->blockno);
  acquire(&bcache.buckets[key].lock);
  b->refcnt--;
  release(&bcache.buckets[key].lock);
}


