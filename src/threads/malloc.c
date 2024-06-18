#include "threads/malloc.h"
#include <debug.h>
#include <list.h>
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/** A simple implementation of malloc().

   The size of each request, in bytes, is rounded up to a power
   of 2 and assigned to the "descriptor" that manages blocks of
   that size.  The descriptor keeps a list of free blocks.  If
   the free list is nonempty, one of its blocks is used to
   satisfy the request.
   内存请求和描述符(descriptor):
    当进行内存分配请求时,请求的大小会被向上取整到最近的2的幂.
    每个2的幂的大小(例如8字节,16字节m32字节等)都有一个对应的"descriptor".
    每个descriptor管理一个特定大小的空闲内存块列表(free list).
    如果空闲列表中有可用的块,就会使用其中的一个块来满足内存请求.

   Otherwise, a new page of memory, called an "arena", is
   obtained from the page allocator (if none is available,
   malloc() returns a null pointer).  The new arena is divided
   into blocks, all of which are added to the descriptor's free
   list.  Then we return one of the new blocks.
   内存池(Arena):
    如果空闲列表中没有可用的块,则会从页面分配器(page allocator)中请求一个新的[[内存页(称为"内存池"或"Arena")]]]
    内存池会被划分成较小的块,这些块的大小由descriptor管理.
    这些新划分的块会被添加到descriptor的空闲列表中.
    然后返回其中的一个新块来满足内存请求.

   When we free a block, we add it to its descriptor's free list.
   But if the arena that the block was in now has no in-use
   blocks, we remove all of the arena's blocks from the free list
   and give the arena back to the page allocator.
   释放内存:
    当释放一个内存块时,这个块会被添加到它对应的descriptor的空闲列表中.
    [[[如果内存池中的所有块都被释放,则会将整个内存池的块从空闲列表中移除,并将内存池归还给页面分配器]]]

   We can't handle blocks bigger than 2 kB using this scheme,
   because they're too big to fit in a single page with a
   descriptor.  We handle those by allocating contiguous pages
   with the page allocator and sticking the allocation size at
   the beginning of the allocated block's arena header.
   大块内存分配:
    对于超过2 KB的内存请求,无法使用上述方法处理,因为它们太大,无法与descriptor一起放入单个页面.
    这些请求会通过页面分配器分配连续的多个页面,并在分配块的内存池头部记录分配大小.
*/

/** Descriptor. */
struct desc
  {
    size_t block_size;          /**< Size of each element in bytes. */
    size_t blocks_per_arena;    /**< Number of blocks in an arena. */
    struct list free_list;      /**< List of free blocks. */
    struct lock lock;           /**< Lock. */
  };

/** Magic number for detecting arena corruption. */
#define ARENA_MAGIC 0x9a548eed

// 一个内存池中block_size都一样
/** Arena. */
struct arena 
  {
    unsigned magic;             /**< Always set to ARENA_MAGIC. */
    struct desc *desc;          /**< Owning descriptor, null for big block. */
    size_t free_cnt;            /**< Free blocks; pages in big block. */
  };

/** Free block. */
struct block 
  {
    struct list_elem free_elem; /**< Free list element. */
  };

/** Our set of descriptors. */
static struct desc descs[10];   /**< Descriptors. */
static size_t desc_cnt;         /**< Number of descriptors. */

static struct arena *block_to_arena (struct block *);
static struct block *arena_to_block (struct arena *, size_t idx);

/** Initializes the malloc() descriptors. */
void
malloc_init (void) 
{
  size_t block_size;

  // 最小block_size是16
  for (block_size = 16; block_size < PGSIZE / 2; block_size *= 2)
    {
      struct desc *d = &descs[desc_cnt++];
      ASSERT (desc_cnt <= sizeof descs / sizeof *descs);
      d->block_size = block_size;
      // TODO - sizeof (struct arena) 用意?
      // 因为 a = palloc_get_multiple (0, page_cnt); arena在页最开始
      d->blocks_per_arena = (PGSIZE - sizeof (struct arena)) / block_size;
      list_init (&d->free_list);
      lock_init (&d->lock);
    }
  // 目前还没有实际分配内存,是malloc时发现desc中free_list为空后才palloc_get_page,
  // 然后分为block加入free_list
}

// from the kernel pool
// 不会拆分内存块,比如需要12,分配16,多余的4不会拆出来用
/** Obtains and returns a new block of at least SIZE bytes.
   Returns a null pointer if memory is not available. */
void *
malloc (size_t size) 
{
  struct desc *d;
  struct block *b;
  struct arena *a;

  /* A null pointer satisfies a request for 0 bytes. */
  if (size == 0)
    return NULL;

  /* Find the smallest descriptor that satisfies a SIZE-byte
     request. */
  for (d = descs; d < descs + desc_cnt; d++)
    if (d->block_size >= size)
      break;
  if (d == descs + desc_cnt) 
    {
      // 需要的内存太大,所有descriptor都不满足
      /* SIZE is too big for any descriptor.
         Allocate enough pages to hold SIZE plus an arena. */
      size_t page_cnt = DIV_ROUND_UP (size + sizeof *a, PGSIZE);
      a = palloc_get_multiple (0, page_cnt);
      if (a == NULL)
        return NULL;

      // free_cnt表示需要释放的page数量
      /* Initialize the arena to indicate a big block of PAGE_CNT
         pages, and return it. */
      a->magic = ARENA_MAGIC;
      a->desc = NULL;
      a->free_cnt = page_cnt;
      return a + 1;
    }

  lock_acquire (&d->lock);

  /* If the free list is empty, create a new arena. */
  if (list_empty (&d->free_list))
    {
      size_t i;

      /* Allocate a page. */
      a = palloc_get_page (0);
      if (a == NULL) 
        {
          lock_release (&d->lock);
          return NULL; 
        }

      /* Initialize arena and add its blocks to the free list. */
      a->magic = ARENA_MAGIC;
      a->desc = d;
      a->free_cnt = d->blocks_per_arena;
      for (i = 0; i < d->blocks_per_arena; i++) 
        {
          struct block *b = arena_to_block (a, i);
          list_push_back (&d->free_list, &b->free_elem);
        }
    }

  /* Get a block from free list and return it. */
  b = list_entry (list_pop_front (&d->free_list), struct block, free_elem);
  a = block_to_arena (b);
  a->free_cnt--;
  lock_release (&d->lock);
  return b;
}

// from the kernel pool, 该块的内容将被清零
/** Allocates and return A times B bytes initialized to zeroes.
   Returns a null pointer if memory is not available. */
void *
calloc (size_t a, size_t b) 
{
  void *p;
  size_t size;

  /* Calculate block size and make sure it fits in size_t. */
  size = a * b;
  if (size < a || size < b)
    return NULL;

  /* Allocate and zero memory. */
  p = malloc (size);
  if (p != NULL)
    memset (p, 0, size);

  return p;
}

/** Returns the number of bytes allocated for BLOCK. */
static size_t
block_size (void *block) 
{
  struct block *b = block;
  struct arena *a = block_to_arena (b);
  struct desc *d = a->desc;

  return d != NULL ? d->block_size : PGSIZE * a->free_cnt - pg_ofs (block);
}

// 可能会在此过程中移动它
// 使用 block null 的调用相当于 malloc(), new_size 为零的调用相当于 free()
/** Attempts to resize OLD_BLOCK to NEW_SIZE bytes, possibly
   moving it in the process.
   If successful, returns the new block; on failure, returns a
   null pointer.
   A call with null OLD_BLOCK is equivalent to malloc(NEW_SIZE).
   A call with zero NEW_SIZE is equivalent to free(OLD_BLOCK). */
void *
realloc (void *old_block, size_t new_size) 
{
  if (new_size == 0) 
    {
      free (old_block);
      return NULL;
    }
  else 
    {
      void *new_block = malloc (new_size);
      if (old_block != NULL && new_block != NULL)
        {
          size_t old_size = block_size (old_block);
          size_t min_size = new_size < old_size ? new_size : old_size;
          memcpy (new_block, old_block, min_size);
          free (old_block);
        }
      return new_block;
    }
}

/** Frees block P, which must have been previously allocated with
   malloc(), calloc(), or realloc(). */
void
free (void *p) 
{
  if (p != NULL)
    {
      struct block *b = p;
      struct arena *a = block_to_arena (b);
      struct desc *d = a->desc;
      
      if (d != NULL) 
        {
          /* It's a normal block.  We handle it here. */

#ifndef NDEBUG
          /* Clear the block to help detect use-after-free bugs. */
          memset (b, 0xcc, d->block_size);
#endif
  
          lock_acquire (&d->lock);

          /* Add block to free list. */
          list_push_front (&d->free_list, &b->free_elem);

          /* If the arena is now entirely unused, free it. */
          if (++a->free_cnt >= d->blocks_per_arena) 
            {
              size_t i;

              ASSERT (a->free_cnt == d->blocks_per_arena);
              for (i = 0; i < d->blocks_per_arena; i++) 
                {
                  struct block *b = arena_to_block (a, i);
                  list_remove (&b->free_elem);
                }
              palloc_free_page (a);
            }

          lock_release (&d->lock);
        }
      else
        {
          /* It's a big block.  Free its pages. */
          palloc_free_multiple (a, a->free_cnt);
          return;
        }
    }
}

/** Returns the arena that block B is inside. */
static struct arena *
block_to_arena (struct block *b)
{
  // 因为block和arena在同一个内存页,所以直接把 *b round_down
  struct arena *a = pg_round_down (b);

  /* Check that the arena is valid. */
  ASSERT (a != NULL);
  ASSERT (a->magic == ARENA_MAGIC);

  /* Check that the block is properly aligned for the arena. */
  ASSERT (a->desc == NULL
          || (pg_ofs (b) - sizeof *a) % a->desc->block_size == 0);
  ASSERT (a->desc != NULL || pg_ofs (b) == sizeof *a);

  return a;
}

/** Returns the (IDX - 1)'th block within arena A. */
static struct block *
arena_to_block (struct arena *a, size_t idx) 
{
  ASSERT (a != NULL);
  ASSERT (a->magic == ARENA_MAGIC);
  ASSERT (idx < a->desc->blocks_per_arena);
  return (struct block *) ((uint8_t *) a
                           + sizeof *a
                           + idx * a->desc->block_size);
}
