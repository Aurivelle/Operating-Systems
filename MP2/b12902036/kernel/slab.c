#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "debug.h"

#define MP2_MIN_PARTIAL 2

static void file_slab_printer(void *obj)
{
    struct file *f = (struct file *)obj;
    fileprint_metadata(f);
}

void print_kmem_cache(struct kmem_cache *cache, void (*print_fn)(void *))
{
    acquire(&cache->lock);

    printf("[SLAB] kmem_cache { name: %s, object_size: %d, at: %p, in_cache_obj: %d }\n",
           cache->name, cache->object_size, cache, cache->in_cache_obj);

    if (cache->in_cache_obj > 0 && cache->cache_slab)
    {
        struct slab *s = cache->cache_slab;
        printf("[SLAB]    [ cache slabs ]\n");

        int free_count = 0;
        struct run *it = s->freelist;
        while (it)
        {
            free_count++;
            it = it->next;
        }

        int inuse = cache->in_cache_obj - free_count;

        printf("[SLAB]        [ slab %p ] { freelist: %p, inuse: %d, nxt: 0x0000000000000000 }\n",
               s, s->freelist, inuse);

        char *start = (char *)((uint64)s + sizeof(struct slab));
        for (int i = 0; i < cache->in_cache_obj; i++)
        {
            void *obj = (void *)(start + i * cache->object_size);
            void *as_ptr = *(void **)obj;
            printf("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: { ", i, obj, as_ptr);
            print_fn(obj);
            printf(" } }\n");
        }
    }

    if (!list_empty(&cache->partial))
    {
        printf("[SLAB]    [ partial slabs ]\n");
        struct list_head *p;
        list_for_each(p, &cache->partial)
        {
            struct slab *s = list_entry(p, struct slab, list);

            int free_count = 0;
            struct run *it = s->freelist;
            while (it)
            {
                free_count++;
                it = it->next;
            }

            uint64 start_addr = (uint64)s + sizeof(struct slab);
            int max_objs = (PGSIZE - sizeof(struct slab)) / cache->object_size;
            int inuse = max_objs - free_count;

            printf("[SLAB]        [ slab %p ] { freelist: %p, inuse: %d, nxt: %p }\n",
                   s, s->freelist, inuse, s->list.next);

            char *start = (char *)start_addr;
            for (int i = 0; i < max_objs; i++)
            {
                void *obj = (void *)(start + i * cache->object_size);
                void *as_ptr = *(void **)obj;
                printf("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: { ", i, obj, as_ptr);
                print_fn(obj);
                printf(" } }\n");
            }
        }
    }

    printf("[SLAB] print_kmem_cache end\n");
    release(&cache->lock);
}

struct kmem_cache *kmem_cache_create(const char *name, uint object_size)
{

    struct kmem_cache *cache = (struct kmem_cache *)kalloc();
    if (!cache)
        return 0;
    memset(cache, 0, PGSIZE);

    safestrcpy(cache->name, name, sizeof(cache->name));
    cache->object_size = object_size;
    initlock(&cache->lock, "kmem_cache_lock");
    cache->avail_slabs = 0;
    INIT_LIST_HEAD(&cache->partial);
    INIT_LIST_HEAD(&cache->free);
    INIT_LIST_HEAD(&cache->full);

    uint64 aligned_cache_size = (sizeof(struct kmem_cache) + 7) & ~7;
    struct slab *s = (struct slab *)((uint64)cache + aligned_cache_size);
    cache->cache_slab = s;

    uint64 obj_start = (uint64)s + sizeof(struct slab);
    uint max_objs = (PGSIZE - sizeof(struct slab)) / object_size;
    cache->in_cache_obj = max_objs;

    struct run *prev = 0;
    for (uint i = 0; i < max_objs; i++)
    {
        struct run *r = (struct run *)(obj_start + i * object_size);
        if (prev)
            prev->next = r;
        else
            s->freelist = r;
        prev = r;
    }
    if (prev)
        prev->next = 0;

    INIT_LIST_HEAD(&s->list);
    uint64 usable = PGSIZE - sizeof(struct slab);
    debug("[SLAB] New kmem_cache (name: %s, object size: %d bytes, at: %p, max objects per slab: %d, support in cache obj: %d) is created\n",
          cache->name, object_size, cache, max_objs, cache->in_cache_obj);
    debug("[SLAB-DEBUG] sizeof(kmem_cache) = %ld\n", sizeof(struct kmem_cache));
    debug("[SLAB-DEBUG] sizeof(slab)       = %ld\n", sizeof(struct slab));
    debug("[SLAB-DEBUG] cache addr         = %p\n", cache);
    debug("[SLAB-DEBUG] aligned_cache_size = %ld\n", aligned_cache_size);
    debug("[SLAB-DEBUG] obj_start          = %p\n", (void *)obj_start);
    debug("[SLAB-DEBUG] usable bytes       = %ld\n", usable);
    debug("[SLAB-DEBUG] max_objs           = %d\n", max_objs);

    return cache;
}

void *kmem_cache_alloc(struct kmem_cache *cache)
{
    acquire(&cache->lock);
    printf("[SLAB] Alloc request on cache %s\n", cache->name);

    if (cache->in_cache_obj > 0 && cache->cache_slab && cache->cache_slab->freelist)
    {
        struct run *r = cache->cache_slab->freelist;
        cache->cache_slab->freelist = r->next;

        printf("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n",
               r, cache->cache_slab, cache->name);

        release(&cache->lock);
        return (void *)r;
    }

    struct slab *s = NULL;

    if (!list_empty(&cache->partial))
    {
        s = list_first_entry(&cache->partial, struct slab, list);
    }

    if (!s && !list_empty(&cache->free))
    {
        s = list_first_entry(&cache->free, struct slab, list);
        list_del(&s->list);
        list_add(&s->list, &cache->partial);

        uint64 obj_start = (uint64)s + sizeof(struct slab);
        char *obj_base = (char *)obj_start;
        uint max_objs = (PGSIZE - sizeof(struct slab)) / cache->object_size;

        s->freelist = NULL;
        struct run *prev = NULL;
        for (uint i = 0; i < max_objs; i++)
        {
            struct run *r = (struct run *)(obj_base + i * cache->object_size);
            if (prev)
                prev->next = r;
            else
                s->freelist = r;
            prev = r;
        }
        if (prev)
            prev->next = NULL;
    }

    if (!s)
    {
        s = (struct slab *)kalloc();
        if (!s)
        {
            release(&cache->lock);
            return NULL;
        }

        uint64 obj_start = (uint64)s + sizeof(struct slab);
        char *obj_base = (char *)obj_start;
        uint max_objs = (PGSIZE - sizeof(struct slab)) / cache->object_size;

        s->freelist = NULL;
        struct run *prev = NULL;
        for (uint i = 0; i < max_objs; i++)
        {
            struct run *r = (struct run *)(obj_base + i * cache->object_size);
            if (prev)
                prev->next = r;
            else
                s->freelist = r;
            prev = r;
        }
        if (prev)
            prev->next = NULL;

        INIT_LIST_HEAD(&s->list);
        list_add(&s->list, &cache->partial);
        cache->avail_slabs++;

        printf("[SLAB] A new slab %p (%s) is allocated\n", s, cache->name);
    }

    struct run *r = s->freelist;
    s->freelist = r->next;

    printf("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", r, s, cache->name);

    if (s->freelist == NULL)
    {
        list_del(&s->list);
        list_add(&s->list, &cache->full);
    }

    release(&cache->lock);
    return (void *)r;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
    acquire(&cache->lock);

    struct slab *s;
    if ((uint64)obj >= (uint64)cache->cache_slab &&
        (uint64)obj < (uint64)cache + PGSIZE)
    {

        s = cache->cache_slab;
    }
    else
    {

        s = (struct slab *)((uint64)obj & ~(PGSIZE - 1));
    }

    printf("[SLAB] Free %p in slab %p (%s)\n", obj, s, cache->name);

    struct run *r = (struct run *)obj;
    r->next = s->freelist;
    s->freelist = r;

    if (s == cache->cache_slab)
    {
        debug("[SLAB] End of free\n");
        release(&cache->lock);
        return;
    }

    if (!list_empty(&cache->full))
    {

        struct list_head *p;
        list_for_each(p, &cache->full)
        {
            if (list_entry(p, struct slab, list) == s)
            {
                list_del(&s->list);
                list_add(&s->list, &cache->partial);
                break;
            }
        }
    }

    uint max_objs = (PGSIZE - sizeof(struct slab)) / cache->object_size;

    struct run *scan = s->freelist;
    int free_count = 0;
    while (scan)
    {
        free_count++;
        scan = scan->next;
    }

    if (free_count == max_objs)
    {

        list_del(&s->list);

        int partial_cnt = 0, free_cnt = 0;
        struct list_head *p;
        list_for_each(p, &cache->partial) partial_cnt++;
        list_for_each(p, &cache->free) free_cnt++;

        if (partial_cnt + free_cnt >= MP2_MIN_PARTIAL)
        {
            printf("[SLAB] slab %p (%s) is freed due to save memory\n", s, cache->name);
            kfree((void *)s);
        }
        else
        {
            list_add(&s->list, &cache->free);
        }
        cache->avail_slabs--;
    }

    debug("[SLAB] End of free\n");
    release(&cache->lock);
}

int sys_printfslab(void)
{
    if (!file_cache)
    {
        panic("[SLAB] Error: file_cache is NULL (fileinit() may not have been called)");
    }
    print_kmem_cache(file_cache, file_slab_printer);
    return 0;
}
