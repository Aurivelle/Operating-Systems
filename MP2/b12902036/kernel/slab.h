#pragma once

#include "types.h"
#include "spinlock.h"
#include "list.h"

struct run
{
    struct run *next;
};
struct slab
{

    struct run *freelist;
    struct list_head list;
};

struct kmem_cache
{
    struct slab *cache_slab;
    char name[32];
    uint object_size;
    struct spinlock lock;
    int in_cache_obj;
    int avail_slabs;

    struct list_head partial;
    struct list_head free;
    struct list_head full;
};

struct kmem_cache *kmem_cache_create(const char *name, uint object_size);

void kmem_cache_destroy(struct kmem_cache *cache);

void *kmem_cache_alloc(struct kmem_cache *cache);

void kmem_cache_free(struct kmem_cache *cache, void *obj);

void print_kmem_cache(struct kmem_cache *cache, void (*print_fn)(void *));