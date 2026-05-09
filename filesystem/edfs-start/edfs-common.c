/* EdFS -- An educational file system
 *
 * Copyright (C) 2017--2026 Leiden University, The Netherlands.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "edfs-common.h"

/*
 * EdFS image management
 */

/* Close the image file @img.
 */
void
edfs_image_close(edfs_image_t *img)
{
  if(img == NULL)
    return;

  if(img->fd >= 0)
    close(img->fd);

  free(img);
}

/* Read the super block into @img.
 * Returns whether it was valid.
 */
static bool
edfs_read_super(edfs_image_t *img)
{
  if(pread(img->fd, &img->sb, sizeof(edfs_super_block_t),
           EDFS_SUPER_BLOCK_OFFSET) < 0){
    fprintf(stderr, "%s: %s: %s\n",
            progname, img->filename, strerror(errno));
    return false;
  }

  if(img->sb.magic != EDFS_MAGIC){
    fprintf(stderr, "%s: %s: not an EdFS file system.\n",
            progname, img->filename);
    return false;
  }

  /* Simple sanity check of size of file system image. */
  struct stat buf;
  if(fstat(img->fd, &buf) < 0){
    fprintf(stderr, "%s: %s: stat failed (%s)\n",
            progname, img->filename, strerror(errno));
    return false;
  }

  if(buf.st_size < edfs_get_size(&img->sb)){
    fprintf(stderr, "%s: %s: file system size larger than image size.\n",
            progname, img->filename);
    return false;
  }

  return true;
}

/* Open an EdFS image file at @filename. If @read_super is true, the super
 *  block is read into memory.
 * Returns the image.
 */
edfs_image_t *
edfs_image_open(const char *filename, bool read_super)
{
  edfs_image_t *img = malloc(sizeof(edfs_image_t));

  img->filename = filename;
  img->fd = open(img->filename, O_RDWR);
  if(img->fd < 0){
    fprintf(stderr, "%s: %s: %s\n", progname, img->filename, strerror(errno));
    edfs_image_close(img);
    return NULL;
  }

  /* Load super block into memory. */
  if(read_super && !edfs_read_super(img)){
    edfs_image_close(img);
    return NULL;
  }
  return img;
}

/*
 * Inode-related routines
 */

/* Read @inode from disk; @inode->inumber indicates which inode to read.
 * Returns the amount of bytes read.
 */
int
edfs_read_inode(edfs_image_t *img,
                edfs_inode_t *inode)
{
  if(inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);
  return pread(img->fd, &inode->inode, sizeof(edfs_disk_inode_t), offset);
}

/* Reads the root inode from disk into @inode.
 * Returns the amount of bytes read.
 */
int
edfs_read_root_inode(edfs_image_t *img,
                     edfs_inode_t *inode)
{
  inode->inumber = img->sb.root_inumber;
  return edfs_read_inode(img, inode);
}

/* Writes @inode to disk. @inode->inumber indicates which inode to write to.
 * Returns the amount of bytes written.
 */
int
edfs_write_inode(edfs_image_t *img,
                 edfs_inode_t *inode)
{
  if(inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);
  return pwrite(img->fd, &inode->inode, sizeof(edfs_disk_inode_t), offset);
}

/* Clears an inode on disk. @inode->inumber indicates which inode to clear.
 * Returns the amount of bytes written.
 */
int
edfs_clear_inode(edfs_image_t *img,
                 edfs_inode_t *inode)
{
  if(inode->inumber >= img->sb.inode_table_n_inodes)
    return -ENOENT;

  off_t offset = edfs_get_inode_offset(&img->sb, inode->inumber);

  edfs_disk_inode_t disk_inode;
  memset(&disk_inode, 0, sizeof(edfs_disk_inode_t));
  return pwrite(img->fd, &disk_inode, sizeof(edfs_disk_inode_t), offset);
}

/* Finds an unused (free) inode. NOTE: This does not yet allocate the inode!
 * The inode is allocated only after a valid inode has been written to it.
 * Returns the inumber of the free inode,
 *  or 0 if the inode table is full.
 */
edfs_inumber_t
edfs_find_free_inode(edfs_image_t *img)
{
  edfs_inode_t inode = {.inumber = 1};

  for(; inode.inumber < img->sb.inode_table_n_inodes; inode.inumber++){
    if(edfs_read_inode(img, &inode) > 0 &&
       inode.inode.type == EDFS_INODE_TYPE_FREE)
      return inode.inumber;
  }
  return 0;
}

/* Create a new inode with the given @type.
 * Returns 0 on success,
 *  or -ENOSPC if the inode table is full.
 */
int
edfs_new_inode(edfs_image_t *img,
               edfs_inode_t *inode,
               edfs_inode_type_t type)
{
  edfs_inumber_t inumber;

  inumber = edfs_find_free_inode(img);
  if(inumber == 0)
    return -ENOSPC;

  memset(inode, 0, sizeof(edfs_inode_t));
  inode->inumber = inumber;
  inode->inode.type = type;

  return 0;
}

/*
 * Block allocation routines
 */

/* Finds a free block in the bitmap.
 * Returns the block number of the free block,
 *  or EDFS_BLOCK_INVALID if no free block is available.
 */
edfs_block_t
edfs_find_free_block(edfs_image_t *img)
{
  /* Read the bitmap */
  uint8_t *bitmap = malloc(img->sb.bitmap_size);
  if (bitmap == NULL) {
    return EDFS_BLOCK_INVALID;
  }

  ssize_t bytes_read = pread(img->fd, bitmap, img->sb.bitmap_size, img->sb.bitmap_start);
  if (bytes_read != img->sb.bitmap_size) {
    free(bitmap);
    return EDFS_BLOCK_INVALID;
  }

  /* Find a free block (bit set to 0) */
  for (uint32_t byte_idx = 0; byte_idx < img->sb.bitmap_size; byte_idx++) {
    if (bitmap[byte_idx] != 0xFF) {  /* Not all bits set */
      for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
        if ((bitmap[byte_idx] & (1 << bit_idx)) == 0) {
          edfs_block_t block = byte_idx * 8 + bit_idx + 1;  /* +1 because block 0 is invalid */
          if (block < img->sb.n_blocks) {
            free(bitmap);
            return block;
          }
        }
      }
    }
  }

  free(bitmap);
  return EDFS_BLOCK_INVALID;
}

/* Marks a block as allocated in the bitmap.
 * Returns 0 on success, -1 on failure.
 */
int
edfs_allocate_block(edfs_image_t *img, edfs_block_t block)
{
  if (block == EDFS_BLOCK_INVALID || block >= img->sb.n_blocks) {
    return -1;
  }

  /* Calculate bitmap position */
  uint32_t byte_idx = (block - 1) / 8;
  int bit_idx = (block - 1) % 8;

  /* Read the bitmap byte */
  uint8_t bitmap_byte;
  off_t offset = img->sb.bitmap_start + byte_idx;
  ssize_t bytes_read = pread(img->fd, &bitmap_byte, 1, offset);
  if (bytes_read != 1) {
    return -1;
  }

  /* Set the bit */
  bitmap_byte |= (1 << bit_idx);

  /* Write back */
  ssize_t bytes_written = pwrite(img->fd, &bitmap_byte, 1, offset);
  return (bytes_written == 1) ? 0 : -1;
}

/* Marks a block as free in the bitmap.
 * Returns 0 on success, -1 on failure.
 */
int
edfs_free_block(edfs_image_t *img, edfs_block_t block)
{
  if (block == EDFS_BLOCK_INVALID || block >= img->sb.n_blocks) {
    return -1;
  }

  /* Calculate bitmap position */
  uint32_t byte_idx = (block - 1) / 8;
  int bit_idx = (block - 1) % 8;

  /* Read the bitmap byte */
  uint8_t bitmap_byte;
  off_t offset = img->sb.bitmap_start + byte_idx;
  ssize_t bytes_read = pread(img->fd, &bitmap_byte, 1, offset);
  if (bytes_read != 1) {
    return -1;
  }

  /* Clear the bit */
  bitmap_byte &= ~(1 << bit_idx);

  /* Write back */
  ssize_t bytes_written = pwrite(img->fd, &bitmap_byte, 1, offset);
  return (bytes_written == 1) ? 0 : -1;
}

/* Allocates a new block, marks it as allocated, and zeros it out.
 * Returns the block number on success, EDFS_BLOCK_INVALID on failure.
 */
edfs_block_t
edfs_new_block(edfs_image_t *img)
{
  edfs_block_t block = edfs_find_free_block(img);
  if (block == EDFS_BLOCK_INVALID) {
    return EDFS_BLOCK_INVALID;
  }

  if (edfs_allocate_block(img, block) != 0) {
    return EDFS_BLOCK_INVALID;
  }

  /* Zero out the block */
  uint32_t block_offset = edfs_get_block_offset(&img->sb, block);
  void *zero_buffer = calloc(1, img->sb.block_size);
  if (zero_buffer == NULL) {
    edfs_free_block(img, block);
    return EDFS_BLOCK_INVALID;
  }

  ssize_t bytes_written = pwrite(img->fd, zero_buffer, img->sb.block_size, block_offset);
  free(zero_buffer);

  if (bytes_written != img->sb.block_size) {
    edfs_free_block(img, block);
    return EDFS_BLOCK_INVALID;
  }

  return block;
}
