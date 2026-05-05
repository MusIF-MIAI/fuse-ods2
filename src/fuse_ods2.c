/*
 * fuse_ods2.c - entry point and FUSE wiring.
 *
 * Parses the command line, mounts an ODS-2 volume through ods2lib,
 * registers the FUSE operation table (implemented in ops.c) and
 * tears the mount down on exit.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ods2_ops.h"
#include "recfmt.h"

#include "access.h"
#include "f11def.h"
#include "ods2.h"
#include "ssdef.h"
#include "stsdef.h"

/* ------------------------------------------------------ globals */

int   fuse_ods2_debug  = 0;
off_t fuse_ods2_offset = 0;

struct ods2_runtime ods2_rt;
struct VCB         *ods2_vcb = NULL;

/* ------------------------------------------------------ option processing */

struct cli_opts {
    const char *image;
    const char *vol_extra;
    long long   offset;
    int         allversions;
    int         lower;
    int         textmode;
    int         debug;
    unsigned    uid;
    unsigned    gid;
};

static struct cli_opts cli;

#define OPT_KEY_HELP     1
#define OPT_KEY_VERSION  2

static struct fuse_opt fuse_ods2_opts[] = {
    { "offset=%lld",      offsetof(struct cli_opts, offset),      0 },
    { "vol=%s",           offsetof(struct cli_opts, vol_extra),   0 },
    { "uid=%u",           offsetof(struct cli_opts, uid),         0 },
    { "gid=%u",           offsetof(struct cli_opts, gid),         0 },
    { "allversions",      offsetof(struct cli_opts, allversions), 1 },
    { "lower",            offsetof(struct cli_opts, lower),       1 },
    { "textmode",         offsetof(struct cli_opts, textmode),    1 },
    { "debug",            offsetof(struct cli_opts, debug),       1 },
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
            if( cli.image == NULL ) {
                cli.image = strdup( arg );
                return 0;
            }
            return 1;
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

/* ------------------------------------------------------ FUSE table */

static void *ops_init( struct fuse_conn_info *conn,
                       struct fuse_config *cfg ) {
    (void)conn;
    cfg->kernel_cache = 1;
    cfg->use_ino      = 1;
    return NULL;
}

#define ROFS(name, ...) static int ops_##name(__VA_ARGS__) { return -EROFS; }
ROFS(mknod,    const char *p, mode_t m, dev_t d)
ROFS(mkdir,    const char *p, mode_t m)
ROFS(unlink,   const char *p)
ROFS(rmdir,    const char *p)
ROFS(symlink,  const char *t, const char *l)
ROFS(rename,   const char *f, const char *t, unsigned int flags)
ROFS(link,     const char *f, const char *t)
ROFS(chmod,    const char *p, mode_t m, struct fuse_file_info *fi)
ROFS(chown,    const char *p, uid_t u, gid_t g, struct fuse_file_info *fi)
ROFS(truncate, const char *p, off_t s, struct fuse_file_info *fi)
ROFS(write,    const char *p, const char *b, size_t s, off_t o,
               struct fuse_file_info *fi)
ROFS(create,   const char *p, mode_t m, struct fuse_file_info *fi)
ROFS(utimens,  const char *p, const struct timespec t[2],
               struct fuse_file_info *fi)
#undef ROFS

static const struct fuse_operations ods2_ops = {
    .init     = ops_init,
    .getattr  = ods2_getattr,
    .readdir  = ods2_readdir,
    .open     = ods2_open,
    .read     = ods2_read,
    .release  = ods2_release,
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
    for( int i = 11; i >= 0 && volname[i] == ' '; --i )
        volname[i] = '\0';

    /* In a read-only mount simtools' get_scb() is skipped, so
     * d->clustersize / d->max_cluster / d->free_clusters stay zero.
     * Reconstruct the first two from the home block and the device
     * size; free-clusters needs the bitmap (write-side only) so we
     * just say "n/a" rather than print a misleading zero. */
    unsigned cluster = (unsigned)F11WORD( d->home.hm2$w_cluster );
    if( cluster == 0 ) cluster = 1;
    off_t    devsz   = (d->dev != NULL) ? d->dev->eofptr : 0;
    unsigned long max_cluster = cluster
        ? (unsigned long)((devsz / 512) / cluster)
        : 0;

    fprintf( stderr,
        "fuse-ods2: mounted volume \"%s\"\n"
        "           cluster size  : %u blocks\n"
        "           total clusters: %lu\n"
        "           free clusters : n/a (read-only)\n"
        "           devices in set: %u\n",
        volname, cluster, max_cluster, vcb->devices );
}

static int do_mount( void ) {
    char *names[16] = { 0 };
    char *labels[16] = { 0 };
    unsigned ndev = 0;
    vmscond_t sts;

    names[ndev++] = (char *)cli.image;

    if( cli.vol_extra != NULL ) {
        char *list = strdup( cli.vol_extra );
        char *tok, *save = NULL;
        for( tok = strtok_r( list, ",", &save );
             tok != NULL && ndev < (sizeof names / sizeof names[0]);
             tok = strtok_r( NULL, ",", &save ) )
            names[ndev++] = strdup( tok );
    }

    sts = mount( MOU_VIRTUAL, ndev, names, labels );
    if( !$SUCCESSFUL(sts) ) {
        fprintf( stderr,
                 "fuse-ods2: mount failed: ods2-status=0x%08x\n",
                 (unsigned)(sts & STS$M_COND_ID) );
        return -1;
    }

    ods2_vcb = vcb_list;
    if( cli.debug && ods2_vcb != NULL )
        log_home( ods2_vcb );
    return 0;
}

static void do_dismount( void ) {
    recfmt_clear_cache();
    if( ods2_vcb != NULL ) {
        dismount( ods2_vcb, 0 );
        ods2_vcb = NULL;
    }
}

/* ------------------------------------------------------ main */

int main( int argc, char *argv[] ) {
    struct fuse_args args = FUSE_ARGS_INIT( argc, argv );
    int ret;

    if( fuse_opt_parse( &args, &cli, fuse_ods2_opts, opt_proc ) == -1 )
        return 1;

    /* Read-only is enforced inside every write op (-EROFS).  We do
     * NOT inject -oro / -odefault_permissions into the libfuse arg
     * vector here: some kernel/libfuse3 combos return EPERM on the
     * first /dev/fuse read when these options are duplicated in the
     * command line.  The kernel-side "ro" flag, if the user wants it,
     * can still be passed via -o ro on the CLI. */

    /* Decide whether uid/gid were really set: parse argv heuristically
     * since fuse_opt always writes the field even for the default. */
    int seen_uid = 0, seen_gid = 0;
    for( int i = 0; i < argc; ++i ) {
        if( strstr( argv[i], "uid=" ) ) seen_uid = 1;
        if( strstr( argv[i], "gid=" ) ) seen_gid = 1;
    }

    /* Push parsed options into the runtime struct that ops.c reads. */
    ods2_rt.allversions = cli.allversions;
    ods2_rt.lower       = cli.lower;
    ods2_rt.textmode    = cli.textmode;
    ods2_rt.debug       = cli.debug;
    ods2_rt.force_uid   = seen_uid;
    ods2_rt.force_gid   = seen_gid;
    ods2_rt.uid         = (uid_t)cli.uid;
    ods2_rt.gid         = (gid_t)cli.gid;

    fuse_ods2_debug  = cli.debug;
    fuse_ods2_offset = (off_t)cli.offset;

    if( cli.image == NULL ) {
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
