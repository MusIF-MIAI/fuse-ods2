/*
 * ods2_lookup.h - FUSE-free helpers shared by lookup.c, ops.c and the
 * (future) read-path implementation.  By keeping these declarations in
 * a header that does NOT pull in <fuse.h>, lookup.c can be compiled
 * even on hosts that lack libfuse3 (e.g. a developer machine without
 * macFUSE).
 */

#ifndef FUSE_ODS2_LOOKUP_H
#define FUSE_ODS2_LOOKUP_H

#include <sys/stat.h>
#include <sys/types.h>

#include "access.h"
#include "f11def.h"
#include "ods2.h"

/* ------------------------------------------------------ runtime options */

struct ods2_runtime {
    int    allversions;
    int    lower;
    int    textmode;
    int    debug;
    int    force_uid;
    int    force_gid;
    uid_t  uid;
    gid_t  gid;
};

extern struct ods2_runtime ods2_rt;
extern struct VCB         *ods2_vcb;

/* ------------------------------------------------------ helpers */

vmscond_t ods2_path_to_fid( const char *path, struct fiddef *out_fid,
                            int *out_is_dir );

void   ods2_head_to_stat( const struct HEAD *head, struct stat *st );
time_t ods2_vmstime_to_time_t( const VMSTIME t );
ino_t  ods2_fid_to_inode( const struct fiddef *fid );

typedef int (*ods2_diriter_cb)( const char *name, int namelen,
                                int version,
                                const struct fiddef *fid, void *ctx );

vmscond_t ods2_iterate_dir( struct FCB *dir_fcb, int latest_only,
                            ods2_diriter_cb cb, void *ctx );

#endif /* FUSE_ODS2_LOOKUP_H */
