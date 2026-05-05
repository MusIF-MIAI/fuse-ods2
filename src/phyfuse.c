/*
 * phyfuse.c - I/O backend for fuse-ods2.
 *
 * Replaces the simtools phyvirt.c / phyunix.c pair.  All it does is
 * pread() against a flat image file at a configurable byte offset, with
 * writes refused at the layer entry point.  No SimH geometry tricks, no
 * bad-block tables, no VHD: an ODS-2 dump on a modern host is just a
 * sequence of 512-byte blocks.
 *
 * Public surface (consumed by ods2lib/access.c):
 *   - virt_open / virt_close / virt_read / virt_write
 *   - virt_lookup (always returns NULL: no virtual-device registry)
 *   - virt_show   (no-op)
 *   - disktype[]  (a single GENERIC entry; ods2_mount() only reads geometry
 *                  to compute a HOME-search delta, which is 1 for us)
 *   - max_disktype
 *   - delta_from_index() (always 1)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "access.h"
#include "device.h"
#include "ods2.h"
#include "phyio.h"
#include "phyvirt.h"
#include "ssdef.h"
#include "stsdef.h"

extern int   fuse_ods2_debug;
extern off_t fuse_ods2_offset;

/* ------------------------------------------------------------------ disktype */

/* ods2_mount() iterates this table only when listing volumes; access.c does
 * not care about geometry once delta_from_index() returns.  One generic
 * entry is enough.
 */
struct disktype disktype[] = {
    { "GENERIC", 512, 1, 1, 1, 0, 0, 0, 0 },
    { NULL,        0, 0, 0, 0, 0, 0, 0, 0 }
};

size_t max_disktype = (sizeof(disktype) / sizeof(disktype[0])) - 1;

uint32_t delta_from_index( size_t index ) {
    (void)index;
    return 1;
}

/* ------------------------------------------------------------------ virt_* */

void virt_show( void ) {}

/* No virtual-device registry: ods2_mount() in access.c only calls
 * virt_lookup() during dismount accounting, where NULL is the right
 * answer for "this device is not a virtual disk image alias".
 */
char *virt_lookup( const char *devnam ) {
    (void)devnam;
    return NULL;
}

static vmscond_t phyfuse_read( struct DEV *dev, uint32_t block,
                               uint32_t length, char *buffer );
static vmscond_t phyfuse_write( struct DEV *dev, uint32_t block,
                                uint32_t length, const char *buffer );

vmscond_t virt_open( char **devname, uint32_t flags, struct DEV **retdev ) {
    vmscond_t sts;
    struct DEV *dev = NULL;
    int fd;
    struct stat st;
    const char *path = *devname;

    if( path == NULL || *path == '\0' )
        return SS$_BADPARAM;

    if( $FAILS(sts = device_lookup( strlen( path ), (char *)path,
                                    1 /* create */, &dev )) )
        return sts;

    /* The simtools ods2_mount() test for "device already mounted" reads
     * dev->vcb; a freshly cached DEV may already be in the LRU from a
     * previous mount that closed.  Reset the per-mount fields.
     */
    dev->vcb     = NULL;
    dev->access  = flags;
    dev->disktype = &disktype[0];
    dev->devread  = phyfuse_read;
    dev->devwrite = phyfuse_write;
    dev->handle   = -1;
    dev->eofptr   = 0;

    fd = open( path, O_RDONLY );
    if( fd < 0 ) {
        if( fuse_ods2_debug )
            fprintf( stderr, "fuse-ods2: open('%s'): %s\n",
                     path, strerror( errno ) );
        device_done( dev );
        return SS$_NOSUCHFILE | STS$M_INHIB_MSG;
    }

    if( fstat( fd, &st ) != 0 ) {
        close( fd );
        device_done( dev );
        return SS$_NOSUCHFILE | STS$M_INHIB_MSG;
    }

    if( st.st_size <= fuse_ods2_offset ) {
        fprintf( stderr,
                 "fuse-ods2: image '%s' is smaller than the requested "
                 "offset (size=%lld, offset=%lld)\n",
                 path, (long long)st.st_size, (long long)fuse_ods2_offset );
        close( fd );
        device_done( dev );
        return SS$_BADPARAM | STS$M_INHIB_MSG;
    }

    dev->handle = fd;
    dev->eofptr = st.st_size - fuse_ods2_offset;

    if( fuse_ods2_debug )
        fprintf( stderr,
                 "fuse-ods2: opened '%s' fd=%d size=%lld offset=%lld\n",
                 path, fd, (long long)st.st_size,
                 (long long)fuse_ods2_offset );

    *retdev = dev;
    return SS$_NORMAL;
}

vmscond_t virt_close( struct DEV *dev ) {
    if( dev == NULL )
        return SS$_NORMAL;
    if( dev->handle >= 0 ) {
        close( dev->handle );
        dev->handle = -1;
    }
    device_done( dev );
    return SS$_NORMAL;
}

/* ----------------------------------------------------------- block-level I/O */

static vmscond_t phyfuse_read( struct DEV *dev, uint32_t block,
                               uint32_t length, char *buffer ) {
    off_t pos;
    ssize_t got;

    if( dev->handle < 0 )
        return SS$_DEVOFFLINE;

    pos = (off_t)block * (off_t)512 + fuse_ods2_offset;

    got = pread( dev->handle, buffer, length, pos );
    if( got < 0 ) {
        if( fuse_ods2_debug )
            fprintf( stderr,
                     "fuse-ods2: pread(block=%u len=%u pos=%lld): %s\n",
                     (unsigned)block, (unsigned)length,
                     (long long)pos, strerror( errno ) );
        return SS$_PARITY | STS$M_INHIB_MSG;
    }
    if( (uint32_t)got != length ) {
        if( got == 0 )
            return SS$_ENDOFFILE | STS$M_INHIB_MSG;
        if( fuse_ods2_debug )
            fprintf( stderr,
                     "fuse-ods2: short read at block=%u (got=%zd of %u)\n",
                     (unsigned)block, got, (unsigned)length );
        memset( buffer + got, 0, length - (uint32_t)got );
        return SS$_ENDOFFILE | STS$M_INHIB_MSG;
    }
    return SS$_NORMAL;
}

static vmscond_t phyfuse_write( struct DEV *dev, uint32_t block,
                                uint32_t length, const char *buffer ) {
    (void)dev; (void)block; (void)length; (void)buffer;
    return SS$_WRITLCK | STS$M_INHIB_MSG;
}

vmscond_t virt_read( struct DEV *dev, uint32_t lbn, uint32_t length,
                     char *buffer ) {
    return phyfuse_read( dev, lbn, length, buffer );
}

vmscond_t virt_write( struct DEV *dev, uint32_t lbn, uint32_t length,
                      const char *buffer ) {
    return phyfuse_write( dev, lbn, length, buffer );
}
