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

struct {
  struct buf buf[NBUF];
	// if a thread wants to access a buffer, it needs to hold the lock of that buffer
	// the index of that lock is a buf->blockno % NBUCKETS, like a hashfunc.
	struct spinlock locks[NBUCKETS];	// avoid lock contention
	struct spinlock biglock;	// to serialize some part of code
} bcache;

void
binit(void)
{
  struct buf *b;
	for(int i = 0; i < NCPU; i++) {
		for(b = bcache.buf; b < bcache.buf+NBUF; b++){
			initsleeplock(&b->lock, "buffer");
			b->refcnt = 0;
			b->blockno = 0xffffffff;
			b->ticks = ticks;
		}
	}
	for(int i = 0; i < NBUCKETS; i++) {
		initlock(&bcache.locks[i], "bcache.buckets");
	}
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
	// big lock is try for debugging, but accidentally resolve the problem of failing
	// manywrites in usertests, though I still don't know why...
	acquire(&bcache.biglock);		
	int index = blockno % NBUCKETS;
	acquire(&bcache.locks[index]);
	// check if in the cached buffer
	struct buf* b;
	for(b = bcache.buf; b < bcache.buf + NBUF; b++){
		if(b->dev == dev && b->blockno == blockno){
			b->refcnt++;
			b->ticks = ticks;
			release(&bcache.locks[index]);
			release(&bcache.biglock);
			acquiresleep(&b->lock);
			return b;
		}
	}
	release(&bcache.biglock);

	// If not cache, then recycle the least recently used (LRU) unused buffer.
	uint minticks = 0xffffffff;
	struct buf* bLRU = nullptr;
	for(b = bcache.buf; b < bcache.buf + NBUF; b++) {
		if(b->refcnt == 0 && b->ticks < minticks) {
			bLRU = b;
			minticks = b->ticks;
		}
	}
	if (bLRU) {
		bLRU->dev = dev;
		bLRU->blockno = blockno;
		bLRU->valid = 0;
		bLRU->refcnt = 1;
		bLRU->ticks = ticks;
		release(&bcache.locks[index]);
		acquiresleep(&bLRU->lock);
		return bLRU;
	}
	panic("no available buffer in the cache");
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

	int index = b->blockno % NBUCKETS;
	acquire(&bcache.locks[index]);
	b->refcnt--;
  if (b->refcnt == 0) {
		b->dev = 0xffffffff;
		b->blockno = 0xffffffff;
		b->ticks = ticks;
  }
	release(&bcache.locks[index]);
}

void
bpin(struct buf *b) {
	int index = b->blockno % NBUCKETS;
	acquire(&bcache.locks[index]);
  b->refcnt++;
	release(&bcache.locks[index]);
}

void
bunpin(struct buf *b) {
	int index = b->blockno % NBUCKETS;
	acquire(&bcache.locks[index]);
  b->refcnt--;
	release(&bcache.locks[index]);
}
