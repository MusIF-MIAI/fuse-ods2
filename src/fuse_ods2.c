/*
 * fuse_ods2.c - entry point and FUSE wiring.
 *
 * In this Phase-1 milestone the program parses its command line,
 * mounts an ODS-2 volume through ods2lib, prints the volume header
 * under -o debug, and registers a stub set of FUSE operations.  The
 * filesystem mount succeeds but the namespace is empty; getattr,
 * readdir and friends are filled in by the next phase.
 */

#define FUSE_USE_VERSION 31

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <fuse.h>

#include "access.h"
#include "f11def.h"
#include "ods2.h"
#include "ssdef.h"
#include "stsdef.h"

/* ------------------------------------------------------ global mount state */

int   fuse_ods2_debug  = 0;
off_t fuse_ods2_offset = 0;

static struct {
    const char *image;          /* primary image path                   */
    const char *vol_extra;      /* comma-separated extra volumes (RVN>1)*/
    long long   offset;         /* byte offset into the image           */
    int         allversions;
    int         lower;
    int         textmode;
    int         debug;
    int         force_uid;      /* 1 if uid was supplied                 */
    uid_t       uid;
    int         force_gid;
    gid_t       gid;
} opts;

static struct VCB *g_vcb = NULL;

/* ------------------------------------------------------ option processing */

#define OPT_KEY_HELP     1
#define OPT_KEY_VERSION  2

static struct fuse_opt fuse_ods2_opts[] = {
    { "offset=%lld",      offsetof(__typeof__(opts), offset),      0 },
    { "vol=%s",           offsetof(__typeof__(opts), vol_extra),   0 },
    { "uid=%u",           offsetof(__typeof__(opts), uid),         0 },
    { "gid=%u",           offsetof(__typeof__(opts), gid),         0 },
    { "allversions",      offsetof(__typeof__(opts), allversions), 1 },
    { "lower",            offsetof(__typeof__(opts), lower),       1 },
    { "textmode",         offsetof(__typeof__(opts), textmode),    1 },
    { "debug",            offsetof(__typeof__(opts), debug),       1 },
    FUSE_OPT_KEY("-h",        OPT_KEY_HELP),
    FUSE_OPT_KEY("--help",    OPT_KEY_HELP),
    FUSE_OPT_KEY("-V",        OPT_KEY_VERSION),
    FUSE_OPT_KEY("--version", OPT_KEY_VERSION),
    FUSE_OPT_END
};

static void usage( const char *prog ) {
    fprintf( stderr,
        "usage: %s [options] <image> <mountpoint>\n"
        "\n"
        "options:\n"
        "  -o offset=N        byte offset of the ODS-2 volume in the image\n"
        "  -o vol=A,B,...     extra image files for RVNs 2..N (volume set)\n"
        "  -o allversions     expose every file version as NAME.EXT;n\n"
        "  -o lower           lowercase the file names\n"
        "  -o textmode        decode RMS VAR/VFC records into LF lines\n"
        "  -o uid=N           force a specific uid on every file\n"
        "  -o gid=N           force a specific gid on every file\n"
        "  -o debug           verbose diagnostics on stderr\n"
        "  -s                 single-threaded (recommended)\n"
        "  -f                 foreground (do not daemonize)\n"
        "  -d                 same as -odebug + -f\n"
        "\n"
        "The mount is always read-only.\n",
        prog );
}

static int opt_proc( void *data, const char *arg, int key,
                     struct fuse_args *outargs ) {
    (void)data;
    switch( key ) {
        case FUSE_OPT_KEY_NONOPT:
            if( opts.image == NULL ) {
                opts.image = strdup( arg );
                return 0;       /* swallow */
            }
            return 1;           /* keep mountpoint for libfuse */
        case OPT_KEY_HELP:
            usage( outargs->argv[0] );
            fuse_opt_add_arg( outargs, "-ho" );
            return 0;
        case OPT_KEY_VERSION:
            fprintf( stderr, "fuse-ods2 0.1\n" );
            fuse_opt_add_arg( outargs, "--version" );
            return 0;
        default:
            return 1;
    }
}

/* ------------------------------------------------------ FUSE operations */

static void *ops_init( struct fuse_conn_info *conn,
                       struct fuse_config *cfg ) {
    (void)conn;
    cfg->kernel_cache = 1;
    cfg->use_ino      = 1;
    return NULL;
}

static int ops_getattr( const char *path, struct stat *st,
                        struct fuse_file_info *fi ) {
    (void)fi;
    memset( st, 0, sizeof *st );
    if( strcmp( path, "/" ) == 0 ) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_uid   = opts.force_uid ? opts.uid : getuid();
        st->st_gid   = opts.force_gid ? opts.gid : getgid();
        return 0;
    }
    return -ENOENT;
}

static int ops_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags ) {
    (void)offset; (void)fi; (void)flags;
    if( strcmp( path, "/" ) != 0 )
        return -ENOENT;
    filler( buf, ".",  NULL, 0, 0 );
    filler( buf, "..", NULL, 0, 0 );
    return 0;
}

static int ops_open( const char *path, struct fuse_file_info *fi ) {
    (void)path; (void)fi;
    return -ENOENT;
}

static int ops_read( const char *path, char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi ) {
    (void)path; (void)buf; (void)size; (void)off; (void)fi;
    return -ENOENT;
}

#define ROFS(name, ...) static int ops_##name(__VA_ARGS__) { return -EROFS; }
ROFS(mknod,   const char *p, mode_t m, dev_t d)
ROFS(mkdir,   const char *p, mode_t m)
ROFS(unlink,  const char *p)
ROFS(rmdir,   const char *p)
ROFS(symlink, const char *t, const char *l)
ROFS(rename,  const char *f, const char *t, unsigned int flags)
ROFS(link,    const char *f, const char *t)
ROFS(chmod,   const char *p, mode_t m, struct fuse_file_info *fi)
ROFS(chown,   const char *p, uid_t u, gid_t g, struct fuse_file_info *fi)
ROFS(truncate, const char *p, off_t s, struct fuse_file_info *fi)
ROFS(write,   const char *p, const char *b, size_t s, off_t o,
              struct fuse_file_info *fi)
ROFS(create,  const char *p, mode_t m, struct fuse_file_info *fi)
ROFS(utimens, const char *p, const struct timespec t[2],
              struct fuse_file_info *fi)
#undef ROFS

static const struct fuse_operations ods2_ops = {
    .init     = ops_init,
    .getattr  = ops_getattr,
    .readdir  = ops_readdir,
    .open     = ops_open,
    .read     = ops_read,
    .mknod    = ops_mknod,
    .mkdir    = ops_mkdir,
    .unlink   = ops_unlink,
    .rmdir    = ops_rmdir,
    .symlink  = ops_symlink,
    .rename   = ops_rename,
    .link     = ops_link,
    .chmod    = ops_chmod,
    .chown    = ops_chown,
    .truncate = ops_truncate,
    .write    = ops_write,
    .create   = ops_create,
    .utimens  = ops_utimens,
};

/* ------------------------------------------------------ mount lifecycle */

static void log_home( const struct VCB *vcb ) {
    const struct VCBDEV *d = &vcb->vcbdev[0];
    char volname[13];

    memcpy( volname, d->home.hm2$t_volname, 12 );
    volname[12] = '\0';
    /* Trim trailing spaces. */
    for( int i = 11; i >= 0 && volname[i] == ' '; --i )
        volname[i] = '\0';

    fprintf( stderr,
        "fuse-ods2: mounted volume \"%s\"\n"
        "           cluster size  : %u blocks\n"
        "           max cluster   : %u\n"
        "           free clusters : %u\n"
        "           devices in set: %u\n",
        volname,
        (unsigned)d->clustersize,
        (unsigned)d->max_cluster,
        (unsigned)d->free_clusters,
        vcb->devices );
}

static int do_mount( void ) {
    char *names[16] = { 0 };
    char *labels[16] = { 0 };
    unsigned ndev = 0;
    vmscond_t sts;

    names[ndev++] = (char *)opts.image;

    if( opts.vol_extra != NULL ) {
        char *list = strdup( opts.vol_extra );
        char *tok, *save = NULL;
        for( tok = strtok_r( list, ",", &save );
             tok != NULL && ndev < (sizeof names / sizeof names[0]);
             tok = strtok_r( NULL, ",", &save ) )
            names[ndev++] = strdup( tok );
        /* leak list/strings until exit; the strings are owned by
         * device_lookup() entries past mount() success. */
    }

    sts = mount( MOU_VIRTUAL, ndev, names, labels );
    if( !$SUCCESSFUL(sts) ) {
        fprintf( stderr,
                 "fuse-ods2: mount failed: ods2-status=0x%08x\n",
                 (unsigned)(sts & STS$M_COND_ID) );
        return -1;
    }

    g_vcb = vcb_list;
    if( opts.debug && g_vcb != NULL )
        log_home( g_vcb );
    return 0;
}

static void do_dismount( void ) {
    if( g_vcb != NULL ) {
        dismount( g_vcb, 0 );
        g_vcb = NULL;
    }
}

/* ------------------------------------------------------ main */

int main( int argc, char *argv[] ) {
    struct fuse_args args = FUSE_ARGS_INIT( argc, argv );
    int ret;

    if( fuse_opt_parse( &args, &opts, fuse_ods2_opts, opt_proc ) == -1 )
        return 1;

    /* Force read-only at the kernel level too. */
    fuse_opt_add_arg( &args, "-oro" );
    fuse_opt_add_arg( &args, "-odefault_permissions" );

    /* Propagate to backend statics. */
    fuse_ods2_debug  = opts.debug;
    fuse_ods2_offset = (off_t)opts.offset;

    /* Track whether the user really set uid/gid: fuse_opt sets the
     * field regardless, but the default of 0 is meaningful (root), so
     * we infer "set" from whether the option string was seen by parsing
     * argv ourselves.  Cheap heuristic: scan the original argv.
     */
    for( int i = 0; i < argc; ++i ) {
        if( strstr( argv[i], "uid=" ) ) opts.force_uid = 1;
        if( strstr( argv[i], "gid=" ) ) opts.force_gid = 1;
    }

    if( opts.image == NULL ) {
        usage( argv[0] );
        return 1;
    }

    if( do_mount() != 0 )
        return 2;

    ret = fuse_main( args.argc, args.argv, &ods2_ops, NULL );

    do_dismount();
    fuse_opt_free_args( &args );
    return ret;
}
