/*
 * ops.c - FUSE operation implementations.
 *
 * Read-only implementations of getattr and readdir on top of the
 * helpers in lookup.c.  open/read land in phase 3.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ods2_ops.h"

#include "access.h"
#include "f11def.h"
#include "ods2.h"
#include "ssdef.h"
#include "stsdef.h"

/* ------------------------------------------------------ getattr */

int ods2_getattr( const char *path, struct stat *st,
                  struct fuse_file_info *fi ) {
    (void)fi;

    if( strcmp( path, "/" ) == 0 ) {
        memset( st, 0, sizeof *st );
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_uid   = ods2_rt.force_uid ? ods2_rt.uid : getuid();
        st->st_gid   = ods2_rt.force_gid ? ods2_rt.gid : getgid();
        st->st_ino   = 4;     /* MFD by convention */
        return 0;
    }

    struct fiddef fid;
    int           is_dir_hint = 0;
    vmscond_t     sts;

    sts = ods2_path_to_fid( path, &fid, &is_dir_hint );
    if( !$SUCCESSFUL(sts) ) {
        if( ods2_rt.debug )
            fprintf( stderr, "fuse-ods2: getattr('%s') -> not found (%08x)\n",
                     path, (unsigned)(sts & STS$M_COND_ID) );
        return -ENOENT;
    }

    struct VIOC *vioc = NULL;
    struct HEAD *head = NULL;
    uint32_t     idx;

    sts = accesshead( ods2_vcb, &fid, 0, &vioc, &head, &idx, 0 );
    if( !$SUCCESSFUL(sts) ) {
        if( ods2_rt.debug )
            fprintf( stderr, "fuse-ods2: getattr('%s') accesshead failed (%08x)\n",
                     path, (unsigned)(sts & STS$M_COND_ID) );
        return -EIO;
    }

    ods2_head_to_stat( head, st );

    /* Override owner/group if not forced. ods2_head_to_stat already
     * honours force_*, but we also fall back to the mounting user
     * when the on-disk UIC is 0 (system) so files don't appear owned
     * by root unexpectedly. */
    if( !ods2_rt.force_uid && st->st_uid == 0 )
        st->st_uid = getuid();
    if( !ods2_rt.force_gid && st->st_gid == 0 )
        st->st_gid = getgid();

    deaccesshead( vioc, NULL, 0 );
    return 0;
}

/* ------------------------------------------------------ readdir */

struct readdir_ctx {
    void                  *buf;
    fuse_fill_dir_t        filler;
};

static void emit_name( const char *raw, int rawlen, int version,
                       int allversions, int lower, char *out, size_t outlen ) {
    /* "raw" is uppercase on disk, NOT NUL-terminated. */
    size_t i = 0;
    int    has_dot = 0;
    int    has_ver = 0;
    /* Strip a trailing ";<digits>" if it accidentally appears in the
     * raw name -- on disk the directory record name is just "FOO.BAR"
     * but be defensive. */
    int eff = rawlen;
    for( int k = 0; k < rawlen; ++k )
        if( raw[k] == ';' ) { eff = k; break; }

    for( int k = 0; k < eff && i + 1 < outlen; ++k ) {
        unsigned char c = (unsigned char)raw[k];
        if( c == '.' ) has_dot = 1;
        out[i++] = lower ? (char)tolower(c) : (char)c;
    }
    if( !has_dot && i + 1 < outlen )
        out[i++] = '.';                 /* canonical "NAME." form */

    if( allversions && i + 16 < outlen ) {
        i += (size_t)snprintf( out + i, outlen - i, ";%d", version );
        has_ver = 1;
        (void)has_ver;
    }
    out[i] = '\0';
}

static int readdir_cb( const char *name, int namelen, int version,
                       const struct fiddef *fid, void *ctx ) {
    struct readdir_ctx *r = ctx;
    char display[80];
    struct stat st;

    emit_name( name, namelen, version,
               ods2_rt.allversions, ods2_rt.lower,
               display, sizeof display );

    memset( &st, 0, sizeof st );
    st.st_ino = ods2_fid_to_inode( fid );
    /* We don't know if it's a directory without reading the HEAD; let
     * the kernel call getattr for that.  But hint a "regular file"
     * type by default so 'ls -l' doesn't issue a stat for every entry
     * just to learn the type.  Directories will get fixed by getattr.
     */
    st.st_mode = S_IFREG;

    r->filler( r->buf, display, &st, 0, 0 );
    return 0;
}

int ods2_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi,
                  enum fuse_readdir_flags flags ) {
    (void)offset; (void)fi; (void)flags;

    struct fiddef fid;
    vmscond_t     sts;

    sts = ods2_path_to_fid( path, &fid, NULL );
    if( !$SUCCESSFUL(sts) )
        return -ENOENT;

    struct FCB *dfcb = NULL;
    sts = accessfile( ods2_vcb, &fid, &dfcb, 0 );
    if( !$SUCCESSFUL(sts) )
        return -EIO;

    /* Reject non-directories. */
    if( !(F11LONG( dfcb->head->fh2$l_filechar ) & FH2$M_DIRECTORY) ) {
        deaccessfile( dfcb );
        return -ENOTDIR;
    }

    filler( buf, ".",  NULL, 0, 0 );
    filler( buf, "..", NULL, 0, 0 );

    struct readdir_ctx ctx = { .buf = buf, .filler = filler };

    sts = ods2_iterate_dir( dfcb,
                            ods2_rt.allversions ? 0 : 1,
                            readdir_cb, &ctx );

    deaccessfile( dfcb );
    return $SUCCESSFUL(sts) ? 0 : -EIO;
}
