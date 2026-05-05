/*
 * ops.c - FUSE operation implementations.
 *
 * Read-only implementations of getattr and readdir on top of the
 * helpers in lookup.c.  open/read land in phase 3.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ods2_ops.h"
#include "recfmt.h"

#include "access.h"
#include "device.h"
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

    /* In textmode the on-disk size is wrong (record headers, padding,
     * implicit LFs).  Open the file briefly to compute the decoded
     * length; the result is cached, so subsequent stat/read are cheap.
     */
    if( ods2_rt.textmode && S_ISREG( st->st_mode )
        && recfmt_textmode_eligible( head ) ) {
        deaccesshead( vioc, NULL, 0 );

        struct FCB *fcb = NULL;
        if( $SUCCESSFUL( accessfile( ods2_vcb, &fid, &fcb, 0 ) ) ) {
            ssize_t lsize = recfmt_logical_size( fcb );
            if( lsize >= 0 )
                st->st_size = (off_t)lsize;
            deaccessfile( fcb );
        }
        return 0;
    }

    deaccesshead( vioc, NULL, 0 );
    return 0;
}

/* ------------------------------------------------------ readdir */

struct readdir_ctx {
    void                  *buf;
    fuse_fill_dir_t        filler;
};

static int has_dir_suffix( const char *raw, int rawlen ) {
    return rawlen > 4 && memcmp( raw + rawlen - 4, ".DIR", 4 ) == 0;
}

static void emit_name( const char *raw, int rawlen, int version, int is_dir,
                       int allversions, int lower, char *out, size_t outlen ) {
    /* "raw" is uppercase on disk, NOT NUL-terminated. */
    size_t i = 0;
    int    has_dot = 0;
    /* Strip a trailing ";<digits>" if it accidentally appears in the
     * raw name -- on disk the directory record name is just "FOO.BAR"
     * but be defensive. */
    int eff = rawlen;
    for( int k = 0; k < rawlen; ++k )
        if( raw[k] == ';' ) { eff = k; break; }
    if( is_dir && has_dir_suffix( raw, eff ) )
        eff -= 4;

    for( int k = 0; k < eff && i + 1 < outlen; ++k ) {
        unsigned char c = (unsigned char)raw[k];
        if( c == '.' ) has_dot = 1;
        out[i++] = lower ? (char)tolower(c) : (char)c;
    }
    if( !is_dir && !has_dot && i + 1 < outlen )
        out[i++] = '.';                 /* canonical "NAME." form */

    if( !is_dir && allversions && i + 16 < outlen ) {
        i += (size_t)snprintf( out + i, outlen - i, ";%d", version );
    }
    out[i] = '\0';
}

static int readdir_cb( const char *name, int namelen, int version,
                       const struct fiddef *fid, void *ctx ) {
    struct readdir_ctx *r = ctx;
    char display[80];
    struct stat st;
    int is_dir = 0;

    struct VIOC *vioc = NULL;
    struct HEAD *head = NULL;
    uint32_t idx = 0;
    if( $SUCCESSFUL( accesshead( ods2_vcb, (struct fiddef *)fid, 0,
                                 &vioc, &head, &idx, 0 ) ) ) {
        is_dir = (F11LONG( head->fh2$l_filechar ) & FH2$M_DIRECTORY) != 0;
        deaccesshead( vioc, NULL, 0 );
    }

    emit_name( name, namelen, version, is_dir,
               ods2_rt.allversions, ods2_rt.lower,
               display, sizeof display );

    if( ods2_rt.debug )
        fprintf( stderr,
                 "fuse-ods2: readdir entry: '%.*s';%d -> '%s'\n",
                 namelen, name, version, display );

    memset( &st, 0, sizeof st );
    st.st_ino = ods2_fid_to_inode( fid );
    /* Hint the file type when HEAD lookup succeeds so common directory
     * walkers do not need an immediate getattr just to identify dirs.
     */
    st.st_mode = is_dir ? S_IFDIR : S_IFREG;

    return r->filler( r->buf, display, &st, 0, 0 );
}

int ods2_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi,
                  enum fuse_readdir_flags flags ) {
    (void)offset; (void)fi; (void)flags;

    struct fiddef fid;
    vmscond_t     sts;

    sts = ods2_path_to_fid( path, &fid, NULL );
    if( !$SUCCESSFUL(sts) ) {
        if( ods2_rt.debug )
            fprintf( stderr,
                     "fuse-ods2: readdir('%s') path_to_fid failed (%08x)\n",
                     path, (unsigned)(sts & STS$M_COND_ID) );
        return -ENOENT;
    }

    if( ods2_rt.debug )
        fprintf( stderr,
                 "fuse-ods2: readdir('%s') fid=(%u,%u,%u,%u)\n",
                 path,
                 (unsigned)F11WORD(fid.fid$w_num),
                 (unsigned)F11WORD(fid.fid$w_seq),
                 (unsigned)fid.fid$b_rvn,
                 (unsigned)fid.fid$b_nmx );

    struct FCB *dfcb = NULL;
    sts = accessfile( ods2_vcb, &fid, &dfcb, 0 );
    if( !$SUCCESSFUL(sts) ) {
        if( ods2_rt.debug )
            fprintf( stderr,
                     "fuse-ods2: readdir accessfile failed (%08x)\n",
                     (unsigned)(sts & STS$M_COND_ID) );
        return -EIO;
    }

    if( ods2_rt.debug ) {
        struct HEAD *h = dfcb->head;
        fprintf( stderr,
                 "fuse-ods2: readdir filechar=%08x efblk=%u hiblk=%u ffb=%u "
                 "rtype=0x%02x rattr=0x%02x\n",
                 (unsigned)F11LONG(h->fh2$l_filechar),
                 (unsigned)F11SWAP(h->fh2$w_recattr.fat$l_efblk),
                 (unsigned)F11SWAP(h->fh2$w_recattr.fat$l_hiblk),
                 (unsigned)F11WORD(h->fh2$w_recattr.fat$w_ffbyte),
                 (unsigned)h->fh2$w_recattr.fat$b_rtype,
                 (unsigned)h->fh2$w_recattr.fat$b_rattrib );
    }

    /* Reject non-directories. */
    if( !(F11LONG( dfcb->head->fh2$l_filechar ) & FH2$M_DIRECTORY) ) {
        if( ods2_rt.debug )
            fprintf( stderr,
                     "fuse-ods2: readdir: target is not a directory\n" );
        deaccessfile( dfcb );
        return -ENOTDIR;
    }

    filler( buf, ".",  NULL, 0, 0 );
    filler( buf, "..", NULL, 0, 0 );

    struct readdir_ctx ctx = { .buf = buf, .filler = filler };

    sts = ods2_iterate_dir( dfcb,
                            ods2_rt.allversions ? 0 : 1,
                            readdir_cb, &ctx );

    if( ods2_rt.debug )
        fprintf( stderr,
                 "fuse-ods2: readdir iterate done sts=%08x\n",
                 (unsigned)(sts & STS$M_COND_ID) );

    deaccessfile( dfcb );
    return $SUCCESSFUL(sts) ? 0 : -EIO;
}

/* ------------------------------------------------------ open / read / release */

int ods2_open( const char *path, struct fuse_file_info *fi ) {
    /* Read-only mount: refuse any write or O_TRUNC at the open layer
     * before we even bother resolving the path. */
    if( (fi->flags & O_ACCMODE) != O_RDONLY )
        return -EROFS;
    if( fi->flags & (O_TRUNC | O_APPEND | O_CREAT) )
        return -EROFS;

    struct fiddef fid;
    vmscond_t     sts = ods2_path_to_fid( path, &fid, NULL );
    if( !$SUCCESSFUL(sts) )
        return -ENOENT;

    struct FCB *fcb = NULL;
    sts = accessfile( ods2_vcb, &fid, &fcb, 0 );
    if( !$SUCCESSFUL(sts) ) {
        if( ods2_rt.debug )
            fprintf( stderr,
                     "fuse-ods2: open('%s') accessfile failed (%08x)\n",
                     path, (unsigned)(sts & STS$M_COND_ID) );
        return -EIO;
    }

    if( F11LONG( fcb->head->fh2$l_filechar ) & FH2$M_DIRECTORY ) {
        deaccessfile( fcb );
        return -EISDIR;
    }

    fi->fh = (uint64_t)(uintptr_t)fcb;
    fi->keep_cache = 1;
    return 0;
}

int ods2_release( const char *path, struct fuse_file_info *fi ) {
    (void)path;
    struct FCB *fcb = (struct FCB *)(uintptr_t)fi->fh;
    if( fcb != NULL )
        deaccessfile( fcb );
    fi->fh = 0;
    return 0;
}

int ods2_read( const char *path, char *out, size_t size, off_t offset,
               struct fuse_file_info *fi ) {
    (void)path;

    struct FCB *fcb = (struct FCB *)(uintptr_t)fi->fh;
    if( fcb == NULL )
        return -EBADF;

    /* In textmode redirect through the record-format decoder; it has
     * its own size accounting and cached buffer. */
    if( ods2_rt.textmode && fcb->head != NULL
        && recfmt_textmode_eligible( fcb->head ) ) {
        ssize_t n = recfmt_read( fcb, offset, out, size );
        if( n < 0 ) return -EIO;
        return (int)n;
    }

    /* Logical end of file derived from the RECATTR.  EOF block is the
     * first block past the last block holding data; ffb is the count
     * of bytes used in the last block.  When efblk == 0 the file has
     * no data at all. */
    uint32_t efblk = F11SWAP( fcb->head->fh2$w_recattr.fat$l_efblk );
    uint16_t ffb   = F11WORD( fcb->head->fh2$w_recattr.fat$w_ffbyte );
    off_t    fsize = (efblk == 0) ? 0
                                  : (off_t)(efblk - 1) * 512 + ffb;

    if( offset < 0 )
        return -EINVAL;
    if( offset >= fsize )
        return 0;
    if( (off_t)size > fsize - offset )
        size = (size_t)(fsize - offset);
    if( size == 0 )
        return 0;

    size_t  done = 0;
    off_t   pos  = offset;

    while( done < size ) {
        uint32_t vbn        = (uint32_t)(pos / 512) + 1;
        uint32_t block_off  = (uint32_t)(pos % 512);

        struct VIOC *vioc   = NULL;
        char        *buf    = NULL;
        uint32_t     blocks = 0;
        vmscond_t    sts;

        sts = accesschunk( fcb, vbn, &vioc, &buf, &blocks, 0 );
        if( !$SUCCESSFUL(sts) ) {
            if( ods2_rt.debug )
                fprintf( stderr,
                         "fuse-ods2: read accesschunk vbn=%u failed (%08x)\n",
                         (unsigned)vbn, (unsigned)(sts & STS$M_COND_ID) );
            return done > 0 ? (int)done : -EIO;
        }
        if( blocks == 0 )
            blocks = 1;     /* defensive: should be at least 1 */

        size_t avail = (size_t)blocks * 512;
        if( avail <= block_off ) {
            deaccesschunk( vioc, 0, 0, 1 );
            break;
        }
        avail -= block_off;

        size_t want = size - done;
        size_t n    = avail < want ? avail : want;

        memcpy( out + done, buf + block_off, n );

        deaccesschunk( vioc, 0, 0, 1 );

        done += n;
        pos  += (off_t)n;
    }

    return (int)done;
}

/* ------------------------------------------------------ statfs / readlink */

int ods2_statfs( const char *path, struct statvfs *st ) {
    (void)path;
    if( ods2_vcb == NULL || ods2_vcb->devices == 0 )
        return -ENXIO;

    /* Sum block counts across the volume set.  ODS-2 cluster size and
     * device size live in the home block of each VCBDEV; total blocks
     * is the device size in 512-byte units.  Free blocks would need
     * the storage bitmap (write-side accounting) so we report 0. */
    fsblkcnt_t total_blocks = 0;
    unsigned   cluster_blocks = 0;

    for( unsigned i = 0; i < ods2_vcb->devices; ++i ) {
        const struct VCBDEV *d = &ods2_vcb->vcbdev[i];
        unsigned c = (unsigned)F11WORD( d->home.hm2$w_cluster );
        if( c == 0 ) c = 1;
        if( cluster_blocks == 0 ) cluster_blocks = c;
        if( d->dev != NULL )
            total_blocks += (fsblkcnt_t)( d->dev->eofptr / 512 );
    }
    if( cluster_blocks == 0 ) cluster_blocks = 1;

    memset( st, 0, sizeof *st );
    st->f_bsize   = 512;
    st->f_frsize  = 512;
    st->f_blocks  = total_blocks;
    st->f_bfree   = 0;          /* read-only mount; bitmap not parsed */
    st->f_bavail  = 0;
    st->f_files   = 0;
    st->f_ffree   = 0;
    st->f_namemax = 80;         /* VMS file spec including version    */
    return 0;
}

int ods2_readlink( const char *path, char *buf, size_t size ) {
    (void)path; (void)buf; (void)size;
    /* ODS-2 has no symbolic links. */
    return -EINVAL;
}
