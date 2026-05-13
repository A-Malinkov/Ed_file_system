/* EdFS -- An educational file system
 *
 * Copyright (C) 2017--2026 Leiden University, The Netherlands.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include <unistd.h>
#include <limits.h>
#include <errno.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "edfs-common.h"

char *progname;

/* Returns the EdFS image.
 */
static inline edfs_image_t *
get_edfs_image(void)
{
  return (edfs_image_t *)fuse_get_context()->private_data;
}

static int
edfs_read_block(edfs_image_t *img, edfs_block_t block, void *buf, size_t size)
{
  if(block == EDFS_BLOCK_INVALID)
    return -EINVAL;

  if(size > img->sb.block_size)
    size = img->sb.block_size;

  off_t offset = edfs_get_block_offset(&img->sb, block);
  return pread(img->fd, buf, size, offset);
}

static int
edfs_visit_dir_block(edfs_image_t *img,
                     edfs_block_t block,
                     int (*cb)(const edfs_dir_entry_t *, void *),
                     void *data)
{
  char *buf = malloc(img->sb.block_size);
  if(buf == NULL)
    return -ENOMEM;

  ssize_t got = edfs_read_block(img, block, buf, img->sb.block_size);
  if(got < 0){
    free(buf);
    return (int)got;
  }

  int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);
  edfs_dir_entry_t *entries = (edfs_dir_entry_t *)buf;
  for(int i = 0; i < n_entries; i++){
    if(edfs_dir_entry_is_empty(&entries[i]))
      continue;

    int ret = cb(&entries[i], data);
    if(ret != 0){
      free(buf);
      return ret;
    }
  }

  free(buf);
  return 0;
}

static int
edfs_visit_directory_entries(edfs_image_t *img,
                             const edfs_disk_inode_t *inode,
                             int (*cb)(const edfs_dir_entry_t *, void *),
                             void *data)
{
  if(edfs_inode_has_indirect(inode)){
    int n_blocks = edfs_get_n_blocks_per_indirect_block(&img->sb);
    size_t bufsize = n_blocks * sizeof(edfs_block_t);

    for(int i = 0; i < EDFS_INODE_N_BLOCKS; i++){
      edfs_block_t indirect = inode->blocks[i];
      if(indirect == EDFS_BLOCK_INVALID)
        continue;

      edfs_block_t *block_list = malloc(bufsize);
      if(block_list == NULL)
        return -ENOMEM;

      ssize_t got = edfs_read_block(img, indirect, block_list, bufsize);
      if(got < 0){
        free(block_list);
        return (int)got;
      }

      int n = got / sizeof(edfs_block_t);
      for(int j = 0; j < n; j++){
        if(block_list[j] == EDFS_BLOCK_INVALID)
          break;

        int ret = edfs_visit_dir_block(img, block_list[j], cb, data);
        if(ret != 0){
          free(block_list);
          return ret;
        }
      }
      free(block_list);
    }
  } else {
    for(int i = 0; i < EDFS_INODE_N_BLOCKS; i++){
      if(inode->blocks[i] == EDFS_BLOCK_INVALID)
        continue;

      int ret = edfs_visit_dir_block(img, inode->blocks[i], cb, data);
      if(ret != 0)
        return ret;
    }
  }
  return 0;
}

typedef struct
{
  const char *filename;
  edfs_dir_entry_t *entry;
} edfs_find_dir_entry_data_t;

static int
edfs_find_dir_entry_cb(const edfs_dir_entry_t *entry, void *data)
{
  edfs_find_dir_entry_data_t *d = data;
  if(strcmp(entry->filename, d->filename) == 0){
    *d->entry = *entry;
    return 1;
  }
  return 0;
}

typedef struct
{
  void *buf;
  fuse_fill_dir_t filler;
} edfs_readdir_data_t;

static int
edfs_readdir_cb(const edfs_dir_entry_t *entry, void *data)
{
  edfs_readdir_data_t *d = data;
  int res = d->filler(d->buf, entry->filename, NULL, 0);
  return res != 0 ? res : 0;
}

/* Search the file system hierarchy to find the @inode for the given @path.
 * Returns whether the inode was found.
 */
static bool
edfs_find_inode(edfs_image_t *img,
                const char *path,
                edfs_inode_t *inode)
{
  edfs_inode_t current_inode;
  edfs_read_root_inode(img, &current_inode);

  const char *cur = path,
             *next = cur;
  for(; *next != '\0'; cur = next + 1){
    /* Split @path on slashes */
    next = strchrnul(cur, '/');

    int len = next - cur;
    if(len == 0)
      continue;
    if(len >= EDFS_FILENAME_SIZE)
      return false;

    /* Within the directory pointed to by @current_inode,
     * find the inode number for @direntry.
     */
    edfs_dir_entry_t direntry = {0,};
    strncpy(direntry.filename, cur, len);
    direntry.filename[len] = 0;

    bool found = false;

    /* TODO: Implement.
     * Find the directory entry in @current_inode with that has the same
     * filename as @direntry, and store its inode number in direntry.inumber.
     */
    edfs_find_dir_entry_data_t data = {
      .filename = cur,
      .entry = &direntry,
    };
    int rc = edfs_visit_directory_entries(img, &current_inode.inode,
                                         edfs_find_dir_entry_cb, &data);
    if(rc == 1)
      found = true;
    else if(rc < 0)
      return false;

    if(!found)
      return false;

    /* Found the inode for this part; continue searching there */
    current_inode.inumber = direntry.inumber;
    edfs_read_inode(img, &current_inode);
  }

  *inode = current_inode;
  return true;
}

/*
 * Implementation of necessary FUSE operations.
 */

/* Read a directory listing for the directory at @path. Each directory entry is
 *  provided to the function @filler.
 * Returns 0 on success,
 *  -ENOENT if @path could not be found,
 *  -ENOTDIR if @path is not a directory.
 */
static int
edfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{
  edfs_image_t *img = get_edfs_image();
  edfs_inode_t inode = {0,};

  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;
  if(!edfs_inode_is_directory(&inode.inode))
    return -ENOTDIR;

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);


  /* TODO: Call the function filler for all valid directory entries of @inode.
   * The second argument of filler is the filename of the directory entry.
   */
  edfs_readdir_data_t data = {
    .buf = buf,
    .filler = filler,
  };

  int rc = edfs_visit_directory_entries(img, &inode.inode,
                                       edfs_readdir_cb, &data);
  if(rc < 0)
    return -EIO;

  return 0;
}


/* Create a new directory at @path. The @mode is ignored.
 * Returns 0 on success,
 *  ...
 */
static int
edfuse_mkdir(const char *path, mode_t mode)
{
  /* TODO: Implement.
   *
   * Create a new inode; register it in the parent directory, write the inode
   * to disk.
   */
  return -ENOSYS;
}

/* Removes the directory at @path.
 * Returns 0 on success,
 *  ...
 */
static int
edfuse_rmdir(const char *path)
{
  /* TODO: Implement.
   *
   * Validate @path exists and is an empty directory; remove directory entry
   * from its parent directory; release allocated blocks; release inode.
   */
  return -ENOSYS;
}

/* Sets @stbuf to the attributes of the file or directory at @path.
 * Returns 0 on success,
 *  -ENOENT if @path could not be found.
 */
static int
edfuse_getattr(const char *path, struct stat *stbuf)
{
  /* At least mode, nlink and size must be filled in, otherwise the "ls"
   * listings appear broken.
   */
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;

  memset(stbuf, 0, sizeof(struct stat));
  if(strcmp(path, "/") == 0){
    /* Root directory */
    stbuf->st_mode = S_IFDIR | 0755; /* drwxr-xr-x */
    stbuf->st_nlink = 2;
  }else if(edfs_inode_is_directory(&inode.inode)){
    stbuf->st_mode = S_IFDIR | 0770; /* drwxrwx--- */
    stbuf->st_nlink = 2;
  }else{
    stbuf->st_mode = S_IFREG | 0660; /* -rw-rw---- */
    stbuf->st_nlink = 1;
  }
  stbuf->st_size = inode.inode.size;

  /* This setting is ignored unless the FUSE file system is mounted with the
   * option "use_ino".
   */
  stbuf->st_ino = inode.inumber;
  return 0;
}

/* Open file at @path; we only verify it exists. A real file system would keep
 *  track of which files are open.
 * Returns 0 on success,
 *  -ENOENT if @path could not be found,
 *  -EISDIR if @path is a directory.
 */
static int
edfuse_open(const char *path, struct fuse_file_info *fi)
{
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;

  /* Open may only be called on files */
  if(edfs_inode_is_directory(&inode.inode))
    return -EISDIR;

  return 0;
}

/* Create a new empty file at @path. The @mode is ignored.
 * Returns 0 on success,
 *  ...
 */
static int
edfuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  /* TODO: Implement.
   *
   * Create a new inode; register it in the parent directory; write the inode
   * to disk.
   */
  return -ENOSYS;
}

/* Remove one of the links to @path. If the link count becomes zero, the
 *  file is deleted.
 */
static int
edfuse_unlink(const char *path)
{
  /* NOTE: Not implemented and not part of the assignment. */
  return -ENOSYS;
}

/* Read at most @size bytes from offset @offset in the file at @path into the
 *  buffer @buf.
 * Returns the amount of bytes read,
 *  or ...
 * (TODO: See `man errno` for a list of error codes.)
 */
static int
edfuse_read(const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
  edfs_image_t *img = get_edfs_image();
  edfs_inode_t inode = {0,};

  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;
  if(edfs_inode_is_directory(&inode.inode))
    return -EISDIR;

  if(offset >= inode.inode.size)
    return 0;
  if(offset + size > inode.inode.size)
    size = inode.inode.size - offset;
  if(size == 0)
    return 0;

  uint32_t block_size = img->sb.block_size;
  unsigned int first_block = offset / block_size;
  unsigned int last_block = (offset + size - 1) / block_size;
  size_t remaining = size;
  size_t written = 0;

  for(unsigned int block_index = first_block;
      block_index <= last_block;
      block_index++){
    edfs_block_t data_block = EDFS_BLOCK_INVALID;
    unsigned int need = block_index;

    if(edfs_inode_has_indirect(&inode.inode)){
      int n_blocks = edfs_get_n_blocks_per_indirect_block(&img->sb);
      size_t bufsize = n_blocks * sizeof(edfs_block_t);
      edfs_block_t *table = malloc(bufsize);
      if(table == NULL)
        return -ENOMEM;

      for(int i = 0; i < EDFS_INODE_N_BLOCKS; i++){
        edfs_block_t indirect = inode.inode.blocks[i];
        if(indirect == EDFS_BLOCK_INVALID)
          continue;

        ssize_t got = edfs_read_block(img, indirect, table, bufsize);
        if(got < 0){
          free(table);
          return -EIO;
        }

        int n = got / sizeof(edfs_block_t);
        for(int j = 0; j < n; j++){
          if(table[j] == EDFS_BLOCK_INVALID)
            break;
          if(need == 0){
            data_block = table[j];
            break;
          }
          need--;
        }

        if(data_block != EDFS_BLOCK_INVALID)
          break;
      }
      free(table);
    } else {
      if(need < EDFS_INODE_N_BLOCKS)
        data_block = inode.inode.blocks[need];
    }

    if(data_block == EDFS_BLOCK_INVALID)
      return -EIO;

    off_t block_offset = 0;
    if(block_index == first_block)
      block_offset = offset % block_size;

    size_t to_read = block_size - block_offset;
    if(to_read > remaining)
      to_read = remaining;

    off_t read_offset = edfs_get_block_offset(&img->sb, data_block) + block_offset;
    ssize_t got = pread(img->fd, buf + written, to_read, read_offset);
    if(got < 0)
      return -EIO;

    written += got;
    remaining -= got;
  }

  return written;
}

/* Write @size bytes from the buffer @buf at offset @offset in the file at
 *  @path.
 * Returns the amount of bytes written,
 *  ...
 */
static int
edfuse_write(const char *path, const char *buf, size_t size, off_t offset,
             struct fuse_file_info *fi)
{
  /* TODO: Implement.
   *
   * Write @size bytes of data from @buf to the file at @path, starting at
   * @offset.
   *
   * Allocate new blocks (and update the file size) if the @size or @offset
   * (or both) are larger than the file size. New blocks must be zeroed.
   */
  return -ENOSYS;
}

/* Change the size of the file at @path to @offset.
 * Returns 0 on success,
 *  ...
 */
static int
edfuse_truncate(const char *path, off_t offset)
{
  /* TODO: Implement.
   *
   * The size of the file at @path must be set to be @offset; this may be
   * smaller or larger than the current file size. Release now superfluous
   * blocks or allocate new blocks that are necessary to cover @offset bytes.
   */
  return -ENOSYS;
}

/*
 * FUSE setup
 */
static struct fuse_operations edfs_oper =
{
  .readdir   = edfuse_readdir,
  .mkdir     = edfuse_mkdir,
  .rmdir     = edfuse_rmdir,
  .getattr   = edfuse_getattr,
  .open      = edfuse_open,
  .create    = edfuse_create,
  .unlink    = edfuse_unlink,
  .read      = edfuse_read,
  .write     = edfuse_write,
  .truncate  = edfuse_truncate,
};

int
main(int argc, char *argv[])
{
  progname = argv[0];

  /* Count number of non-option arguments */
  int count = 0;
  for(int i = 1; i < argc; i++){
    if(argv[i][0] != '-')
      count++;
  }
  if(count != 2){
    fprintf(stderr, "usage: %s [-f] [-s] mountpoint image\n", progname);
    fprintf(stderr, "\t-f\trun in foreground\n");
    fprintf(stderr, "\t-s\tdisable multithreading\n");
    return 1;
  }

  /* Try to open the file system */
  edfs_image_t *img = edfs_image_open(argv[argc - 1], true);
  if(img == NULL)
    return 1;

  /* Start FUSE main loop */
  int ret = fuse_main(argc - 1, argv, &edfs_oper, img);
  edfs_image_close(img);

  return ret;
}
