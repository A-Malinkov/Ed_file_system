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
  /* TODO: Implement.
   *
   * Read @size bytes of data from the file at @path starting at @offset and
   * write this to @buf.
   */
  return -ENOSYS;
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
