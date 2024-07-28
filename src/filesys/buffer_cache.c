//
// Created by will on 2024/7/28.
//
#include <debug.h>
#include <string.h>
#include "filesys/buffer_cache.h"
#include "filesys/filesys.h"
#include "stdio.h"
#include "threads/synch.h"

// 512B * 64 = 32768 = 8 * 4k
#define MAX_CACHE_SIZE 64

static struct lock buffer_cache_lock;

struct buffer_cache_slot {
    size_t id;
    block_sector_t sector;
    uint8_t data[BLOCK_SECTOR_SIZE];
    bool in_use;
    bool dirty;
    bool pined;
    bool access;
};

static struct buffer_cache_slot cache[MAX_CACHE_SIZE];

void
buffer_cache_init (void) {
  lock_init (&buffer_cache_lock);

  for (size_t i = 0; i < MAX_CACHE_SIZE; ++i) {
    cache[i].id = i;
    cache[i].in_use = false;
    cache[i].dirty = false;
    cache[i].pined = false;
    cache[i].access = false;
  }
}

static void
buffer_cache_write_back (struct buffer_cache_slot *cache_slot) {
  ASSERT (lock_held_by_current_thread(&buffer_cache_lock));
  ASSERT (cache_slot && cache_slot->in_use == true);

  if (cache_slot->dirty) {
    block_write (fs_device, cache_slot->sector, cache_slot->data);
    cache_slot->dirty = false;
  }
}

static struct buffer_cache_slot*
buffer_cache_evict (void) {
  ASSERT (lock_held_by_current_thread(&buffer_cache_lock));

  static size_t clock = 0;
  while (true) {
    if (cache[clock].in_use == false) {
      return &(cache[clock]);
    }

    if (cache[clock].pined == false) {
      if (cache[clock].access) {
        cache[clock].access = false;
      }
      else break;
    }

    clock ++;
    clock %= MAX_CACHE_SIZE;
  }

  struct buffer_cache_slot *slot = &cache[clock];
  if (slot->dirty) {
    buffer_cache_write_back(slot);
  }

  slot->in_use = false;
  return slot;
}

static struct buffer_cache_slot *
get_cache_slot (block_sector_t sector) {
  ASSERT (lock_held_by_current_thread(&buffer_cache_lock));

  struct buffer_cache_slot *cache_slot = NULL;

  for (size_t i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (cache[i].in_use && cache[i].sector == sector) {
      cache_slot = &cache[i];
      break;
    }
  }

  if (cache_slot == NULL) {
    cache_slot = buffer_cache_evict ();
    ASSERT (cache_slot != NULL && cache_slot->in_use == false);

    cache_slot->in_use = true;
    cache_slot->sector = sector;
    cache_slot->dirty = false;
    block_read (fs_device, sector, cache_slot->data);
  }
  return cache_slot;
}

void
buffer_cache_read_sector_at (struct block *block, block_sector_t sector, void *buffer,
                             off_t size, off_t offset) {
  if (block == fs_device) {
    if (offset < 0) {
      PANIC("buffer_cache_write_sector_at");
    }
    if (size <= 0) {
      return;
    }
    lock_acquire (&buffer_cache_lock);

    struct buffer_cache_slot *cache_slot = get_cache_slot(sector);
    // printf("read sector %d from %d\n", sector, cache_slot->id);
    cache_slot->access = true;
    memcpy (buffer, cache_slot->data + offset, size);

    lock_release (&buffer_cache_lock);
  } else {
    PANIC("buffer_cache_read_sector_at");
  }
}

void
buffer_cache_read_sector (struct block *block, block_sector_t sector, void *buffer) {
  if (block == fs_device) {
    buffer_cache_read_sector_at(block, sector, buffer, BLOCK_SECTOR_SIZE, 0);
  } else {
    block_read (block, sector, buffer);
  }
}

void
buffer_cache_write_sector_at (struct block *block, block_sector_t sector, const void *buffer,
                              off_t size, off_t offset) {
  if (block == fs_device) {
    if (offset < 0) {
      PANIC("buffer_cache_write_sector_at");
    }
    if (size <= 0) {
      return;
    }
    lock_acquire (&buffer_cache_lock);

    struct buffer_cache_slot *cache_slot = get_cache_slot(sector);
    // printf("write sector %d at %d\n", sector, cache_slot->id);
    cache_slot->access = true;
    cache_slot->dirty = true;
    memcpy (cache_slot->data + offset, buffer, size);

    lock_release (&buffer_cache_lock);
  } else {
    PANIC("buffer_cache_write_sector_at");
  }
}

void
buffer_cache_write_sector (struct block *block, block_sector_t sector, const void *buffer) {
  if (block == fs_device) {
    buffer_cache_write_sector_at(block, sector, buffer, BLOCK_SECTOR_SIZE, 0);
  } else {
    block_write (block, sector, buffer);
  }
}

void
buffer_cache_done (void) {
  lock_acquire (&buffer_cache_lock);

  for (size_t i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (cache[i].in_use) {
      buffer_cache_write_back(&(cache[i]));
    }
  }

  lock_release (&buffer_cache_lock);
}
