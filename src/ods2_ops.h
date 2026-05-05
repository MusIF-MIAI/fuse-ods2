/*
 * Internal interface between fuse_ods2.c and the ODS-2 operation
 * helpers (lookup.c, ops.c).
 */

#ifndef FUSE_ODS2_OPS_H
#define FUSE_ODS2_OPS_H

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "access.h"
#include "f11def.h"

/* ------------------------------------------------------ runtime options */

struct ods2_runtime {
    int    allversions;     /* expose every NAME.EXT;n                */
    int    lower;            /* lowercase names exposed to FUSE        */
    int    textmode;         /* (phase 4)                              */
    int    debug;
    int    force_uid;
    int    force_gid;
    uid_t  uid;
    gid_t  gid;
};

extern struct ods2_runtime ods2_rt;
extern struct VCB         *ods2_vcb;

/* ------------------------------------------------------ FUSE operations */

int ods2_getattr( const char *path, struct stat *st,
                  struct fuse_file_info *fi );
int ods2_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi,
                  enum fuse_readdir_flags flags );

/* ------------------------------------------------------ helpers */

/* Resolve a POSIX-style path ("/", "/A/FILE.TXT", "/A/B/", ...) into
 * an ODS-2 FID.  When *out_dir is true the resolved object is a
 * directory (the caller may use it as a parent for readdir).  When
 * version != NULL it receives the version that matched (0 for "no
 * specific version requested").
 */
vmscond_t ods2_path_to_fid( const char *path, struct fiddef *out_fid,
                            int *out_is_dir );

/* Fill in a struct stat from a HEAD block. */
void ods2_head_to_stat( const struct HEAD *head, struct stat *st );

/* Convert VMSTIME (8 bytes, 100ns ticks since 1858-11-17) to time_t. */
time_t ods2_vmstime_to_time_t( const VMSTIME t );

/* Map an ODS-2 FID to a stable POSIX inode number. */
ino_t ods2_fid_to_inode( const struct fiddef *fid );

/* Internal: iterate a directory file FCB, calling 'cb' for every
 * (name, version, fid) tuple.  When 'latest_only' is true, only the
 * highest version of each name is reported.
 */
typedef int (*ods2_diriter_cb)( const char *name, int namelen,
                                int version,
                                const struct fiddef *fid, void *ctx );

vmscond_t ods2_iterate_dir( struct FCB *dir_fcb, int latest_only,
                            ods2_diriter_cb cb, void *ctx );

#endif /* FUSE_ODS2_OPS_H */
