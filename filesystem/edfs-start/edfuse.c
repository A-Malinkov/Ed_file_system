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

    /* Check if current inode is a directory */
    if (!edfs_inode_is_directory(&current_inode.inode)) {
      return false;
    }

    /* Read directory entries from the inode blocks */
    for (int i = 0; i < EDFS_INODE_N_BLOCKS && current_inode.inode.blocks[i] != EDFS_BLOCK_INVALID; i++) {
      uint32_t block_offset = edfs_get_block_offset(&img->sb, current_inode.inode.blocks[i]);
      int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

      /* Allocate buffer for one block of directory entries */
      edfs_dir_entry_t *entries = malloc(img->sb.block_size);
      if (entries == NULL) {
        return false;
      }

      /* Read the block */
      ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
      if (bytes_read != img->sb.block_size) {
        free(entries);
        return false;
      }

      /* Search through directory entries */
      for (int j = 0; j < n_entries; j++) {
        if (!edfs_dir_entry_is_empty(&entries[j]) &&
            strcmp(entries[j].filename, direntry.filename) == 0) {
          direntry.inumber = entries[j].inumber;
          found = true;
          break;
        }
      }

      free(entries);

      if (found) {
        break;
      }
    }

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

  /* Read directory entries from the inode's blocks */
  for (int i = 0; i < EDFS_INODE_N_BLOCKS && inode.inode.blocks[i] != EDFS_BLOCK_INVALID; i++) {
    uint32_t block_offset = edfs_get_block_offset(&img->sb, inode.inode.blocks[i]);
    int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

    /* Allocate buffer for one block of directory entries */
    edfs_dir_entry_t *entries = malloc(img->sb.block_size);
    if (entries == NULL) {
      return -ENOMEM;
    }

    /* Read the block */
    ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
    if (bytes_read != img->sb.block_size) {
      free(entries);
      return -EIO;
    }

    /* Add all valid directory entries to the listing */
    for (int j = 0; j < n_entries; j++) {
      if (!edfs_dir_entry_is_empty(&entries[j])) {
        filler(buf, entries[j].filename, NULL, 0);
      }
    }

    free(entries);
  }

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

  edfs_inode_t existing_inode;
  if (edfs_find_inode(img, path, &existing_inode)) {
    return -EEXIST;
  }

  edfs_inode_t parent_inode;
  if (!edfs_find_parent_inode(img, path, &parent_inode)) {
    return -ENOENT;
  }

  char *basename = edfs_get_basename(path);
  if (basename == NULL) {
    return -ENOMEM;
  }

  edfs_inode_t new_inode;
  if (edfs_new_inode(img, &new_inode, EDFS_INODE_TYPE_DIRECTORY) != 0) {
    free(basename);
    return -ENOSPC;
  }

  if (edfs_write_inode(img, &new_inode) != 0) {
    edfs_clear_inode(img, &new_inode);
    free(basename);
    return -EIO;
  }

  bool added = false;
  for (int i = 0; i < EDFS_INODE_N_BLOCKS && parent_inode.inode.blocks[i] != EDFS_BLOCK_INVALID && !added; i++) {
    uint32_t block_offset = edfs_get_block_offset(&img->sb, parent_inode.inode.blocks[i]);
    int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

    edfs_dir_entry_t *entries = malloc(img->sb.block_size);
    if (entries == NULL) {
      edfs_clear_inode(img, &new_inode);
      free(basename);
      return -ENOMEM;
    }

    ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
    if (bytes_read != img->sb.block_size) {
      free(entries);
      edfs_clear_inode(img, &new_inode);
      free(basename);
      return -EIO;
    }

    for (int j = 0; j < n_entries && !added; j++) {
      if (edfs_dir_entry_is_empty(&entries[j])) {
        entries[j].inumber = new_inode.inumber;
        strncpy(entries[j].filename, basename, EDFS_FILENAME_SIZE);
        entries[j].filename[EDFS_FILENAME_SIZE - 1] = '\0';
        added = true;
      }
    }

    if (added) {
      ssize_t bytes_written = pwrite(img->fd, entries, img->sb.block_size, block_offset);
      if (bytes_written != img->sb.block_size) {
        free(entries);
        edfs_clear_inode(img, &new_inode);
        free(basename);
        return -EIO;
      }
    }

    free(entries);
  }

  free(basename);
  if (!added) {
    edfs_clear_inode(img, &new_inode);
    return -ENOSPC;
  }
  return 0;
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
  edfs_image_t *img = get_edfs_image();

  if (strcmp(path, "/") == 0) {
    return -EBUSY;
  }

  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode)) {
    return -ENOENT;
  }

  if (!edfs_inode_is_directory(&inode.inode)) {
    return -ENOTDIR;
  }

  bool is_empty = true;
  for (int i = 0; i < EDFS_INODE_N_BLOCKS && inode.inode.blocks[i] != EDFS_BLOCK_INVALID && is_empty; i++) {
    uint32_t block_offset = edfs_get_block_offset(&img->sb, inode.inode.blocks[i]);
    int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

    edfs_dir_entry_t *entries = malloc(img->sb.block_size);
    if (entries == NULL) {
      return -ENOMEM;
    }

    ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
    if (bytes_read != img->sb.block_size) {
      free(entries);
      return -EIO;
    }

    for (int j = 0; j < n_entries; j++) {
      if (!edfs_dir_entry_is_empty(&entries[j])) {
        if (strcmp(entries[j].filename, ".") != 0 && strcmp(entries[j].filename, "..") != 0) {
          is_empty = false;
          break;
        }
      }
    }

    free(entries);
  }

  if (!is_empty) {
    return -ENOTEMPTY;
  }

  edfs_inode_t parent_inode;
  if (!edfs_find_parent_inode(img, path, &parent_inode)) {
    return -ENOENT;
  }

  char *basename = edfs_get_basename(path);
  if (basename == NULL) {
    return -ENOMEM;
  }

  bool removed = false;
  for (int i = 0; i < EDFS_INODE_N_BLOCKS && parent_inode.inode.blocks[i] != EDFS_BLOCK_INVALID && !removed; i++) {
    uint32_t block_offset = edfs_get_block_offset(&img->sb, parent_inode.inode.blocks[i]);
    int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

    edfs_dir_entry_t *entries = malloc(img->sb.block_size);
    if (entries == NULL) {
      free(basename);
      return -ENOMEM;
    }

    ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
    if (bytes_read != img->sb.block_size) {
      free(entries);
      free(basename);
      return -EIO;
    }

    for (int j = 0; j < n_entries && !removed; j++) {
      if (!edfs_dir_entry_is_empty(&entries[j]) &&
          strcmp(entries[j].filename, basename) == 0 &&
          entries[j].inumber == inode.inumber) {
        memset(&entries[j], 0, sizeof(edfs_dir_entry_t));
        removed = true;
      }
    }

    if (removed) {
      ssize_t bytes_written = pwrite(img->fd, entries, img->sb.block_size, block_offset);
      if (bytes_written != img->sb.block_size) {
        free(entries);
        free(basename);
        return -EIO;
      }
    }

    free(entries);
  }

  free(basename);

  if (!removed) {
    return -ENOENT;
  }

  for (int i = 0; i < EDFS_INODE_N_BLOCKS; i++) {
    if (inode.inode.blocks[i] != EDFS_BLOCK_INVALID) {
      edfs_free_block(img, inode.inode.blocks[i]);
    }
  }

  edfs_clear_inode(img, &inode);

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
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t existing_inode;
  if (edfs_find_inode(img, path, &existing_inode)) {
    return -EEXIST;
  }

  edfs_inode_t parent_inode;
  if (!edfs_find_parent_inode(img, path, &parent_inode)) {
    return -ENOENT;
  }

  char *basename = edfs_get_basename(path);
  if (basename == NULL) {
    return -ENOMEM;
  }

  edfs_inode_t new_inode;
  if (edfs_new_inode(img, &new_inode, EDFS_INODE_TYPE_FILE) != 0) {
    free(basename);
    return -ENOSPC;
  }

  if (edfs_write_inode(img, &new_inode) != 0) {
    edfs_clear_inode(img, &new_inode);
    free(basename);
    return -EIO;
  }

  bool added = false;
  for (int i = 0; i < EDFS_INODE_N_BLOCKS && parent_inode.inode.blocks[i] != EDFS_BLOCK_INVALID && !added; i++) {
    uint32_t block_offset = edfs_get_block_offset(&img->sb, parent_inode.inode.blocks[i]);
    int n_entries = edfs_get_n_dir_entries_per_block(&img->sb);

    edfs_dir_entry_t *entries = malloc(img->sb.block_size);
    if (entries == NULL) {
      edfs_clear_inode(img, &new_inode);
      free(basename);
      return -ENOMEM;
    }

    ssize_t bytes_read = pread(img->fd, entries, img->sb.block_size, block_offset);
    if (bytes_read != img->sb.block_size) {
      free(entries);
      edfs_clear_inode(img, &new_inode);
      free(basename);
      return -EIO;
    }

    for (int j = 0; j < n_entries && !added; j++) {
      if (edfs_dir_entry_is_empty(&entries[j])) {
        entries[j].inumber = new_inode.inumber;
        strncpy(entries[j].filename, basename, EDFS_FILENAME_SIZE);
        entries[j].filename[EDFS_FILENAME_SIZE - 1] = '\0';
        added = true;
      }
    }

    if (added) {
      ssize_t bytes_written = pwrite(img->fd, entries, img->sb.block_size, block_offset);
      if (bytes_written != img->sb.block_size) {
        free(entries);
        edfs_clear_inode(img, &new_inode);
        free(basename);
        return -EIO;
      }
    }

    free(entries);
  }

  free(basename);
  if (!added) {
    edfs_clear_inode(img, &new_inode);
    return -ENOSPC;
  }
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
  /* TODO: Implement.
   *
   * Read @size bytes of data from the file at @path starting at @offset and
   * write this to @buf.
   */
  edfs_image_t *img = get_edfs_image();

  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode)) {
    return -ENOENT;
  }

  if (edfs_inode_is_directory(&inode.inode)) {
    return -EISDIR;
  }

  if (offset >= inode.inode.size) {
    return 0;
  }

  if (offset + size > inode.inode.size) {
    size = inode.inode.size - offset;
  }

  size_t bytes_read = 0;
  size_t remaining = size;
  off_t current_offset = offset;

  for (int i = 0; i < EDFS_INODE_N_BLOCKS && inode.inode.blocks[i] != EDFS_BLOCK_INVALID && remaining > 0; i++) {
    uint32_t block_start = i * img->sb.block_size;
    uint32_t block_end = (i + 1) * img->sb.block_size;

    if (current_offset < block_end && current_offset + remaining > block_start) {
      off_t block_offset = edfs_get_block_offset(&img->sb, inode.inode.blocks[i]);
      off_t read_start = block_offset + (current_offset - block_start);
      size_t read_size = img->sb.block_size - (current_offset - block_start);

      if (read_size > remaining) {
        read_size = remaining;
      }

      ssize_t result = pread(img->fd, buf + bytes_read, read_size, read_start);
      if (result < 0) {
        return -EIO;
      }
      if (result == 0) {
        break;
      }

      bytes_read += result;
      remaining -= result;
      current_offset += result;
    }
  }

  return bytes_read;
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

  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode)) {
    return -ENOENT;
  }

  if (edfs_inode_is_directory(&inode.inode)) {
    return -EISDIR;
  }

  off_t new_size = offset + size;
  if (new_size > inode.inode.size) {
    inode.inode.size = new_size;
  }

  size_t blocks_needed = (new_size + img->sb.block_size - 1) / img->sb.block_size;
  if (blocks_needed > EDFS_INODE_N_BLOCKS) {
    return -EFBIG;
  }

  for (size_t i = 0; i < blocks_needed; i++) {
    if (inode.inode.blocks[i] == EDFS_BLOCK_INVALID) {
      edfs_block_t new_block = edfs_new_block(img);
      if (new_block == EDFS_BLOCK_INVALID) {
        if (edfs_write_inode(img, &inode) != 0) {
          return -EIO;
        }
        return -ENOSPC;
      }
      inode.inode.blocks[i] = new_block;
    }
  }

  size_t bytes_written = 0;
  size_t remaining = size;
  off_t current_offset = offset;

  for (int i = 0; i < EDFS_INODE_N_BLOCKS && inode.inode.blocks[i] != EDFS_BLOCK_INVALID && remaining > 0; i++) {
    uint32_t block_start = i * img->sb.block_size;
    uint32_t block_end = (i + 1) * img->sb.block_size;

    if (current_offset < block_end && current_offset + remaining > block_start) {
      off_t block_offset = edfs_get_block_offset(&img->sb, inode.inode.blocks[i]);
      off_t write_start = block_offset + (current_offset - block_start);
      size_t write_size = img->sb.block_size - (current_offset - block_start);

      if (write_size > remaining) {
        write_size = remaining;
      }

      ssize_t result = pwrite(img->fd, buf + bytes_written, write_size, write_start);
      if (result < 0) {
        if (edfs_write_inode(img, &inode) != 0) {
          return -EIO;
        }
        return -EIO;
      }

      bytes_written += result;
      remaining -= result;
      current_offset += result;
    }
  }

  if (edfs_write_inode(img, &inode) != 0) {
    return -EIO;
  }

  return bytes_written;
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

  /* Find the file inode */
  edfs_inode_t inode;
  if (!edfs_find_inode(img, path, &inode)) {
    return -ENOENT;
  }

  if (edfs_inode_is_directory(&inode.inode)) {
    return -EISDIR;
  }

  off_t old_size = inode.inode.size;
  inode.inode.size = offset;

  if (offset > old_size) {
    /* Growing: allocate new blocks if needed */
    size_t blocks_needed = (offset + img->sb.block_size - 1) / img->sb.block_size;
    if (blocks_needed > EDFS_INODE_N_BLOCKS) {
      inode.inode.size = old_size; /* Restore original size */
      return -EFBIG;
    }

    /* Allocate missing blocks */
    for (size_t i = 0; i < blocks_needed; i++) {
      if (inode.inode.blocks[i] == EDFS_BLOCK_INVALID) {
        edfs_block_t new_block = edfs_new_block(img);
        if (new_block == EDFS_BLOCK_INVALID) {
          inode.inode.size = old_size; /* Restore original size */
          return -ENOSPC;
        }
        inode.inode.blocks[i] = new_block;
      }
    }
  } else if (offset < old_size) {
    /* Shrinking: free unused blocks */
    size_t blocks_needed = (offset + img->sb.block_size - 1) / img->sb.block_size;
    for (size_t i = blocks_needed; i < EDFS_INODE_N_BLOCKS; i++) {
      if (inode.inode.blocks[i] != EDFS_BLOCK_INVALID) {
        edfs_free_block(img, inode.inode.blocks[i]);
        inode.inode.blocks[i] = EDFS_BLOCK_INVALID;
      }
    }

    /* Zero out the truncated portion of the last block */
    if (blocks_needed > 0 && offset > 0) {
      size_t last_block_idx = (offset - 1) / img->sb.block_size;
      if (last_block_idx < EDFS_INODE_N_BLOCKS && inode.inode.blocks[last_block_idx] != EDFS_BLOCK_INVALID) {
        off_t block_offset = edfs_get_block_offset(&img->sb, inode.inode.blocks[last_block_idx]);
        size_t offset_in_block = offset % img->sb.block_size;
        if (offset_in_block > 0) {
          /* Zero from offset_in_block to end of block */
          void *zero_buffer = calloc(1, img->sb.block_size - offset_in_block);
          if (zero_buffer != NULL) {
            pwrite(img->fd, zero_buffer, img->sb.block_size - offset_in_block,
                   block_offset + offset_in_block);
            free(zero_buffer);
          }
        }
      }
    }
  }

  /* Write updated inode */
  if (edfs_write_inode(img, &inode) != 0) {
    return -EIO;
  }

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
