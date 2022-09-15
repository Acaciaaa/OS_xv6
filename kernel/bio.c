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

#define N 13

struct
{
    struct spinlock lock;
    struct buf buf[NBUF];

    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
} bcache;

struct
{
    struct spinlock lock;
    struct buf* head;
}bucket[N];

void
binit(void)
{
    initlock(&bcache.lock, "bcache");
    for(struct buf* i = bcache.buf; i < bcache.buf+NBUF; ++i)
        initsleeplock(&i->lock, "buffer");
    for(int i = 0; i < N; ++i)
        initlock(&bucket[i].lock, "bucket");
    int j = 0;
    for(struct buf* i = bcache.buf; i < bcache.buf+NBUF; ++i)
    {
        i->blockno = j;
        i->next = bucket[j].head;
        bucket[j].head = i;
        j = (j+1)%N;
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
    struct buf *b = 0;

    int no = blockno % N;
    uint time = -1;
    acquire(&bucket[no].lock);

    // Is the block already cached?
    for(struct buf* i = bucket[no].head; i != 0; i = i->next)
    {
        if(i->dev == dev && i->blockno == blockno)
        {
            i->refcnt++;
            release(&bucket[no].lock);
            acquiresleep(&i->lock);
            return i;
        }
        if(i->refcnt == 0 && i->time < time)
        {
            time = i->time;
            b = i;
        }
    }

    // Not cached.
    if(b)
    {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bucket[no].lock);
        acquiresleep(&b->lock);
        return b;
    }

    acquire(&bcache.lock);
    globalfind:
    for(struct buf* i = bcache.buf; i < bcache.buf+NBUF; ++i)
        if(i->refcnt == 0 && i->time < time)
        {
            time = i->time;
            b = i;
        }

    if(b)
    {
        int no2 = b->blockno % N;
        acquire(&bucket[no2].lock);
        if(b->refcnt != 0)
        {
            release(&bucket[no2].lock);
            time = -1;
            goto globalfind;
        }

        if(bucket[no2].head == b)
            bucket[no2].head = b->next;
        else{
            struct buf* pre;
            for(struct buf* p = bucket[no2].head; p != b; p = p->next)
                pre = p;
            pre->next = b->next;
        }
        release(&bucket[no2].lock);

        b->next = bucket[no].head;
        bucket[no].head = b;
        release(&bcache.lock);

        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bucket[no].lock);
        acquiresleep(&b->lock);
        return b;
    }

    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
    struct buf *b;

    b = bget(dev, blockno);
    if(!b->valid)
    {
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

    int no = b->blockno % N;
    acquire(&bucket[no].lock);
    b->refcnt--;
    if (b->refcnt == 0)
    {
        // no one is waiting for it.
        b->time = ticks;
    }
    release(&bucket[no].lock);
}

void
bpin(struct buf *b)
{
    int no = b->blockno % N;
    acquire(&bucket[no].lock);
    b->refcnt++;
    release(&bucket[no].lock);
}

void
bunpin(struct buf *b)
{
    int no = b->blockno % N;
    acquire(&bucket[no].lock);
    b->refcnt--;
    release(&bucket[no].lock);
}


