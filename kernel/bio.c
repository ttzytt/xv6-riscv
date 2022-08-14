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

#define BUCK_SIZ 13
#define BCACHE_HASH(dev, blk) (((dev << 27) | blk) % BUCK_SIZ) // 支持多个 devv
// or 13, 1009, 10007
struct {
  struct spinlock bhash_lk[BUCK_SIZ]; // buf hash lock
  struct spinlock evict_lk; // 持有这个锁之后会去检查应该去驱逐哪个块的缓存
  struct buf bhash_head[BUCK_SIZ]; // 每个桶的开头，不用 buf* 是因为我们需要得到某个 buf 前面的 buf

  struct buf buf[NBUF]; // 最终的缓存

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
} bcache;

void
binit(void)
{
  for (int i = 0; i < BUCK_SIZ; i++){
    initlock(&bcache.bhash_lk[i], "bcache buf hash lock");
    bcache.bhash_head[i].next = 0;
  }

  initlock(&bcache.evict_lk, "bcache eviction lock");

  for(int i = 0; i < NBUF; i++){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buf sleep lock");
    b->lst_use = 0;
    b->refcnt = 0;
    b->next = bcache.bhash_head[0].next; // 往 0 的头上插
    bcache.bhash_head[0].next = b;
  }
}

struct buf* bfind_prelru(int* lru_bkt){ // 返回 lru 前面的一个，并且加锁
  struct buf* lru_res = 0;
  *lru_bkt = -1;
  struct buf* b;
  for(int i = 0; i < BUCK_SIZ; i++){
    acquire(&bcache.bhash_lk[i]);
    int found_new = 0; // 在这个桶里找到了新的，我们要一直拿着 lru 桶的锁
    for(b = &bcache.bhash_head[i]; b->next; b = b->next){ 
      if(b->next->refcnt == 0 && (!lru_res || b->next->lst_use < lru_res->next->lst_use)){
        lru_res = b;
        found_new = 1;
      }
    }
    if(!found_new){
      release(&bcache.bhash_lk[i]);
    }else{ // 有更好的选择
      if(*lru_bkt != -1) release(&bcache.bhash_lk[*lru_bkt]);
      *lru_bkt = i;
    }
  }
  return lru_res;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint key = BCACHE_HASH(dev, blockno);
  // 查看是否被缓存 
  acquire(&bcache.bhash_lk[key]);
  for(b = bcache.bhash_head[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bhash_lk[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bhash_lk[key]);
  // 释放桶锁之后，拿到驱逐锁之前，可能会有释放出来的缓存
  acquire(&bcache.evict_lk);

  acquire(&bcache.bhash_lk[key]);
  for(b = bcache.bhash_head[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bhash_lk[key]);
      release(&bcache.evict_lk);  
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bhash_lk[key]);
  int lru_bkt;
  struct buf* pre_lru = bfind_prelru(&lru_bkt);
  if(pre_lru == 0){
    panic("bget: no buffers");
  }
  
  // 从另一个桶里偷缓存
  struct buf* lru = pre_lru->next;
  pre_lru->next = lru->next; // 用不到 pre
  release(&bcache.bhash_lk[lru_bkt]);
  
  acquire(&bcache.bhash_lk[key]);
  lru->next = bcache.bhash_head[key].next;
  bcache.bhash_head[key].next = lru;
  
  lru->dev = dev, lru->blockno = blockno;
  lru->valid = 0, lru->refcnt = 1; 

  release(&bcache.bhash_lk[key]);
  release(&bcache.evict_lk);  

  acquiresleep(&lru->lock);
  return lru;
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer. 
  
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

  uint key = BCACHE_HASH(b->dev, b->blockno);

  acquire(&bcache.bhash_lk[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->lst_use = ticks;
  }
  
  release(&bcache.bhash_lk[key]);
}

void
bpin(struct buf *b) {
  uint key = BCACHE_HASH(b->dev, b->blockno);
  acquire(&bcache.bhash_lk[key]);
  b->refcnt++;
  release(&bcache.bhash_lk[key]);
}

void
bunpin(struct buf *b) {
  uint key = BCACHE_HASH(b->dev, b->blockno);
  acquire(&bcache.bhash_lk[key]);
  b->refcnt--;
  release(&bcache.bhash_lk[key]);
}


