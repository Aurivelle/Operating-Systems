

#include "types.h"
#include "riscv.h"
#include <stddef.h>
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "debug.h"

struct devsw devsw[NDEV];
struct spinlock global_file_lock;

struct kmem_cache *file_cache;

void fileprint_metadata(void *f)
{
  struct file *file = (struct file *)f;
  debug("tp: %d, ref: %d, readable: %d, writable: %d, pipe: %p, ip: %p, off: %d, major: %d",
        file->type, file->ref, file->readable, file->writable, file->pipe, file->ip, file->off, file->major);
}

void fileinit(void)
{
  debug("[FILE] fileinit\n");

  initlock(&global_file_lock, "global_file_lock");

  file_cache = kmem_cache_create("file", sizeof(struct file));
}

struct file *filealloc(void)
{
  acquire(&global_file_lock);
  debug("[FILE] filealloc\n");
  release(&global_file_lock);
  struct file *f = (struct file *)kmem_cache_alloc(file_cache);
  if (f == NULL)
    return NULL;
  acquire(&global_file_lock);
  memset(f, 0, sizeof(struct file));

  f->ref = 1;
  release(&global_file_lock);
  return f;
}

struct file *filedup(struct file *f)
{
  acquire(&global_file_lock);
  if (f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&global_file_lock);
  return f;
}

void fileclose(struct file *f)
{
  acquire(&global_file_lock);
  if (f->ref < 1)
    panic("fileclose");
  if (--f->ref > 0)
  {
    release(&global_file_lock);
    return;
  }
  debug("[FILE] fileclose\n");
  release(&global_file_lock);

  if (f->type == FD_PIPE)
  {
    pipeclose(f->pipe, f->writable);
  }
  else if (f->type == FD_INODE || f->type == FD_DEVICE)
  {
    begin_op();
    iput(f->ip);
    end_op();
  }
  kmem_cache_free(file_cache, f);
}

int filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if (f->type == FD_INODE || f->type == FD_DEVICE)
  {
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

int fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if (f->readable == 0)
    return -1;

  if (f->type == FD_PIPE)
  {
    r = piperead(f->pipe, addr, n);
  }
  else if (f->type == FD_DEVICE)
  {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  }
  else if (f->type == FD_INODE)
  {
    ilock(f->ip);
    if ((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  }
  else
  {
    panic("fileread");
  }

  return r;
}

int filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if (f->writable == 0)
    return -1;

  if (f->type == FD_PIPE)
  {
    ret = pipewrite(f->pipe, addr, n);
  }
  else if (f->type == FD_DEVICE)
  {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  }
  else if (f->type == FD_INODE)
  {

    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
    int i = 0;
    while (i < n)
    {
      int n1 = n - i;
      if (n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if (r != n1)
        break;
      i += r;
    }
    ret = (i == n ? n : -1);
  }
  else
  {
    panic("filewrite");
  }

  return ret;
}
