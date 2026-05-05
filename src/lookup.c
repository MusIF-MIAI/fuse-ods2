/*
 * lookup.c - path resolution and directory iteration helpers.
 *
 * The FUSE wrapper sees POSIX-style paths.  ODS-2 uses VMS-style
 * directory specs (`[A.B.C]NAME.EXT;ver`) and FIDs.  This module
 * bridges the two:
 *
 *   - ods2_path_to_fid()   : POSIX path -> FID
 *   - ods2_iterate_dir()   : enumerate the entries of an open dir FCB
 *   - ods2_head_to_stat()  : on-disk HEAD -> struct stat
 *
 * The directory iterator parses dir$r_rec / dir$r_ent records straight
 * out of the directory blocks fetched via accesschunk(), exactly the
 * way search_ent() does in ods2lib/direct.c (which we re-use only as a
 * reference).  Doing the iteration ourselves keeps the code we have
 * to glue smaller and lets readdir produce one filler() call per
 * (name, version) without dragging in the FIB-driven search machinery.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ods2_lookup.h"

#include "access.h"
#include "descrip.h"
#include "direct.h"
#include "f11def.h"
#include "fibdef.h"
#include "ods2.h"
#include "ssdef.h"
#include "stsdef.h"
#include "vmstime.h"

#ifndef BLOCKSIZE
#define BLOCKSIZE 512
#endif

/* ------------------------------------------------------ time conversion */

/* Days from 1858-11-17 (VMS epoch) to 1970-01-01 (Unix epoch). */
#define VMS_TO_UNIX_DAYS 40587LL

time_t ods2_vmstime_to_time_t( const VMSTIME t ) {
    /* VMSTIME is 64-bit little-endian, units of 100 ns since the
     * Smithsonian epoch 1858-11-17 00:00:00 UTC.
     */
    uint64_t ticks = 0;
    for( int i = 7; i >= 0; --i )
        ticks = (ticks << 8) | (uint8_t)t[i];

    /* Tick(0) is 1858-11-17.  Subtract the offset to 1970-01-01 in
     * units of 100ns.
     */
    int64_t unix_ticks = (int64_t)ticks
        - VMS_TO_UNIX_DAYS * 24LL * 3600LL * 10000000LL;
    if( unix_ticks < 0 )
        return 0;
    return (time_t)(unix_ticks / 10000000LL);
}

/* ------------------------------------------------------ inode mapping */

ino_t ods2_fid_to_inode( const struct fiddef *fid ) {
    /* Pack FID into a 64-bit inode.  Layout, low to high:
     *   16 bits fid$w_num  | 16 bits fid$w_seq | 8 bits fid$b_rvn
     * | 8 bits fid$b_nmx   | 16 bits reserved
     * The FID fields are stored little-endian on disk; we read them
     * via F11WORD() so they're in host byte order here.
     */
    uint64_t v = 0;
    v |= (uint64_t)F11WORD(fid->fid$w_num);
    v |= (uint64_t)F11WORD(fid->fid$w_seq) << 16;
    v |= (uint64_t)fid->fid$b_rvn          << 32;
    v |= (uint64_t)fid->fid$b_nmx          << 40;
    if( v == 0 )
        v = 1;          /* 0 is reserved by some POSIX tools */
    return (ino_t)v;
}

/* ------------------------------------------------------ HEAD -> stat */

static mode_t protbits_to_mode( unsigned prot ) {
    /* prot is the on-disk protection word.  Each 4-bit nibble holds
     * (S, O, G, W) deny-bits: 1=NOREAD, 2=NOWRITE, 4=NOEXE, 8=NODEL.
     * We translate the read/exec bits into POSIX r/x; "no write" is
     * always implied because the mount is read-only.
     */
    mode_t m = 0;
    static const int shift[4] = { prot$v_owner, prot$v_group, prot$v_world, 0 };
    static const mode_t bits[4][2] = {
        { S_IRUSR, S_IXUSR },
        { S_IRGRP, S_IXGRP },
        { S_IROTH, S_IXOTH },
        { 0,       0       },
    };
    /* Owner/group/world only - VMS "system" is folded into owner. */
    for( int i = 0; i < 3; ++i ) {
        unsigned deny = (prot >> shift[i]) & 0xf;
        if( !(deny & prot$m_noread) )
            m |= bits[i][0];
        if( !(deny & prot$m_noexe) )
            m |= bits[i][1];
    }
    /* Always grant owner read so root can stat the file even when the
     * volume was created with restrictive UIC defaults. */
    if( !(m & S_IRUSR) )
        m |= S_IRUSR;
    return m;
}

void ods2_head_to_stat( const struct HEAD *head, struct stat *st ) {
    uint32_t hiblk = F11SWAP( head->fh2$w_recattr.fat$l_hiblk );
    uint32_t efblk = F11SWAP( head->fh2$w_recattr.fat$l_efblk );
    uint16_t ffb   = F11WORD( head->fh2$w_recattr.fat$w_ffbyte );
    int      isdir = (F11LONG(head->fh2$l_filechar) & FH2$M_DIRECTORY) != 0;
    unsigned prot  = F11WORD( head->fh2$w_fileprot );

    memset( st, 0, sizeof *st );

    if( efblk > 0 )
        st->st_size = (off_t)(efblk - 1) * 512 + ffb;
    else
        st->st_size = 0;
    st->st_blocks = (blkcnt_t)hiblk;        /* in 512-byte units      */
    st->st_blksize = 512;

    st->st_mode = protbits_to_mode( prot );
    if( isdir )
        st->st_mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
    else
        st->st_mode |= S_IFREG;

    st->st_nlink = isdir ? 2 : 1;
    st->st_ino   = ods2_fid_to_inode( &head->fh2$w_fid );

    /* Owner UIC (group:member).  The user can override via
     * -o uid=/-o gid=. */
    if( ods2_rt.force_uid )
        st->st_uid = ods2_rt.uid;
    else
        st->st_uid = (uid_t)F11WORD( head->fh2$l_fileowner.uic$w_mem );
    if( ods2_rt.force_gid )
        st->st_gid = ods2_rt.gid;
    else
        st->st_gid = (gid_t)F11WORD( head->fh2$l_fileowner.uic$w_grp );

    /* Times.  IDENT area lives at fh2$b_idoffset (units of words). */
    if( head->fh2$b_idoffset >= FH2$C_LENGTH / 2 ) {
        const struct IDENT *id = (const struct IDENT *)
            ((const uint16_t *)head + head->fh2$b_idoffset);
        st->st_mtime = ods2_vmstime_to_time_t( id->fi2$q_revdate );
        st->st_ctime = ods2_vmstime_to_time_t( id->fi2$q_credate );
        st->st_atime = st->st_mtime;
    }
}

/* ------------------------------------------------------ name helpers */

static void upper_inplace( char *s ) {
    for( ; *s; ++s ) *s = (char)toupper( (unsigned char)*s );
}

/* Split the trailing path component off.  Returns pointers into the
 * given (mutable) buffer; the original string keeps the leading dirs
 * and the function returns a pointer to just after the last '/'.
 */
static char *split_last( char *path ) {
    char *slash = strrchr( path, '/' );
    if( slash == NULL )
        return path;
    *slash = '\0';
    return slash + 1;
}

/* Translate a POSIX dir prefix ("/A/B/C") into the VMS "A.B.C"
 * argument expected by direct_dirid().  Empty input -> empty output
 * (which direct_dirid() treats as MFD).
 */
static void posix_dir_to_vms( const char *posix, char *out, size_t outlen ) {
    size_t o = 0;
    int first = 1;
    if( outlen == 0 ) return;
    for( const char *p = posix; *p && o + 1 < outlen; ) {
        while( *p == '/' ) ++p;
        if( !*p ) break;
        if( !first && o + 1 < outlen )
            out[o++] = '.';
        first = 0;
        while( *p && *p != '/' && o + 1 < outlen )
            out[o++] = (char)toupper( (unsigned char)*p++ );
    }
    out[o] = '\0';
}

/* Parse "FILE.EXT" or "FILE.EXT;n" into (name, version).  When no
 * version is given, *version is set to 0 (caller treats as "highest").
 * The buffer is assumed to be a writable copy of the file component.
 */
static int parse_file_version( char *namebuf, int *version ) {
    char *semi = strchr( namebuf, ';' );
    if( semi == NULL ) {
        *version = 0;
        return 1;
    }
    *semi = '\0';
    if( semi[1] == '\0' ) {
        *version = 0;
        return 1;
    }

    unsigned v = 0;
    for( char *p = semi + 1; *p != '\0'; ++p ) {
        if( !isdigit( (unsigned char)*p ) )
            return 0;
        v = v * 10u + (unsigned)(*p - '0');
        if( v > 65535u )
            return 0;
    }
    *version = (int)v;
    return 1;
}

static int name_has_dir_suffix( const char *name ) {
    size_t len = strlen( name );
    return len >= 4 && memcmp( name + len - 4, ".DIR", 4 ) == 0;
}

/* ------------------------------------------------------ directory walk */

/* Walk one directory file.  This is intentionally independent of the
 * search_ent() in ods2lib so we can yield every entry to a caller-
 * supplied callback (used both by readdir and by file-name lookup).
 */
vmscond_t ods2_iterate_dir( struct FCB *dir_fcb, int latest_only,
                            ods2_diriter_cb cb, void *ctx ) {
    if( dir_fcb == NULL || dir_fcb->head == NULL )
        return SS$_BADPARAM;

    uint32_t efblk = F11SWAP( dir_fcb->head->fh2$w_recattr.fat$l_efblk );
    uint16_t ffb   = F11WORD( dir_fcb->head->fh2$w_recattr.fat$w_ffbyte );
    if( ods2_rt.debug )
        fprintf( stderr,
                 "fuse-ods2: iterate_dir efblk=%u ffb=%u latest_only=%d\n",
                 (unsigned)efblk, (unsigned)ffb, latest_only );
    if( efblk == 0 )
        return SS$_NORMAL;
    if( ffb == 0 )
        --efblk;
    if( efblk == 0 )
        return SS$_NORMAL;

    /* In latest_only mode we want one filler() call per filename.
     * Names are sorted across records, but a single filename can be
     * split across consecutive records when version count exceeds the
     * record budget (search_ent in ods2lib has the same caveat).  We
     * remember the last emitted name to skip the second piece. */
    char last_name[128];
    int  last_name_len = -1;
    int  stop = 0;

    for( uint32_t blk = 1; blk <= efblk && !stop; ++blk ) {
        struct VIOC *vioc = NULL;
        char        *buf  = NULL;
        uint32_t     blocks = 0;
        vmscond_t    sts;

        sts = accesschunk( dir_fcb, blk, &vioc, &buf, &blocks, 0 );
        if( !$SUCCESSFUL(sts) )
            return sts;

        char *p   = buf;
        char *end = buf + BLOCKSIZE;
        if( ods2_rt.debug )
            fprintf( stderr,
                     "fuse-ods2: iterate_dir: block %u loaded blocks=%u\n",
                     (unsigned)blk, (unsigned)blocks );

        while( p + sizeof(struct dir$r_rec) <= end ) {
            struct dir$r_rec *dr = (struct dir$r_rec *)p;
            uint16_t size = F11WORD( dr->dir$w_size );

            /* End-of-block sentinel: 0xffff. */
            if( size == 0xffff ) {
                if( ods2_rt.debug )
                    fprintf( stderr,
                             "fuse-ods2: iterate_dir:   end-of-block at offs=%ld\n",
                             (long)(p - buf) );
                break;
            }
            if( size + 2 > BLOCKSIZE ) {
                if( ods2_rt.debug )
                    fprintf( stderr,
                             "fuse-ods2: iterate_dir:   record size=%u too big at offs=%ld\n",
                             (unsigned)size, (long)(p - buf) );
                break;
            }
            if( p + size + 2 > end ) {
                if( ods2_rt.debug )
                    fprintf( stderr,
                             "fuse-ods2: iterate_dir:   record overruns block at offs=%ld size=%u\n",
                             (long)(p - buf), (unsigned)size );
                break;
            }

            uint8_t namecount = dr->dir$b_namecount;
            char   *namebase  = dr->dir$t_name;
            if( namebase + namecount > end ) {
                if( ods2_rt.debug )
                    fprintf( stderr,
                             "fuse-ods2: iterate_dir:   name overruns block (count=%u)\n",
                             (unsigned)namecount );
                break;
            }
            if( ods2_rt.debug )
                fprintf( stderr,
                         "fuse-ods2: iterate_dir:   record offs=%ld size=%u namecount=%u name='%.*s'\n",
                         (long)(p - buf), (unsigned)size,
                         (unsigned)namecount, namecount, namebase );

            /* Skip a record whose name we already emitted (split). */
            int same_as_last = ( latest_only
                                 && last_name_len == namecount
                                 && namecount > 0
                                 && (size_t)namecount <= sizeof last_name
                                 && memcmp( last_name, namebase, namecount ) == 0 );

            char *eptr    = namebase + ((namecount + 1) & ~1);
            char *rec_end = (char *)dr + size + 2;
            int   first   = 1;
            while( eptr + sizeof(struct dir$r_ent) <= rec_end ) {
                struct dir$r_ent *de = (struct dir$r_ent *)eptr;
                int version = (int)F11WORD( de->dir$w_version );

                int emit = !latest_only || (first && !same_as_last);
                if( emit ) {
                    if( cb( namebase, namecount, version, &de->dir$w_fid, ctx )
                        != 0 ) {
                        stop = 1;
                        break;
                    }
                }
                first = 0;
                eptr += sizeof(struct dir$r_ent);
            }

            if( namecount > 0 && (size_t)namecount <= sizeof last_name ) {
                memcpy( last_name, namebase, namecount );
                last_name_len = namecount;
            }

            if( stop )
                break;
            p += size + 2;
        }

        deaccesschunk( vioc, 0, 0, 1 );
    }

    return SS$_NORMAL;
}

/* ------------------------------------------------------ path resolver */

/* Lookup state for a "find name within parent dir" search. */
struct namematch_ctx {
    const char     *want;          /* uppercase, no version            */
    int             want_len;
    int             want_version;  /* 0 == any (caller wants latest)   */
    int             best_version;  /* highest version seen for matches */
    struct fiddef   best_fid;
    int             found;
};

static int namematch_cb( const char *name, int namelen, int version,
                         const struct fiddef *fid, void *ctx ) {
    struct namematch_ctx *m = ctx;

    /* Names are uppercase on disk; want has been uppercased too. */
    if( namelen != m->want_len )
        return 0;
    if( memcmp( name, m->want, namelen ) != 0 )
        return 0;

    if( m->want_version > 0 ) {
        if( version == m->want_version ) {
            m->found = 1;
            m->best_fid = *fid;
            return 1;       /* stop */
        }
    } else {
        /* "highest" -- entries are stored highest-first, so the first
         * hit is what we want, but be defensive. */
        if( !m->found || version > m->best_version ) {
            m->best_fid = *fid;
            m->best_version = version;
            m->found = 1;
        }
    }
    return 0;
}

vmscond_t ods2_path_to_fid( const char *path, struct fiddef *out_fid,
                            int *out_is_dir ) {
    if( path == NULL || *path == '\0' )
        return SS$_BADPARAM;

    /* Root and "/000000" both map to the MFD. */
    if( strcmp( path, "/" ) == 0 ) {
        out_fid->fid$w_num = FID$C_MFD;
        out_fid->fid$w_seq = FID$C_MFD;
        out_fid->fid$b_rvn = 0;
        out_fid->fid$b_nmx = 0;
        if( out_is_dir ) *out_is_dir = 1;
        return SS$_NORMAL;
    }

    /* Strip a trailing slash, but remember it as a hint. */
    size_t plen = strlen( path );
    char  *work = strdup( path );
    if( work == NULL )
        return SS$_INSFMEM;
    if( plen > 1 && work[plen-1] == '/' )
        work[plen-1] = '\0';

    /* Split into "/<dir1>/<dir2>/.../<file>".  We treat every component
     * but the last as a directory.  A trailing slash forces the last
     * component to be a directory too.
     */
    char *file = split_last( work );    /* points after last '/' inside work */
    if( file == NULL )                   /* shouldn't happen */
        file = work;

    /* "work" now holds the dir prefix (with leading '/') or empty. */
    char vmsdir[512];
    posix_dir_to_vms( work, vmsdir, sizeof vmsdir );

    /* Resolve directory part. */
    struct dsc_descriptor dirdsc;
    memset( &dirdsc, 0, sizeof dirdsc );
    dirdsc.dsc_w_length  = (uint16_t)strlen( vmsdir );
    dirdsc.dsc_a_pointer = vmsdir;

    struct fiddef dirid, dirfid;
    vmscond_t sts = direct_dirid( ods2_vcb, &dirdsc, &dirid, &dirfid );
    if( !$SUCCESSFUL(sts) ) {
        free( work );
        return sts;
    }

    /* If the path was just "/dir/" (no file part), return the dir FID. */
    if( file == NULL || *file == '\0' ) {
        *out_fid = dirfid;
        if( out_is_dir ) *out_is_dir = 1;
        free( work );
        return SS$_NORMAL;
    }

    /* Otherwise look up <file> inside <dirfid>. */
    char namebuf[256];
    if( strlen( file ) >= sizeof namebuf ) {
        free( work );
        return SS$_BADFILENAME;
    }
    strcpy( namebuf, file );
    upper_inplace( namebuf );

    int want_version;
    if( !parse_file_version( namebuf, &want_version ) ) {
        free( work );
        return SS$_BADFILENAME;
    }

    /* POSIX callers naturally ask for "/SUBDIR", while ODS-2 stores
     * that directory file as "SUBDIR.DIR;1".  First try the literal
     * component, then fall back to the directory-file spelling below.
     */

    struct FCB *dfcb = NULL;
    sts = accessfile( ods2_vcb, &dirfid, &dfcb, 0 );
    if( !$SUCCESSFUL(sts) ) {
        free( work );
        return sts;
    }

    struct namematch_ctx m = { 0 };
    m.want         = namebuf;
    m.want_len     = (int)strlen( namebuf );
    m.want_version = want_version;

    sts = ods2_iterate_dir( dfcb, 0 /* visit every version */,
                            namematch_cb, &m );

    if( $SUCCESSFUL(sts) && !m.found && !name_has_dir_suffix( namebuf ) ) {
        size_t namelen = strlen( namebuf );
        if( namelen + 4 < sizeof namebuf ) {
            memcpy( namebuf + namelen, ".DIR", 5 );
            memset( &m, 0, sizeof m );
            m.want         = namebuf;
            m.want_len     = (int)strlen( namebuf );
            m.want_version = want_version;

            sts = ods2_iterate_dir( dfcb, 0 /* visit every version */,
                                    namematch_cb, &m );
        }
    }

    deaccessfile( dfcb );
    free( work );

    if( !$SUCCESSFUL(sts) )
        return sts;
    if( !m.found )
        return SS$_NOSUCHFILE;

    *out_fid = m.best_fid;

    if( out_is_dir ) {
        /* Cheap test: ".DIR" suffix.  ops_getattr does the real test
         * by reading the HEAD; this hint is only used by callers who
         * want to short-circuit. */
        *out_is_dir = name_has_dir_suffix( namebuf );
    }
    return SS$_NORMAL;
}
