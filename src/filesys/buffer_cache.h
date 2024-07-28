//
// Created by will on 2024/7/28.
//
#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "off_t.h"

void buffer_cache_init (void);
void buffer_cache_done (void);
void buffer_cache_read_sector_at (struct block *block, block_sector_t sector, void *buffer,
                                  off_t size, off_t offset);
void buffer_cache_read_sector (struct block *block, block_sector_t sector, void *buffer);
void buffer_cache_write_sector_at (struct block *block, block_sector_t sector, const void *buffer,
                              off_t size, off_t offset);
void buffer_cache_write_sector (struct block *block, block_sector_t sector, const void *buffer);
#endif