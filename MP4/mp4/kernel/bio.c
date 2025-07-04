

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern int force_read_error_pbn;
extern int force_disk_fail_id;

struct
{
    struct spinlock lock;
    struct buf buf[NBUF];

    struct buf head;
} bcache;

void binit(void)
{
    struct buf *b;

    initlock(&bcache.lock, "bcache");

    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for (b = bcache.buf; b < bcache.buf + NBUF; b++)
    {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        initsleeplock(&b->lock, "buffer");
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

struct buf *bget(uint dev, uint blockno)
{
    struct buf *b;

    acquire(&bcache.lock);

    for (b = bcache.head.next; b != &bcache.head; b = b->next)
    {
        if (b->dev == dev && b->blockno == blockno)
        {
            b->refcnt++;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    for (b = bcache.head.prev; b != &bcache.head; b = b->prev)
    {
        if (b->refcnt == 0)
        {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    panic("bget: no buffers");
}

struct buf *bread(uint dev, uint blockno)
{
    struct buf *b;
    int fail_disk = force_disk_fail_id;
    int is_pbn0_block_fail =
        (force_read_error_pbn == blockno && force_read_error_pbn != -1);

    b = bget(dev, blockno);

    int need_fallback = (fail_disk == 0) || is_pbn0_block_fail;

    if (!b->valid || need_fallback)
    {
        int original_blockno = b->blockno;
        if (need_fallback)
        {

            b->blockno = blockno + DISK1_START_BLOCK;
            virtio_disk_rw(b, 0);
            b->blockno = original_blockno;
        }
        else
        {

            b->blockno = blockno;
            virtio_disk_rw(b, 0);
        }
        b->valid = 1;
    }
    return b;
}

void bwrite(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("bwrite");

    int pbn0 = b->blockno;
    int pbn1 = b->blockno + DISK1_START_BLOCK;

    int sim_disk_fail = force_disk_fail_id;
    int sim_pbn0_block_fail =
        (force_read_error_pbn == pbn0 && force_read_error_pbn != -1);

    printf(
        "BW_DIAG: PBN0=%d, PBN1=%d, sim_disk_fail=%d, sim_pbn0_block_fail=%d\n",
        pbn0, pbn1, sim_disk_fail, sim_pbn0_block_fail);

    int original_blockno = b->blockno;

    if (sim_disk_fail == 0)
    {
        printf(
            "BW_ACTION: SKIP_PBN0 (PBN %d) due to simulated Disk 0 failure.\n",
            pbn0);
    }
    else if (sim_pbn0_block_fail)
    {
        printf("BW_ACTION: SKIP_PBN0 (PBN %d) due to simulated PBN0 block "
               "failure.\n",
               pbn0);
    }
    else
    {
        printf("BW_ACTION: ATTEMPT_PBN0 (PBN %d).\n", pbn0);
        b->blockno = pbn0;
        virtio_disk_rw(b, 1);
    }

    if (sim_disk_fail == 1)
    {
        printf(
            "BW_ACTION: SKIP_PBN1 (PBN %d) due to simulated Disk 1 failure.\n",
            pbn1);
    }
    else
    {
        printf("BW_ACTION: ATTEMPT_PBN1 (PBN %d).\n", pbn1);
        b->blockno = pbn1;
        virtio_disk_rw(b, 1);
    }

    b->blockno = original_blockno;
}

void brelse(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    acquire(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0)
    {

        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock);
}

void bpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

void bunpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}
