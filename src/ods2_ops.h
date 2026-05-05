/*
 * ods2_ops.h - FUSE-using declarations.  Includes <fuse.h>; only ops.c
 * and fuse_ods2.c need this, lookup.c uses ods2_lookup.h instead.
 */

#ifndef FUSE_ODS2_OPS_H
#define FUSE_ODS2_OPS_H

#define FUSE_USE_VERSION 31

#include <fuse.h>

#include "ods2_lookup.h"

int ods2_getattr( const char *path, struct stat *st,
                  struct fuse_file_info *fi );
int ods2_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi,
                  enum fuse_readdir_flags flags );
int ods2_open   ( const char *path, struct fuse_file_info *fi );
int ods2_read   ( const char *path, char *buf, size_t size, off_t off,
                  struct fuse_file_info *fi );
int ods2_release( const char *path, struct fuse_file_info *fi );

#endif /* FUSE_ODS2_OPS_H */
