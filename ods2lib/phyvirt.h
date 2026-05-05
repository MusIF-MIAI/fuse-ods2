/*
 * phyvirt.h - minimal stub interface used by access.c
 *
 * Replaces the full phyvirt.h shipped with simtools/ods2.  In fuse-ods2
 * the virtual-device layer is implemented by src/phyfuse.c, which
 * provides virt_open / virt_close / virt_read / virt_write directly on
 * top of pread() against a flat image file.
 */

#ifndef _PHYVIRT_H
#define _PHYVIRT_H

#include <stddef.h>
#include <stdint.h>

#include "ods2.h"

typedef struct disktype disktype_t;
typedef disktype_t *disktypep_t;

struct disktype {
    const char *name;
    uint32_t sectorsize, sectors, tracks, cylinders;
    uint32_t reserved, interleave, skew;
    uint32_t flags;
#define DISK_BAD144 1
#define DISK_BADSW  2
};

extern struct disktype disktype[];
extern size_t max_disktype;

struct DEV;

void virt_show( void );
vmscond_t virt_open( char **devname, uint32_t flags, struct DEV **dev );
char *virt_lookup( const char *devnam );
vmscond_t virt_close( struct DEV *dev );

vmscond_t virt_read( struct DEV *dev, uint32_t lbn, uint32_t length,
                     char *buffer );
vmscond_t virt_write( struct DEV *dev, uint32_t lbn, uint32_t length,
                      const char *buffer );

#endif /* _PHYVIRT_H */
