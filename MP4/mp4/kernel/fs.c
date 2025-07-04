

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

struct superblock sb;

static void readsb(int dev, struct superblock *sb)
{
    struct buf *bp;

    bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

void fsinit(int dev)
{
    readsb(dev, &sb);
    if (sb.magic != FSMAGIC)
        panic("invalid file system");
    initlog(dev, &sb);
}

static void bzero(int dev, int bno)
{
    struct buf *bp;

    bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    log_write(bp);
    brelse(bp);
}

static uint balloc(uint dev)
{
    int b, bi, m;
    struct buf *bp;

    bp = 0;
    for (b = 0; b < sb.size; b += BPB)
    {
        bp = bread(dev, BBLOCK(b, sb));
        for (bi = 0; bi < BPB && b + bi < sb.size; bi++)
        {
            m = 1 << (bi % 8);
            if ((bp->data[bi / 8] & m) == 0)
            {
                bp->data[bi / 8] |= m;
                log_write(bp);
                brelse(bp);
                bzero(dev, b + bi);
                return b + bi;
            }
        }
        brelse(bp);
    }
    panic("balloc: out of blocks");
}

static void bfree(int dev, uint b)
{
    struct buf *bp;
    int bi, m;

    bp = bread(dev, BBLOCK(b, sb));
    bi = b % BPB;
    m = 1 << (bi % 8);
    if ((bp->data[bi / 8] & m) == 0)
        panic("freeing free block");
    bp->data[bi / 8] &= ~m;
    log_write(bp);
    brelse(bp);
}

struct
{
    struct spinlock lock;
    struct inode inode[NINODE];
} icache;

void iinit()
{
    int i = 0;

    initlock(&icache.lock, "icache");
    for (i = 0; i < NINODE; i++)
    {
        initsleeplock(&icache.inode[i].lock, "inode");
    }
}

static struct inode *iget(uint dev, uint inum);

struct inode *ialloc(uint dev, short type)
{
    struct buf *bp;
    struct dinode *dip;

    for (int inum = 1; inum < sb.ninodes; inum++)
    {
        bp = bread(dev, IBLOCK(inum, sb));
        dip = (struct dinode *)bp->data + inum % IPB;

        if (dip->type == 0)
        {
            memset(dip, 0, sizeof(*dip));
            dip->type = type;
            dip->major = 0;
            dip->minor = M_ALL;
            dip->nlink = 1;
            log_write(bp);
            brelse(bp);
            return iget(dev, inum);
        }
        brelse(bp);
    }
    panic("ialloc: no inodes");
}

void iupdate(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;

    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *)bp->data + ip->inum % IPB;

    dip->type = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size = ip->size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));

    log_write(bp);
    brelse(bp);
}

static struct inode *iget(uint dev, uint inum)
{
    struct inode *ip, *empty;

    acquire(&icache.lock);

    empty = 0;
    for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
    {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
        {
            ip->ref++;
            release(&icache.lock);
            return ip;
        }
        if (empty == 0 && ip->ref == 0)
            empty = ip;
    }

    if (empty == 0)
        panic("iget: no inodes");

    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;
    release(&icache.lock);

    return ip;
}

struct inode *idup(struct inode *ip)
{
    acquire(&icache.lock);
    ip->ref++;
    release(&icache.lock);
    return ip;
}

void ilock(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;

    if (ip == 0 || ip->ref < 1)
        panic("ilock");

    acquiresleep(&ip->lock);

    if (ip->valid == 0)
    {
        bp = bread(ip->dev, IBLOCK(ip->inum, sb));
        dip = (struct dinode *)bp->data + ip->inum % IPB;

        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));

        brelse(bp);
        ip->valid = 1;

        if (ip->type == 0)
            panic("ilock: no type");
    }
}

void iunlock(struct inode *ip)
{
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("iunlock");

    releasesleep(&ip->lock);
}

void iput(struct inode *ip)
{
    acquire(&icache.lock);

    if (ip->ref == 1 && ip->valid && ip->nlink == 0)
    {

        acquiresleep(&ip->lock);

        release(&icache.lock);

        itrunc(ip);
        ip->type = 0;
        iupdate(ip);
        ip->valid = 0;

        releasesleep(&ip->lock);

        acquire(&icache.lock);
    }

    ip->ref--;
    release(&icache.lock);
}

void iunlockput(struct inode *ip)
{
    iunlock(ip);
    iput(ip);
}

uint bmap(struct inode *ip, uint bn)
{
    uint addr, *a;
    struct buf *bp;

    if (bn < NDIRECT)
    {
        if ((addr = ip->addrs[bn]) == 0)
        {
            addr = balloc(ip->dev);
            if (addr == 0)
                panic("bmap: balloc failed");
            ip->addrs[bn] = addr;
        }
        return addr;
    }
    bn -= NDIRECT;
    if (bn < NINDIRECT)
    {
        if ((addr = ip->addrs[NDIRECT]) == 0)
        {
            addr = balloc(ip->dev);
            if (addr == 0)
                panic("bmap: balloc failed for indirect block");
            ip->addrs[NDIRECT] = addr;
        }

        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;

        uint t_addr = a[bn];

        if (t_addr == 0)
        {
            t_addr = balloc(ip->dev);
            if (t_addr == 0)
                panic("bmap: balloc failed for data block via indirect");
            a[bn] = t_addr;
            log_write(bp);
        }
        brelse(bp);

        return t_addr;
    }

    printf("bmap: ERROR! file_bn %d is out of range for inode %d\n",
           bn + NDIRECT, ip->inum);
    panic("bmap: out of range");
}

void itrunc(struct inode *ip)
{
    int i, j;
    struct buf *bp;
    uint *a;

    for (i = 0; i < NDIRECT; i++)
    {
        if (ip->addrs[i])
        {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT])
    {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        a = (uint *)bp->data;
        for (j = 0; j < NINDIRECT; j++)
        {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}

void stati(struct inode *ip, struct stat *st)
{
    st->dev = ip->dev;
    st->ino = ip->inum;
    st->type = ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
    st->mode = ip->minor;
}

int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (!(ip->type == T_FILE || ip->type == T_SYMLINK || ip->type == T_DIR))
        return -1;

    if (off > ip->size || off + n < off)
        return 0;
    if (off + n > ip->size)
        n = ip->size - off;

    for (tot = 0; tot < n; tot += m, off += m, dst += m)
    {
        bp = bread(ip->dev, bmap(ip, off / BSIZE));
        m = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1)
        {
            brelse(bp);
            break;
        }
        brelse(bp);
    }
    return tot;
}

int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (tot = 0; tot < n; tot += m, off += m, src += m)
    {
        uint bn = off / BSIZE;
        uint disk_lbn = bmap(ip, bn);
        bp = bread(ip->dev, disk_lbn);
        m = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1)
        {
            brelse(bp);
            break;
        }
        log_write(bp);
        brelse(bp);
    }

    if (n > 0)
    {
        if (off > ip->size)
            ip->size = off;

        iupdate(ip);
    }

    return n;
}

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *dirlookup(struct inode *dp, char *name, uint *poff)
{
    uint off, inum;
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    for (off = 0; off < dp->size; off += sizeof(de))
    {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        if (namecmp(name, de.name) == 0)
        {

            if (poff)
                *poff = off;
            inum = de.inum;
            return iget(dp->dev, inum);
        }
    }

    return 0;
}

int dirlink(struct inode *dp, char *name, uint inum)
{
    int off;
    struct dirent de;
    struct inode *ip;

    if ((ip = dirlookup(dp, name, 0)) != 0)
    {
        iput(ip);
        return -1;
    }

    for (off = 0; off < dp->size; off += sizeof(de))
    {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break;
    }

    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("dirlink");

    return 0;
}

static char *skipelem(char *path, char *name)
{
    char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else
    {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

struct inode *namex(char *path, int nameiparent, char *name)
{
    struct inode *ip, *next;

    if (*path == '/')
        ip = iget(ROOTDEV, ROOTINO);
    else
        ip = idup(myproc()->cwd);

    while ((path = skipelem(path, name)) != 0)
    {
        ilock(ip);
        if (ip->type == T_DIR && !(ip->minor & M_READ))
        {

            iunlockput(ip);

            return 0;
        }
        if (ip->type != T_DIR)
        {
            iunlockput(ip);
            return 0;
        }
        if (nameiparent && *path == '\0')
        {

            iunlock(ip);
            return ip;
        }
        if ((next = dirlookup(ip, name, 0)) == 0)
        {
            iunlockput(ip);
            return 0;
        }
        iunlockput(ip);
        ip = next;
        ilock(ip);

        if (ip->type == T_SYMLINK)
        {

            char t[MAXPATH], newpath[MAXPATH];

            int n = readi(ip, 0, (uint64)t, 0, MAXPATH - 1);

            iunlockput(ip);

            if (n < 0)

                return 0;

            t[n] = '\0';

            int tlen = strlen(t);

            int plen = strlen(path);

            if (tlen + 1 + plen >= MAXPATH)

                return 0;

            memmove(newpath, t, tlen);

            newpath[tlen] = '/';

            memmove(newpath + tlen + 1, path, plen);

            newpath[tlen + 1 + plen] = '\0';

            iput(ip);

            return namex(newpath, nameiparent, name);
        }

        iunlock(ip);
    }
    if (nameiparent)
    {
        iput(ip);
        return 0;
    }
    return ip;
}

struct inode *namei(char *path)
{
    char name[DIRSIZ];
    struct inode *ip = namex(path, 0, name);

    while (ip && ip->type == T_SYMLINK)
    {

        char buf[MAXPATH];

        int n = readi(ip, 0, (uint64)buf, 0, MAXPATH - 1);

        if (n < 0)
        {

            iput(ip);

            return 0;
        }

        buf[n] = '\0';

        iput(ip);

        ip = namei(buf);
    }
    return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name)
{
    return namex(path, 1, name);
}
