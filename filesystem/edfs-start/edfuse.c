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
edfs_write_block(edfs_image_t *img, edfs_block_t block, const void *buf, size_t size)
{
  if(block == EDFS_BLOCK_INVALID)
    return -EINVAL;

  if(size > img->sb.block_size)
    size = img->sb.block_size;

  off_t offset = edfs_get_block_offset(&img->sb, block);
  return pwrite(img->fd, buf, size, offset);
}

static bool
edfs_is_block_free(edfs_image_t *img, edfs_block_t block)
{
  if(block >= img->sb.n_blocks)
    return false;

  uint32_t byte_offset = img->sb.bitmap_start + (block / 8);
  uint8_t bit_mask = 1 << (block % 8);

  uint8_t byte;
  if(pread(img->fd, &byte, 1, byte_offset) != 1)
    return false;

  return (byte & bit_mask) == 0;
}

static int
edfs_set_block_used(edfs_image_t *img, edfs_block_t block, bool used)
{
  if(block >= img->sb.n_blocks)
    return -EINVAL;

  uint32_t byte_offset = img->sb.bitmap_start + (block / 8);
  uint8_t bit_mask = 1 << (block % 8);

  uint8_t byte;
  if(pread(img->fd, &byte, 1, byte_offset) != 1)
    return -EIO;

  if(used)
    byte |= bit_mask;
  else
    byte &= ~bit_mask;

  if(pwrite(img->fd, &byte, 1, byte_offset) != 1)
    return -EIO;

  return 0;
}

static edfs_block_t
edfs_find_free_block(edfs_image_t *img)
{
  for(edfs_block_t block = 1; block < img->sb.n_blocks; block++){
    if(edfs_is_block_free(img, block))
      return block;
  }
  return EDFS_BLOCK_INVALID;
}

static int
edfs_allocate_block(edfs_image_t *img, edfs_block_t *block)
{
  *block = edfs_find_free_block(img);
  if(*block == EDFS_BLOCK_INVALID)
    return -ENOSPC;

  return edfs_set_block_used(img, *block, true);
}

static int
edfs_free_block(edfs_image_t *img, edfs_block_t block)
{
  return edfs_set_block_used(img, block, false);
}

static int
edfs_zero_block(edfs_image_t *img, edfs_block_t block)
{
  char *zeros = calloc(1, img->sb.block_size);
  if(zeros == NULL)
    return -ENOMEM;

  int ret = edfs_write_block(img, block, zeros, img->sb.block_size);
  free(zeros);
  return ret;
}

static int
edfs_add_dir_entry(edfs_image_t *img, edfs_inode_t *dir_inode, const char *filename, edfs_inumber_t inumber)
{
  edfs_dir_entry_t entry = { .inumber = inumber };
  strncpy(entry.filename, filename, EDFS_FILENAME_SIZE - 1);
  entry.filename[EDFS_FILENAME_SIZE - 1] = '\0';

  // Use the first directory data block only.
  // If the directory does not have a block yet, allocate a block.
  edfs_block_t block = dir_inode->inode.blocks[0];
  if(block == EDFS_BLOCK_INVALID){
    int rc = edfs_allocate_block(img, &block);
    if(rc < 0)
      return rc;

    dir_inode->inode.blocks[0] = block;
    if(edfs_zero_block(img, block) < 0)
      return -EIO;
    if(edfs_write_inode(img, dir_inode) < 0)
      return -EIO;
  }

  char *buf = malloc(img->sb.block_size);
  if(buf == NULL)
    return -ENOMEM;

  if(edfs_read_block(img, block, buf, img->sb.block_size) < 0){
    free(buf);
    return -EIO;
  }

  edfs_dir_entry_t *entries = (edfs_dir_entry_t *)buf;
  int n_entries = img->sb.block_size / sizeof(edfs_dir_entry_t);
  for(int i = 0; i < n_entries; i++){
    if(entries[i].inumber == 0){
      entries[i] = entry;
      if(edfs_write_block(img, block, buf, img->sb.block_size) < 0){
        free(buf);
        return -EIO;
      }
      free(buf);
      return 0;
    }
  }

  free(buf);
  return -ENOSPC;
}

static int
edfs_remove_dir_entry(edfs_image_t *img, edfs_inode_t *dir_inode, const char *filename)
{
  edfs_block_t block = dir_inode->inode.blocks[0];
  if(block == EDFS_BLOCK_INVALID)
    return -ENOENT;

  char *buf = malloc(img->sb.block_size);
  if(buf == NULL)
    return -ENOMEM;
  if(edfs_read_block(img, block, buf, img->sb.block_size) < 0){
    free(buf);
    return -EIO;
  }

  edfs_dir_entry_t *entries = (edfs_dir_entry_t *)buf;
  int n_entries = img->sb.block_size / sizeof(edfs_dir_entry_t);
  for(int i = 0; i < n_entries; i++){
    if(entries[i].inumber != 0 && strcmp(entries[i].filename, filename) == 0){
      entries[i].inumber = 0; // Mark as free
      if(edfs_write_block(img, block, buf, img->sb.block_size) < 0){
        free(buf);
        return -EIO;
      }
      free(buf);
      return 0;
    }
  }

  free(buf);
  return -ENOENT;
}

typedef struct {
  bool found_extra;
} edfs_dir_check_t;

static int
edfs_rmdir_check_cb(const edfs_dir_entry_t *entry, void *data)
{
  edfs_dir_check_t *check = data;
  if(strcmp(entry->filename, ".") != 0 && strcmp(entry->filename, "..") != 0){
    check->found_extra = true;
    return 1;
  }
  return 0;
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
    /* TODO: It is a good idea to write a separate function to visit each
     * directory entry, so you can reuse it later on. Consider using a callback
     * mechanism: see "Functiepointers" in the Skillsdoc (in Dutch).
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



/* Find the inode of the directory that contains @path.
 * Returns whether the inode was found.
 *
 * (This function is not used yet, but will be useful for your implementation.)
 */
static bool
edfs_find_parent_inode(edfs_image_t *img,
                       const char *path,
                       edfs_inode_t *inode)
{
  /* Drop trailing slashes */
  size_t len = strlen(path);
  while(len > 0 && path[len - 1] == '/'){
    len--;
  }

  /* Isolate last part */
  const char *last = memrchr(path, '/', len);
  if(last == NULL)
    last = path;

  char *dirname = strndup(path, (last - path));
  if(dirname == NULL)
    return false;

  bool found = edfs_find_inode(img, dirname, inode);
  free(dirname);
  return found;
}

/* Returns the basename (the actual name of the file) of the @path.
 * The return value must be freed.
 *
 * (This function is not used yet, but will be useful for your implementation.)
 */
static char *
edfs_get_basename(const char *path)
{
  /* Drop trailing slashes */
  size_t len = strlen(path);
  while(len > 0 && path[len - 1] == '/'){
    len--;
  }

  /* Isolate last part */
  const char *last = memrchr(path, '/', len);
  if(last != NULL){
    last++;
  }else{
    last = path;
  }
  return strndup(last, len - (last - path));
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
  /* TODO: Implement.
   *
   * Read @size bytes of data from the file at @path starting at @offset and
   * write this to @buf.
   */
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
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  if(edfs_find_inode(img, path, &inode))
    return -EEXIST;

  edfs_inode_t parent_inode;
  char *basename = edfs_get_basename(path);
  if(basename == NULL)
    return -ENOMEM;

  if(!edfs_find_parent_inode(img, path, &parent_inode)){
    free(basename);
    return -ENOENT;
  }

  if(!edfs_inode_is_directory(&parent_inode.inode)){
    free(basename);
    return -ENOTDIR;
  }

  edfs_inode_t new_inode;
  if(edfs_new_inode(img, &new_inode, EDFS_INODE_TYPE_DIRECTORY) < 0){
    free(basename);
    return -ENOSPC;
  }

  edfs_block_t block;
  if(edfs_allocate_block(img, &block) < 0){
    free(basename);
    return -ENOSPC;
  }
  new_inode.inode.blocks[0] = block;
  if(edfs_zero_block(img, block) < 0){
    free(basename);
    return -EIO;
  }

  edfs_dir_entry_t dot = { .inumber = new_inode.inumber };
  strncpy(dot.filename, ".", EDFS_FILENAME_SIZE - 1);
  dot.filename[EDFS_FILENAME_SIZE - 1] = '\0';
  edfs_dir_entry_t dotdot = { .inumber = parent_inode.inumber };
  strncpy(dotdot.filename, "..", EDFS_FILENAME_SIZE - 1);
  dotdot.filename[EDFS_FILENAME_SIZE - 1] = '\0';

  char *buf = malloc(img->sb.block_size);
  if(buf == NULL){
    free(basename);
    return -ENOMEM;
  }
  memset(buf, 0, img->sb.block_size);
  memcpy(buf, &dot, sizeof(edfs_dir_entry_t));
  memcpy(buf + sizeof(edfs_dir_entry_t), &dotdot, sizeof(edfs_dir_entry_t));
  if(edfs_write_block(img, block, buf, img->sb.block_size) < 0){
    free(buf);
    free(basename);
    return -EIO;
  }
  free(buf);

  new_inode.inode.size = 2 * sizeof(edfs_dir_entry_t);
  if(edfs_write_inode(img, &new_inode) < 0){
    free(basename);
    return -EIO;
  }

  if(edfs_add_dir_entry(img, &parent_inode, basename, new_inode.inumber) < 0){
    edfs_free_block(img, block);
    edfs_clear_inode(img, &new_inode);
    free(basename);
    return -ENOSPC;
  }

  free(basename);
  return 0;
}

static int
edfuse_rmdir(const char *path)
{
  /* TODO: Implement.
   *
   * Validate @path exists and is an empty directory; remove directory entry
   * from its parent directory; release allocated blocks; release inode.
   */
  edfs_image_t *img = get_edfs_image();
  edfs_inode_t inode = {0,};

  if(strcmp(path, "/") == 0)
    return -EINVAL;

  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;
  if(!edfs_inode_is_directory(&inode.inode))
    return -ENOTDIR;

  /* Verify directory is empty except for "." and "..". */
  edfs_dir_check_t check = { .found_extra = false };
  int rc = edfs_visit_directory_entries(img, &inode.inode,
                                        edfs_rmdir_check_cb, &check);
  if(rc == 1 || check.found_extra)
    return -ENOTEMPTY;
  if(rc < 0)
    return -EIO;

  char *basename = edfs_get_basename(path);
  if(basename == NULL)
    return -ENOMEM;

  edfs_inode_t parent_inode;
  if(!edfs_find_parent_inode(img, path, &parent_inode)){
    free(basename);
    return -ENOENT;
  }

  for(int i = 0; i < EDFS_INODE_N_BLOCKS; i++){
    if(inode.inode.blocks[i] != EDFS_BLOCK_INVALID){
      edfs_free_block(img, inode.inode.blocks[i]);
      inode.inode.blocks[i] = EDFS_BLOCK_INVALID;
    }
  }

  if(edfs_remove_dir_entry(img, &parent_inode, basename) < 0){
    free(basename);
    return -ENOENT;
  }

  if(edfs_clear_inode(img, &inode) < 0){
    free(basename);
    return -EIO;
  }

  free(basename);
  return 0;
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


  // issue : the func may leave orphaned/partial state when adding the directory entry fails
  // but it seems like the assignment does not require handling this case, so we will ignore it for now
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  // Turns out we need to check if the file already exists
  if(edfs_find_inode(img, path, &inode))
    return -EEXIST;

  // Find parent directory
  edfs_inode_t parent_inode;
  char *basename = edfs_get_basename(path);
  if(basename == NULL)
    return -ENOMEM;

  if(!edfs_find_parent_inode(img, path, &parent_inode)){
    free(basename);
    return -ENOENT;
  }

  if(!edfs_inode_is_directory(&parent_inode.inode)){
    free(basename);
    return -ENOTDIR;
  }

  // Create new inode for file
  edfs_inode_t new_inode;
  if(edfs_new_inode(img, &new_inode, EDFS_INODE_TYPE_FILE) < 0){
    free(basename);
    return -ENOSPC;
  }

  // Write the new inode to disk
  if(edfs_write_inode(img, &new_inode) < 0){
    free(basename);
    return -EIO;
  }

  // Add directory entry to parent
  if(edfs_add_dir_entry(img, &parent_inode, basename, new_inode.inumber) < 0){
    edfs_clear_inode(img, &new_inode);
    free(basename);
    return -ENOSPC;
  }

  free(basename);
  return 0;
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
  edfs_image_t *img = get_edfs_image();
  edfs_inode_t inode = {0,};

  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;
  if(edfs_inode_is_directory(&inode.inode))
    return -EISDIR;
  if(size == 0)
    return 0;

  off_t new_size = offset + size;
  if(new_size > inode.inode.size)
    inode.inode.size = new_size;

  uint32_t block_size = img->sb.block_size;
  unsigned int start_block = offset / block_size;
  unsigned int end_block = (offset + size - 1) / block_size;

  size_t written = 0;
  for(unsigned int i = start_block; i <= end_block && written < size; i++){
    edfs_block_t block = EDFS_BLOCK_INVALID;
    if(i < EDFS_INODE_N_BLOCKS){
      block = inode.inode.blocks[i];
      if(block == EDFS_BLOCK_INVALID){
        int rc = edfs_allocate_block(img, &block);
        if(rc < 0)
          return rc;
        inode.inode.blocks[i] = block;
        if(edfs_zero_block(img, block) < 0)
          return -EIO;
      }
    } else {
      return -ENOSPC;
    }

    off_t block_offset = (i == start_block) ? (offset % block_size) : 0;
    size_t to_write = block_size - block_offset;
    if(to_write > size - written)
      to_write = size - written;

    char *block_buf = malloc(block_size);
    if(block_buf == NULL)
      return -ENOMEM;
    if(edfs_read_block(img, block, block_buf, block_size) < 0){
      free(block_buf);
      return -EIO;
    }
    memcpy(block_buf + block_offset, buf + written, to_write);
    if(edfs_write_block(img, block, block_buf, block_size) < 0){
      free(block_buf);
      return -EIO;
    }
    free(block_buf);

    written += to_write;
  }

  if(edfs_write_inode(img, &inode) < 0)
    return -EIO;

  return written;
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
  edfs_image_t *img = get_edfs_image();
  edfs_inode_t inode = {0,};

  if(!edfs_find_inode(img, path, &inode))
    return -ENOENT;
  if(edfs_inode_is_directory(&inode.inode))
    return -EISDIR;
  if(offset < 0)
    return -EINVAL;

  uint32_t block_size = img->sb.block_size;
  unsigned int new_blocks = 0;
  if(offset > 0)
    new_blocks = (offset + block_size - 1) / block_size;
  unsigned int current_blocks = 0;
  if(inode.inode.size > 0)
    current_blocks = (inode.inode.size + block_size - 1) / block_size;

  if(new_blocks > EDFS_INODE_N_BLOCKS)
    return -ENOSPC;

  if(new_blocks > current_blocks){
    for(unsigned int i = current_blocks; i < new_blocks; i++){
      if(inode.inode.blocks[i] == EDFS_BLOCK_INVALID){
        edfs_block_t block;
        int rc = edfs_allocate_block(img, &block);
        if(rc < 0)
          return rc;
        inode.inode.blocks[i] = block;
        if(edfs_zero_block(img, block) < 0)
          return -EIO;
      }
    }
  } else if(new_blocks < current_blocks){
    for(unsigned int i = new_blocks; i < current_blocks; i++){
      if(inode.inode.blocks[i] != EDFS_BLOCK_INVALID){
        edfs_free_block(img, inode.inode.blocks[i]);
        inode.inode.blocks[i] = EDFS_BLOCK_INVALID;
      }
    }
  }

  inode.inode.size = offset;
  if(edfs_write_inode(img, &inode) < 0)
    return -EIO;

  return 0;
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
