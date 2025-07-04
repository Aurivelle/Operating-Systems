

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "buf.h"

#define PATH_MAX 128

#define DIRSIZ 14
struct inode *namex(char *path, int nameiparent, char *name);

char *skipelem(char *path, char *name)
{
    while (*path == '/')
        path++;
    if (*path == 0)
    {
        name[0] = 0;
        return 0;
    }
    char *s = path;
    while (*path != '/' && *path != 0)
        path++;
    int len = path - s;
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
int safestrcat(char *dst, const char *src, int max)
{
    int i = 0;
    while (dst[i] && i < max)
        i++;
    int j = 0;
    while (src[j] && i + j + 1 < max)
    {
        dst[i + j] = src[j];
        j++;
    }
    if (i + j < max)
        dst[i + j] = '\0';
    else if (max > 0)
        dst[max - 1] = '\0';
    return i + j;
}

char *kstrchr(const char *s, char c)
{
    while (*s)
    {
        if (*s == c)
            return (char *)s;
        s++;
    }
    return 0;
}
static int chmod_walk(char *path, int add, int bits, int recursive);

static int argfd(int n, int *pfd, struct file **pf)
{
    int fd;
    struct file *f;

    if (argint(n, &fd) < 0)
        return -1;
    if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
        return -1;
    if (pfd)
        *pfd = fd;
    if (pf)
        *pf = f;
    return 0;
}

static int fdalloc(struct file *f)
{
    int fd;
    struct proc *p = myproc();

    for (fd = 0; fd < NOFILE; fd++)
    {
        if (p->ofile[fd] == 0)
        {
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

uint64 sys_dup(void)
{
    struct file *f;
    int fd;

    if (argfd(0, 0, &f) < 0)
        return -1;
    if ((fd = fdalloc(f)) < 0)
        return -1;
    filedup(f);
    return fd;
}

uint64 sys_read(void)
{
    struct file *f;
    int n;
    uint64 p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
        return -1;
    return fileread(f, p, n);
}

uint64 sys_write(void)
{
    struct file *f;
    int n;
    uint64 p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
        return -1;

    return filewrite(f, p, n);
}

uint64 sys_close(void)
{
    int fd;
    struct file *f;

    if (argfd(0, &fd, &f) < 0)
        return -1;
    myproc()->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

uint64 sys_fstat(void)
{
    struct file *f;
    uint64 st;

    if (argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
        return -1;
    return filestat(f, st);
}

uint64 sys_link(void)
{
    char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
    struct inode *dp, *ip;

    if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
        return -1;

    begin_op();
    if ((ip = namei(old)) == 0)
    {
        end_op();
        return -1;
    }

    ilock(ip);
    if (ip->type == T_DIR)
    {
        iunlockput(ip);
        end_op();
        return -1;
    }

    ip->nlink++;
    iupdate(ip);
    iunlock(ip);

    if ((dp = nameiparent(new, name)) == 0)
        goto bad;
    ilock(dp);
    if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
    {
        iunlockput(dp);
        goto bad;
    }
    iunlockput(dp);
    iput(ip);

    end_op();

    return 0;

bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
}

static int isdirempty(struct inode *dp)
{
    int off;
    struct dirent de;

    for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de))
    {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("isdirempty: readi");
        if (de.inum != 0)
            return 0;
    }
    return 1;
}

uint64 sys_unlink(void)
{
    struct inode *ip, *dp;
    struct dirent de;
    char name[DIRSIZ], path[MAXPATH];
    uint off;

    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    begin_op();
    if ((dp = nameiparent(path, name)) == 0)
    {
        end_op();
        return -1;
    }

    ilock(dp);

    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
        goto bad;

    if ((ip = dirlookup(dp, name, &off)) == 0)
        goto bad;
    ilock(ip);

    if (ip->nlink < 1)
        panic("unlink: nlink < 1");
    if (ip->type == T_DIR && !isdirempty(ip))
    {
        iunlockput(ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("unlink: writei");
    if (ip->type == T_DIR)
    {
        dp->nlink--;
        iupdate(dp);
    }
    iunlockput(dp);

    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);

    end_op();

    return 0;

bad:
    iunlockput(dp);
    end_op();
    return -1;
}

static struct inode *create(char *path, short type, short major, short minor)
{
    struct inode *ip, *dp;
    char name[DIRSIZ];

    if ((dp = nameiparent(path, name)) == 0)
        return 0;

    ilock(dp);

    if ((ip = dirlookup(dp, name, 0)) != 0)
    {
        iunlockput(dp);
        ilock(ip);
        if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
            return ip;
        iunlockput(ip);
        return 0;
    }

    if ((ip = ialloc(dp->dev, type)) == 0)
        panic("create: ialloc");

    ilock(ip);
    ip->nlink = 1;

    ip->major = major;

    ip->minor = minor;
    iupdate(ip);

    if (type == T_DIR)
    {
        dp->nlink++;
        iupdate(dp);

        if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
            panic("create dots");
    }

    if (dirlink(dp, name, ip->inum) < 0)
        panic("create: dirlink");

    iunlockput(dp);

    return ip;
}
#define SYMLOOP_MAX 10
#define PATH_SEP '/'

static int readlink_target(struct inode *ip, char *buf, int bufsize)
{
    if (ip->size >= bufsize)
        return -1;
    int n = readi(ip, 0, (uint64)buf, 0, ip->size);
    buf[n] = '\0';
    return 0;
}

static int path_join(char *new, const char *base, const char *rest, int max)
{
    if (strlen(base) + 1 + strlen(rest) + 1 > max)
        return -1;
    safestrcpy(new, base, max);
    if (new[0] == '\0')
        safestrcpy(new, rest, max);
    else
    {

        if (new[strlen(new) - 1] != PATH_SEP)
            safestrcat(new, "/", max);
        safestrcat(new, rest, max);
    }
    return 0;
}

char *kstrrchr(const char *s, char c)
{
    char *last = 0;
    while (*s)
    {
        if (*s == c)
            last = (char *)s;
        s++;
    }
    return last;
}
uint64 sys_open(void)
{
    char path[MAXPATH];
    int omode;

    // 取得使用者參數 ----------------------------------------------------------
    if (argstr(0, path, MAXPATH) < 0 || argint(1, &omode) < 0)
        return -1;

    // 迴圈展開 symlink；最多追 10 次避免循環
    for (int depth = 0; depth < 10; depth++)
    {
        struct inode *ip;
        struct file *f;
        int fd;

        begin_op();

        // ─────────────────────────── 1) 處理 O_CREATE
        // ──────────────────────────
        if (omode & O_CREATE)
        {
            ip = create(path, T_FILE, 0, M_ALL); // 新檔案 rw 預設權限
            if (ip == 0)
            {
                end_op();
                return -1;
            }
        }

        // ──────────────────────── 2) 非 create 的開檔
        // ──────────────────────────
        else
        {
            // ---------- (2a) O_NOACCESS：只拿 inode, 不跟隨 leaf ----------
            if (omode & O_NOACCESS)
            {
                char parent[MAXPATH], leaf[DIRSIZ];

                // 切割父目錄與 leaf
                char *slash = kstrrchr(path, '/');
                if (slash)
                {
                    int plen = slash - path;
                    if (plen >= MAXPATH)
                        plen = MAXPATH - 1;
                    memmove(parent, path, plen);
                    parent[plen] = '\0';
                    safestrcpy(leaf, slash + 1, sizeof(leaf));
                }
                else
                {
                    safestrcpy(parent, ".", sizeof(parent));
                    safestrcpy(leaf, path, sizeof(leaf));
                }

                // 取得 parent inode（不跟隨 leaf）
                ip = namex(path, 1, leaf);
                if (!ip)
                {
                    end_op();
                    return -1;
                }
                ilock(ip);

                // 若 parent 本身是 symlink，追一次
                while (ip->type == T_SYMLINK)
                {
                    char buf[MAXPATH];
                    int n = readi(ip, 0, (uint64)buf, 0, MAXPATH - 1);
                    iunlockput(ip);
                    end_op();
                    if (n < 0)
                        return -1;
                    buf[n] = 0;
                    safestrcpy(path, buf, MAXPATH); // 轉向；重新 loop
                    goto loop_continue;
                }

                if (ip->type != T_DIR)
                {
                    iunlockput(ip);
                    end_op();
                    return -1;
                }

                // 在 parent 內查 leaf (不跟隨 leaf symlink)
                struct inode *dp = ip;
                ip = dirlookup(dp, leaf, 0);
                iunlockput(dp);
                if (!ip)
                {
                    end_op();
                    return -1;
                }
                ilock(ip);
            }
            // ---------- (2b) Normal open：會跟隨 leaf symlink ----------
            else
            {
                ip = namei(path); // 預設會解析 symlink
                if (!ip)
                {
                    end_op();
                    return -1;
                }
                ilock(ip);
            }

            // ---------- (2c) 若開到 symlink，讀其 target 再重來 ----------
            if (!(omode & O_NOACCESS) && ip->type == T_SYMLINK)
            {
                char target[MAXPATH];
                int n = readi(ip, 0, (uint64)target, 0, MAXPATH - 1);
                iunlockput(ip);
                end_op();
                if (n < 0)
                    return -1;
                target[n] = 0;
                safestrcpy(path, target, MAXPATH);
                continue; // loop 追下一層
            }

            // ---------- (2d) 權限檢查 (若非 O_NOACCESS) ----------
            if (!(omode & O_NOACCESS))
            {
                int wantR = !(omode & O_WRONLY);
                int wantW = (omode & O_WRONLY) || (omode & O_RDWR);
                int ok = !(wantR && !(ip->minor & M_READ)) &&
                         !(wantW && !(ip->minor & M_WRITE)) &&
                         !(ip->type == T_DIR && wantW);
                if (!ok)
                {
                    iunlockput(ip);
                    end_op();
                    return -1;
                }
            }
        }

        // ───────────────────── 3) 特殊裝置號檢查
        // ───────────────────────────────
        if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
        {
            iunlockput(ip);
            end_op();
            return -1;
        }

        // ───────────────────── 4) 配置 file 與 fd
        // ──────────────────────────────
        if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
        {
            if (f)
                fileclose(f);
            iunlockput(ip);
            end_op();
            return -1;
        }

        // ───────────────────── 5) 初始化 file 結構
        // ─────────────────────────────
        f->type = (ip->type == T_DEVICE ? FD_DEVICE : FD_INODE);
        f->major = ip->major;
        f->ip = ip;
        f->off = 0;

        if ((omode & O_NOACCESS) && ip->type != T_SYMLINK)
        {
            f->readable = f->writable = 0;
        }
        else
        {
            f->readable = !(omode & O_WRONLY);
            f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
        }

        if ((omode & O_TRUNC) && ip->type == T_FILE)
            itrunc(ip);

        iunlock(ip);
        end_op();
        return fd;

    loop_continue:; // label target；什麼都不做，for 迴圈會繼續
    }

    // 超過 symlink 深度限制
    return -1;
}

uint64 sys_mkdir(void)
{
    char path[MAXPATH];
    struct inode *ip;

    begin_op();
    if (argstr(0, path, MAXPATH) < 0 ||
        (ip = create(path, T_DIR, 0, M_ALL)) == 0)
    {
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
    return 0;
}

uint64 sys_mknod(void)
{
    struct inode *ip;
    char path[MAXPATH];
    int major, minor;

    begin_op();
    if ((argstr(0, path, MAXPATH)) < 0 || argint(1, &major) < 0 ||
        argint(2, &minor) < 0 ||
        (ip = create(path, T_DEVICE, major, M_ALL)) == 0)
    {
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
    return 0;
}

uint64 sys_chdir(void)
{
    char path[MAXPATH];
    struct inode *ip;
    struct proc *p = myproc();

    begin_op();
    if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0)
    {
        end_op();
        return -1;
    }
    ilock(ip);
    if (ip->type != T_DIR)
    {
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlock(ip);
    iput(p->cwd);
    end_op();
    p->cwd = ip;
    return 0;
}

uint64 sys_exec(void)
{
    char path[MAXPATH], *argv[MAXARG];
    int i;
    uint64 uargv, uarg;

    if (argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0)
    {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0;; i++)
    {
        if (i >= NELEM(argv))
        {
            goto bad;
        }
        if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0)
        {
            goto bad;
        }
        if (uarg == 0)
        {
            argv[i] = 0;
            break;
        }
        argv[i] = kalloc();
        if (argv[i] == 0)
            goto bad;
        if (fetchstr(uarg, argv[i], PGSIZE) < 0)
            goto bad;
    }

    int ret = exec(path, argv);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);

    return ret;

bad:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    return -1;
}

uint64 sys_pipe(void)
{
    uint64 fdarray;
    struct file *rf, *wf;
    int fd0, fd1;
    struct proc *p = myproc();

    if (argaddr(0, &fdarray) < 0)
        return -1;
    if (pipealloc(&rf, &wf) < 0)
        return -1;
    fd0 = -1;
    if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0)
    {
        if (fd0 >= 0)
            p->ofile[fd0] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1,
                sizeof(fd1)) < 0)
    {
        p->ofile[fd0] = 0;
        p->ofile[fd1] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    return 0;
}

uint64 sys_symlink(void)

{

    char target[MAXPATH], path[MAXPATH];

    struct inode *ip;

    if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)

    {

        return -1;
    }

    begin_op();

    ip = create(path, T_SYMLINK, 0, M_ALL);

    if (ip == 0)
    {

        end_op();

        return -1;
    }

    if (writei(ip, 0, (uint64)target, 0, strlen(target)) != strlen(target))

    {

        iunlockput(ip);

        end_op();

        return -1;
    }

    iunlockput(ip);

    end_op();

    return 0;
}

uint64 sys_raw_read(void)
{
    int pbn;
    uint64 user_buf_addr;
    struct buf *b;

    if (argint(0, &pbn) < 0 || argaddr(1, &user_buf_addr) < 0)
    {
        return -1;
    }

    if (pbn < 0 || pbn >= FSSIZE)
    {
        return -1;
    }

    b = bget(ROOTDEV, pbn);
    if (b == 0)
    {
        return -1;
    }

    virtio_disk_rw(b, 0);

    struct proc *p = myproc();
    if (copyout(p->pagetable, user_buf_addr, (char *)b->data, BSIZE) < 0)
    {
        brelse(b);
        return -1;
    }

    brelse(b);
    return 0;
}

uint64 sys_get_disk_lbn(void)
{
    struct file *f;
    int fd;
    int file_lbn;
    uint disk_lbn;

    if (argfd(0, &fd, &f) < 0 || argint(1, &file_lbn) < 0)
    {
        return -1;
    }

    if (!f->readable)
    {
        return -1;
    }

    struct inode *ip = f->ip;

    ilock(ip);

    disk_lbn = bmap(ip, file_lbn);

    iunlock(ip);

    return (uint64)disk_lbn;
}

uint64 sys_raw_write(void)
{
    int pbn;
    uint64 user_buf_addr;
    struct buf *b;

    if (argint(0, &pbn) < 0 || argaddr(1, &user_buf_addr) < 0)
    {
        return -1;
    }

    if (pbn < 0 || pbn >= FSSIZE)
    {
        return -1;
    }

    b = bget(ROOTDEV, pbn);
    if (b == 0)
    {
        printf("sys_raw_write: bget failed for PBN %d\n", pbn);
        return -1;
    }
    struct proc *p = myproc();
    if (copyin(p->pagetable, (char *)b->data, user_buf_addr, BSIZE) < 0)
    {
        brelse(b);
        return -1;
    }

    b->valid = 1;
    virtio_disk_rw(b, 1);
    brelse(b);

    return 0;
}

static int chmod_walk(char *path, int add, int bits, int recursive)
{
    struct inode *ip;
    if ((ip = namei(path)) == 0)
        return -1;
    ilock(ip);

    int isdir = (ip->type == T_DIR);
    int preorder = 1;

    if (recursive && isdir)
    {
        if (!add && (bits & M_READ))
            preorder = 0;
        if (add && (bits & M_READ) && !(ip->minor & M_READ))
            preorder = 1;
    }

    if (preorder)
    {
        ip->minor = add ? (ip->minor | bits) : (ip->minor & ~bits);
        iupdate(ip);
    }

    if (recursive && isdir && (ip->minor & M_READ))
    {
        struct dirent de;
        char child[MAXPATH];
        for (uint off = 0; off < ip->size; off += sizeof(de))
        {
            if (readi(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
                break;
            if (de.inum == 0)
                continue;
            if (namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0)
                continue;

            int len = strlen(path);
            if (len + 1 + DIRSIZ + 1 >= MAXPATH)
            {
                iunlock(ip);
                return -1;
            }
            memmove(child, path, len);
            child[len] = '/';
            memmove(child + len + 1, de.name, DIRSIZ);
            child[len + 1 + DIRSIZ] = 0;

            iunlock(ip);
            int r = chmod_walk(child, add, bits, 1);
            ilock(ip);
            if (r < 0)
            {
                iunlock(ip);
                return -1;
            }
        }
    }

    if (!preorder)
    {
        ip->minor = add ? (ip->minor | bits) : (ip->minor & ~bits);
        iupdate(ip);
    }
    iunlock(ip);
    return 0;
}

uint64 sys_chmod(void)
{

    char path[128];

    int mode_op;

    if (argint(0, &mode_op) < 0 || argstr(1, path, sizeof(path)) < 0)

        return -1;

    begin_op();

    struct inode *ip = namei(path);

    if (ip == 0)

    {

        end_op();

        return -1;
    }

    ilock(ip);

    ip->minor = mode_op;

    iupdate(ip);

    iunlock(ip);

    iput(ip);

    end_op();

    return 0;
}
