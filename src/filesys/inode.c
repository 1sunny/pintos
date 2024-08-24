#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "buffer_cache.h"

/** Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define N_DIRECT ((int)10)
#define N_DIRECT_BYTES (N_DIRECT * BLOCK_SECTOR_SIZE)
#define N_INDIRECT ((int)(BLOCK_SECTOR_SIZE / sizeof(uint32_t)))
#define N_INDIRECT_BYTES (N_INDIRECT * BLOCK_SECTOR_SIZE)
#define N_INDIRECT2 (N_INDIRECT * N_INDIRECT)
#define N_INDIRECT2_BYTES (N_INDIRECT2 * BLOCK_SECTOR_SIZE)
#define MAX_FILE_SIZE (N_DIRECT_BYTES + N_INDIRECT_BYTES + N_INDIRECT2_BYTES)

// TODO In-memory inode和On-disk inode区别?
// In-memory inode(struct inode)中包含存储inode_disk的sector,inode_disk中记录文件实际占用了哪些sector
/** On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    // block_sector_t start;               /**< First data sector. */
    off_t length;                       /**< File size in bytes. */
    int direct[N_DIRECT];            /**< Not used. */
    int indirect;
    int indirect2;
    enum FILE_TYPE file_type;
    char unused[BLOCK_SECTOR_SIZE - sizeof (off_t) - (N_DIRECT + 2) * sizeof (int) - sizeof (enum FILE_TYPE) - sizeof (unsigned)];
    unsigned magic;                     /**< Magic number. */
  };

// 返回size个字节需要的扇区数
/** Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/** In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /**< Element in inode list. */
    block_sector_t sector;              /**< Sector number of disk location. */
    int open_cnt;                       /**< Number of openers. */
    bool removed;                       /**< True if deleted, false otherwise. */
    int deny_write_cnt;                 /**< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /**< Inode content. */
  };

block_sector_t
inode_get_sector (struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->sector;
}

int
inode_get_open_cnt (struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->open_cnt;
}

enum FILE_TYPE
inode_get_file_type (struct inode *inode) {
  ASSERT(inode->data.file_type != UNKNOWN);
  return inode->data.file_type;
}

// 返回包含inode对应文件中第pos个字节所在的扇区编号
/** Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static int
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    if (pos < N_DIRECT_BYTES) {
      ASSERT(inode->data.direct[pos / BLOCK_SECTOR_SIZE] != -1);
      return inode->data.direct[pos / BLOCK_SECTOR_SIZE];
    }
    pos -= N_DIRECT_BYTES;
    if (pos < N_INDIRECT_BYTES) {
      ASSERT(inode->data.indirect != -1);
      block_sector_t res;
      buffer_cache_read_sector_at(fs_device, inode->data.indirect, &res, sizeof res, (pos / BLOCK_SECTOR_SIZE) * (sizeof res));
      return res;
    }
    pos -= N_INDIRECT_BYTES;
    ASSERT(pos < N_INDIRECT2_BYTES);
    off_t i = pos / N_INDIRECT_BYTES;
    off_t j = (pos % N_INDIRECT_BYTES) / BLOCK_SECTOR_SIZE;
    block_sector_t res;
    buffer_cache_read_sector_at(fs_device, inode->data.indirect2, &res, sizeof res, i * (sizeof res));
    buffer_cache_read_sector_at(fs_device, res, &res, sizeof res, j * (sizeof res));
    return res;
  }
  else {
    return -1;
  }
}

/** List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/** Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

static bool
alloc_zeroed_sector (block_sector_t *sector, bool is_data) {
  if (free_map_allocate (1, sector) == false) {
    return false;
  }
  if (is_data) {
    // printf("alloc: %d\n", *sector);
  }
  static char zeros[BLOCK_SECTOR_SIZE];
  buffer_cache_write_sector(fs_device, *sector, zeros);
  return true;
}

static bool
alloc_indirect_sector (block_sector_t *indirect_sector, int base, int grow) {
  ASSERT(*indirect_sector != 0);
  ASSERT(base >= 0 && grow >= 0);
  ASSERT(base + grow <= N_INDIRECT);
  for (int i = base; i < base + grow; ++i) {
    block_sector_t cur;
    if (alloc_zeroed_sector(&cur, true) == false) {
      return false;
    }
    buffer_cache_write_sector_at(fs_device, *indirect_sector, &cur, sizeof cur, i * (sizeof cur));
  }
  return true;
}

static bool
inode_grow_sectors (struct inode_disk *inode_disk, int sectors) {
  ASSERT(sectors >= 0);
  if (sectors == 0) {
    return true;
  }
  int cur_sectors = bytes_to_sectors(inode_disk->length);
  // size_t和int比较会把int转为size_t !!!
  // min(要增长的sectors, 能在DIRECT中增长的)
  int grow_direct = sectors < (N_DIRECT - cur_sectors) ? sectors : (N_DIRECT - cur_sectors);
  if (grow_direct > 0) {
    ASSERT(cur_sectors + grow_direct <= N_INDIRECT);
    for (int i = cur_sectors; i < cur_sectors + grow_direct; ++i) {
      if (alloc_zeroed_sector(&inode_disk->direct[i], true) == false) {
        // TODO 应该把之前分配的释放掉
        return false;
      }
    }
    sectors -= grow_direct;
    cur_sectors += grow_direct;
    if (sectors == 0) {
      return true;
    }
  }
  ASSERT(sectors > 0);
  // 如果cur_sectors恰好等于N_DIRECT,并且现在还要分配sectors>0个扇区,就需要分配inode_disk->indirect
  if (cur_sectors == N_DIRECT) {
    if (alloc_zeroed_sector(&inode_disk->indirect, false) == false) {
      // TODO 应该把之前分配的释放掉
      return false;
    }
  }
  // 如果INDIRECT中有空余位置
  if (cur_sectors - N_DIRECT < N_INDIRECT) {
    // min(要增长的sectors, 能在INDIRECT中增长的)
    int grow_indirects = sectors < N_INDIRECT - (cur_sectors - N_DIRECT) ? sectors : N_INDIRECT - (cur_sectors - N_DIRECT);
    if (grow_indirects > 0) {
      ASSERT(grow_indirects <= N_INDIRECT && cur_sectors - N_DIRECT + grow_indirects <= N_INDIRECT);
      if (alloc_indirect_sector(&inode_disk->indirect, cur_sectors - N_DIRECT, grow_indirects) == false) {
        return false;
      }
      sectors -= grow_indirects;
      cur_sectors += grow_indirects;
      if (sectors == 0) {
        return true;
      }
    }
  }
  ASSERT(sectors > 0);
  if (cur_sectors == N_DIRECT + N_INDIRECT) {
    if (alloc_zeroed_sector(&inode_disk->indirect2, false) == false) {
      return false;
    }
  }
  while (sectors > 0) {
    int base = cur_sectors - N_DIRECT - N_INDIRECT;
    int index_indirect = base / N_INDIRECT;
    int off_indirect = base % N_INDIRECT;
    block_sector_t indirect_sector;
    // off_indirect:这个indirect中已有的sector数量
    if (off_indirect == 0) {
      // 这个indirect槽还没分配(off_indirect = 0),需要分配
      if (alloc_zeroed_sector(&indirect_sector, false) == false) {
        return false;
      }
      buffer_cache_write_sector_at(fs_device, inode_disk->indirect2, &indirect_sector, sizeof indirect_sector,
                                   index_indirect * (sizeof indirect_sector));
    } else {
      buffer_cache_read_sector_at(fs_device, inode_disk->indirect2, &indirect_sector, sizeof indirect_sector,
                                   index_indirect * (sizeof indirect_sector));
    }
    int grow = sectors < (N_INDIRECT - off_indirect) ? sectors : (N_INDIRECT - off_indirect);
    if (alloc_indirect_sector(&indirect_sector, off_indirect, grow) == false) {
      return false;
    }
    cur_sectors += grow;
    sectors -= grow;
  }
  return true;
}

// 在第sector个扇区处创建一个长度为length的on-disk inode,会维护free map
/** Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, enum FILE_TYPE file_type)
{
  ASSERT(file_type != UNKNOWN);

  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);
  if (length > MAX_FILE_SIZE) {
    goto fail;
  }

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      if (inode_grow_sectors(disk_inode, sectors) == false) {
        goto fail;
      }
      disk_inode->length = length;
      disk_inode->file_type = file_type;
      buffer_cache_write_sector(fs_device, sector, disk_inode);
    }
  return true;

fail:
  free (disk_inode);
  return false;
}

// 从第sector个扇区读取on-disk inode, 并返回一个通过malloc分配的struct inode标识这个on-dist inode
/** Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  buffer_cache_read_sector(fs_device, inode->sector, &inode->data);
  // block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/** Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/** Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

// 关闭文件的inode并将其对应的所有sector写入到磁盘
/** Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      buffer_cache_write_sector(fs_device, inode->sector, &inode->data);
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
        // TODO
          // free_map_release (inode->sector, 1);
          // free_map_release (inode->data.start,
          //                   bytes_to_sectors (inode->data.length)); 
        }

      free (inode); 
    }
}

// TODO inode什么时候free呢? inode_close
// 一个进程退出时需要关闭它打开的文件,之前已经实现了
/** Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

// 从inode对应的文件offset偏移处读取size个字节到buffer_(就是读所有inode中的sectors)
// 因为没有实现缓冲区,对于一个并不需要完全读取的扇区,
// 现在还是通过将扇区读到临时分配一个BLOCK_SECTOR_SIZE大小的缓冲区,然后从缓冲区复制指定个数到buffer_
/** Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      int sector_idx = byte_to_sector (inode, offset);
      // printf("read %d\n", sector_idx);
      if (sector_idx == -1) {
        return 0;
      }
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      buffer_cache_read_sector_at(fs_device, sector_idx, buffer + bytes_read, chunk_size, sector_ofs);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

static bool
inode_grow_to (struct inode *inode, off_t new_size) {
  if (new_size < 0 || new_size > MAX_FILE_SIZE) {
    return false;
  }
  size_t old_sectors = bytes_to_sectors(inode_length(inode));
  size_t new_sectors = bytes_to_sectors(new_size);
  if (new_sectors - old_sectors <= 0) {
    // TODO set 0
    inode->data.length = new_size;
    return true;
  }
  if (inode_grow_sectors(&inode->data, new_sectors - old_sectors) == false) {
    return false;
  }
  // TODO set 0
  inode->data.length = new_size;
  return true;
}

/** Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (size < 0 || offset < 0)
    return 0;

  if (inode_length(inode) < offset + size) {
    if (inode_grow_to(inode, offset + size) == false) {
      return 0;
    }
  }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      // printf("write %d\n", sector_idx);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      buffer_cache_write_sector_at(fs_device, sector_idx, buffer + bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/** Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/** Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/** Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
