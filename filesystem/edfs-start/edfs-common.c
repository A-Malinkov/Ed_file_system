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
