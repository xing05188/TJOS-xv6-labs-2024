// Buffer cache.
//
// The buffer cache is now organized as a hash table with separate chaining,
// where each bucket has its own lock to reduce contention.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13  // Number of buckets - prime number to reduce collisions

struct bucket {
  struct spinlock lock;
  struct buf head;  // Head of the linked list for this bucket
};

struct {
  struct buf buf[NBUF];
  struct bucket buckets[NBUCKET];  // Hash table of buckets
} bcache;

void
binit(void)
{
  struct buf *b;
  char lockname[16];

  // Initialize each bucket's lock and linked list
  for (int i = 0; i < NBUCKET; i++) {
    snprintf(lockname, sizeof(lockname), "bcache.bucket%d", i);
    initlock(&bcache.buckets[i].lock, lockname);
    
    // Initialize the bucket's linked list
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
  }

  // Initialize all buffers and distribute them to bucket 0 initially
  // They will migrate to other buckets as needed during use
  for (b = bcache.buf; b < bcache.buf+NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    
    // Add to bucket 0's list
    b->next = bcache.buckets[0].head.next;
    b->prev = &bcache.buckets[0].head;
    bcache.buckets[0].head.next->prev = b;
    bcache.buckets[0].head.next = b;
  }
}

// Helper function to remove a buffer from its bucket
static void
remove_from_bucket(struct buf *b) {
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

// Helper function to add a buffer to a bucket's head
static void
add_to_bucket(struct bucket *bucket, struct buf *b) {
  b->next = bucket->head.next;
  b->prev = &bucket->head;
  bucket->head.next->prev = b;
  bucket->head.next = b;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket_idx = blockno % NBUCKET;
  struct bucket *bucket = &bcache.buckets[bucket_idx];

  acquire(&bucket->lock);

  // Is the block already cached in this bucket?
  for (b = bucket->head.next; b != &bucket->head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached in this bucket.
  // Try to find a reusable buffer in this bucket first
  for (b = bucket->head.prev; b != &bucket->head; b = b->prev) {
    if (b->refcnt == 0) {
      // Found a reusable buffer in the same bucket
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No available buffers in this bucket, search other buckets
  for (int i = (bucket_idx + 1) % NBUCKET; i != bucket_idx; i = (i + 1) % NBUCKET) {
    struct bucket *other_bucket = &bcache.buckets[i];
    
    // To avoid deadlock, we must acquire locks in a consistent order
    // Here we use bucket index order (lower index first)
    if (i < bucket_idx) {
      if (!holding(&other_bucket->lock)) {
        acquire(&other_bucket->lock);
      } else {
        // Skip if we already hold this lock (shouldn't happen in this design)
        continue;
      }
    } else {
      // For higher index buckets, try to acquire if we don't already hold it
      if (!holding(&other_bucket->lock)) {
        acquire(&other_bucket->lock);
      }
    }

    // Search for a reusable buffer in this other bucket
    for (b = other_bucket->head.prev; b != &other_bucket->head; b = b->prev) {
      if (b->refcnt == 0) {
        // Found a buffer to reuse - move it to our bucket
        remove_from_bucket(b);
        add_to_bucket(bucket, b);
        
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        
        release(&other_bucket->lock);
        release(&bucket->lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    
    release(&other_bucket->lock);
  }

  release(&bucket->lock);
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

// Write b's contents to disk. Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list in its bucket.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket_idx = b->blockno % NBUCKET;
  struct bucket *bucket = &bcache.buckets[bucket_idx];

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // Move to the head of the bucket's LRU list
    remove_from_bucket(b);
    add_to_bucket(bucket, b);
  }
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  uint bucket_idx = b->blockno % NBUCKET;
  struct bucket *bucket = &bcache.buckets[bucket_idx];
  
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  uint bucket_idx = b->blockno % NBUCKET;
  struct bucket *bucket = &bcache.buckets[bucket_idx];
  
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}