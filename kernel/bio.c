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

//#define dbg

/* implement a thread-safe hashtable for looking up the blockno in bcache */
/* I use the double hash function combined with a linear probing method */
/* The memory allocator in kernel is kalloc() which allocates a 4K page at once */
/* It's hard to allocate specific small single space for each hashnode with this allocator */
/* Thus, our buckets are not listed, but with a fixed array size */

struct hashnode {
	char key[BLKLEN];
	int val;
	struct spinlock lock;
};

uint64 hashfunc1(char key[], int tablesize) {
	uint64 hash = 5381;
	uint64 c;
	while ((c = *key++)) {
		hash = (hash << 5) + hash + c;	// hash = 33 * hash + c
	}
	return hash % tablesize;
}

uint64 hashfunc2(char key[], int tablesize) {
	uint64 hash = 5381;
	uint64 c;
	while ((c = *key++)) {
		hash = (hash << 5) + hash + c;	// hash = 33 * hash + c
	}
	return 1 + hash % (tablesize - 1);	// prime with hashfunc1
}

struct {
	struct hashnode buckets[NBUCKETS];
} hashtable;

void init_hashtable() {
	for(int i = 0; i < NBUCKETS; i++) {
		struct hashnode* node = &hashtable.buckets[i];
		initlock(&node->lock, "bucket");
		acquire(&node->lock);
		memset(node->key, 0, BLKLEN);	// empty string
		node->val = -1;								// bucket is not owned
		release(&node->lock);
	}
}

// transfer the search result more easily
struct stat {
	int val;		// which cpu thread holds the key
	int index;	// which bucket in that cpu thread's hashtable
};

/* core function: insert and delete all share this common lookup method */
/* thus lookup() is very specified. lookup() takes 2 hash functions and */
/* 1 linear probing to avoid the confliction as much as possible. */
/* @param 'delete' is needed because when two key maps to the same */
/* bucket with hashfunc1, one of them must be mapped to another bucket, */
/* and once they are deleted, the first one can be deleted in the right */
/* way while the other will be found non-existent, because it will be */
/* detected by the hashfunc1 at first, whose bucket is empty after the */
/* first one has been deleted, but it should be examined again with  */
/* hashfunc2 and linear probing. */
struct stat lookup_hashtable(char key[], int delete) {
	if (key == nullptr) {
		panic("lookup_hashtable(): key cannot be null\n");
	}
	if (strlen(key) > BLKLEN || strlen(key) <= 0) {
		panic("lookup_hashtable(): key length is invalid\n");
	}
	struct stat s;
	s.val = -1;
	s.index = hashfunc1(key, NBUCKETS);
	struct hashnode* node = &hashtable.buckets[s.index];
	acquire(&node->lock);
#ifdef dbg
	printf("\n====== lookup_hashtable(): ======\n");
	printf("  index: %d\n  dst: %s\n  src: %s\n  val: %d\n", s.index, node->key, key, node->val);
	printf("=================================\n");
#endif
	if (node->val == -1 && !delete) {	// the key is not in the bucket and not lookup for delete
		release(&node->lock);
		return s;
	}
	if (node->val != -1 && strncmp(node->key, key, BLKLEN) == 0) {	// find the key
		s.val = node->val;
		release(&node->lock);
		return s;
	}
	release(&node->lock);

	// if an confliction happens, then use another hashfunc to find a new bucket
	s.index = hashfunc2(key, NBUCKETS);
	struct hashnode* node_2 = &hashtable.buckets[s.index];
	acquire(&node_2->lock);
#ifdef dbg
	printf("\n====== lookup_hashtable(): ======\n");
	printf("  index: %d\n  dst: %s\n  src: %s\n  val: %d\n", s.index, node_2->key, key, node_2->val);
	printf("=================================\n");
#endif
	if (node_2->val == -1 && !delete) {	// the key not in the bucket and the lookup is not for delete
		release(&node_2->lock);
		return s;
	}
	if (node_2->val != -1 && strncmp(node_2->key, key, BLKLEN) == 0) {	// find the key
		s.val = node_2->val;
		release(&node_2->lock);
		return s;
	}
	release(&node_2->lock);

	// if still exist confliction, then a linear probe has to be done
	int b_available = 0;	// store the first available bucket's index
	int i = (s.index + 1) % NBUCKETS;
	while (i != s.index) {
		struct hashnode* node_i = &hashtable.buckets[i];
		acquire(&node_i->lock);
		// store the first available bucket if the key not exist at last
		if (node_i->val == -1 && b_available == 0) {	
			s.index = i;
			b_available = 1;
		}
		if (strncmp(node_i->key, key, BLKLEN) == 0) {	// finally find the key
			s.index = i;
			s.val = node_i->val;
			release(&node_i->lock);
			return s;
		}
		i = (i + 1) % NBUCKETS;
		release(&node_i->lock);
	}
	if (!b_available) {	// no buckets are available
		panic("lookup_hashtable(): hash confliction cannot be handled\n");
	}
	return s;
}

void insert_hashtable(char key[], int val, int index) {
	struct hashnode* node = &hashtable.buckets[index];
	acquire(&node->lock);
	safestrcpy(node->key, key, BLKLEN);
	node->val = val;
	release(&node->lock);
}

void delete_hashtable(char key[], int index) {
	struct hashnode* node = &hashtable.buckets[index];
	acquire(&node->lock);
	memset(node->key, 0, BLKLEN);
	node->val = -1;
	release(&node->lock);
}

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache[NCPU];

void
binit(void)
{
  struct buf *b;
	for(int i = 0; i < NCPU; i++) {
  	initlock(&bcache[i].lock, "bcache");
		for(b = bcache[i].buf; b < bcache[i].buf+NBUF; b++){
			initsleeplock(&b->lock, "buffer");
			b->ticks = ticks;
		}
	}
	init_hashtable();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
	char buf_blockno[BLKLEN] = {0};
	snprintf(buf_blockno, sizeof(buf_blockno), "%d", blockno);

#ifdef dbg
	printf("\nbget() calling lookup:");
#endif
	struct stat s = lookup_hashtable(buf_blockno, 0);
	int val = s.val;
	if (val == -1) {	// not in the hashtable, so establish in the current cpu
		push_off();
		val = cpuid();	
		pop_off();
		insert_hashtable(buf_blockno, val, s.index);
#ifdef dbg
		printf("\nbget(): not in hashtable, assigned to cpu %d\n", val);
#endif
	}

  struct buf *b;
  acquire(&bcache[val].lock);
  // Is the block already cached?
  for(b = bcache[val].buf; b < bcache[val].buf + NBUF; b++){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
			b->ticks = ticks;
      release(&bcache[val].lock);
      acquiresleep(&b->lock);
#ifdef dbg
			printf("\nbget(): buf is in the cache\n  refcnt is %d\n  data is: %s\n", b->refcnt, b->data);
#endif
      return b;
    }
  }
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
	uint minticks = 0xffffffff;
	struct buf* bLRU = nullptr;
  for(b = bcache[val].buf; b < bcache[val].buf + NBUF; b++){
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
		release(&bcache[val].lock);
		acquiresleep(&bLRU->lock);
#ifdef dbg
		printf("\nbget(): buf not in the cache\n  data is: %s\n", bLRU->data);
#endif
		return bLRU;
	}
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;
  b = bget(dev, blockno);
	
	char buf_blockno[BLKLEN] = {0};
	snprintf(buf_blockno, sizeof(buf_blockno), "%d", b->blockno);
#ifdef dbg
	printf("\nbread() calling lookup:");
	struct stat s = lookup_hashtable(buf_blockno, 0);
	printf("\nbread(): blockno %s read by cpu %d\n", buf_blockno, s.val);
#endif

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

	char buf_blockno[BLKLEN] = {0};
	snprintf(buf_blockno, sizeof(buf_blockno), "%d", b->blockno);
#ifdef dbg
	printf("\nbrelse: calling lookup:");
#endif
	struct stat s = lookup_hashtable(buf_blockno, 1);
	int val = s.val;
	if (val == -1) {
		panic("data on buffer cache lost\n");
	}

  b->refcnt--;
  if (b->refcnt == 0) {
		b->dev = 0xffffffff;
		b->blockno = 0xffffffff;
		b->ticks = ticks;
		delete_hashtable(buf_blockno, s.index);	
  }
}

void
bpin(struct buf *b) {
	char buf_blockno[BLKLEN] = {0};
	snprintf(buf_blockno, sizeof(buf_blockno), "%d", b->blockno);
#ifdef dbg
	printf("\nbpin() calling lookup:");
#endif
	struct stat s = lookup_hashtable(buf_blockno, 0);
	int val = s.val;
	if (val == -1) {
		panic("bpin():couldn't find the val due to hash confliction\n");
	}
  acquire(&bcache[val].lock);
  b->refcnt++;
  release(&bcache[val].lock);
}

void
bunpin(struct buf *b) {
	char buf_blockno[BLKLEN] = {0};
	snprintf(buf_blockno, sizeof(buf_blockno), "%d", b->blockno);
#ifdef dbg
	printf("\nbunpin() calling lookup:");
#endif
	struct stat s = lookup_hashtable(buf_blockno, 0);
	int val = s.val;
	if (val == -1) {
		panic("bunpin():couldn't find the val due to hash confliction\n");
	}
  acquire(&bcache[val].lock);
  b->refcnt--;
  release(&bcache[val].lock);
}


