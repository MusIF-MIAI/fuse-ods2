/*
 * compat_glue.c - minimal replacements for ods2lib symbols that
 * normally live in the interactive ods2 frontend (sysmsg.c, mountcmd.c,
 * update.c, etc.).
 *
 * fuse-ods2 is read-only and has no DCL-style command interpreter, so
 * we only need:
 *   - a printmsg/getmsg pair that emits something useful on stderr
 *   - a mountdef() that succeeds without recording the device
 *   - write-side stubs that fail loudly if the read-only path ever
 *     ends up calling them (it shouldn't)
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ods2.h"
#include "ssdef.h"
#include "stsdef.h"
#include "sysmsg.h"
#include "access.h"

extern int fuse_ods2_debug;

static const char *severity_letter( vmscond_t cond ) {
    switch( cond & STS$M_SEVERITY ) {
        case STS$K_WARNING:  return "W";
        case STS$K_SUCCESS:  return "S";
        case STS$K_ERROR:    return "E";
        case STS$K_INFO:     return "I";
        case STS$K_SEVERE:   return "F";
        default:             return "?";
    }
}

static char msg_format = MSG_FULL;

vmscond_t printmsg( vmscond_t vmscond, unsigned int flags, ... ) {
    va_list ap;
    va_start( ap, flags );
    vprintmsg( vmscond, flags, ap );
    va_end( ap );
    return vmscond | STS$M_INHIB_MSG;
}

vmscond_t vprintmsg( vmscond_t vmscond, unsigned int flags, va_list ap ) {
    (void)ap;
    if( fuse_ods2_debug || (vmscond & STS$M_SEVERITY) != STS$K_SUCCESS ) {
        fprintf( stderr, "ods2-%s-%08x\n",
                 severity_letter( vmscond ),
                 (unsigned)(vmscond & STS$M_COND_ID) );
    }
    return vmscond | STS$M_INHIB_MSG;
}

const char *getmsg( vmscond_t vmscond, unsigned int flags, ... ) {
    static char buf[64];
    snprintf( buf, sizeof buf, "ods2-%s-%08x",
              severity_letter( vmscond ),
              (unsigned)(vmscond & STS$M_COND_ID) );
    return buf;
}

const char *getmsg_string( vmscond_t vmscond, unsigned int flags ) {
    return getmsg( vmscond, flags );
}

vmscond_t set_message_file( const char *filename ) { return SS$_NORMAL; }
vmscond_t show_message( int argc, char **argv )   { return SS$_NORMAL; }

vmscond_t set_message_format( options_t new ) {
    options_t old = msg_format;
    msg_format = (char)new;
    return old;
}
options_t get_message_format( void ) { return msg_format; }
void sysmsg_rundown( void ) {}

/* mountcmd.c registers the mounted volume with the DCL frontend.
 * fuse-ods2 has no frontend, so this is a no-op.
 */
vmscond_t mountdef( const char *devnam ) {
    (void)devnam;
    return SS$_NORMAL;
}

/* The write path of access.c references a few helpers from update.c.
 * They are reachable only when VCB_WRITE is set in the volume status,
 * which fuse-ods2 never does.  We keep the symbols so the binary links;
 * if any of them is ever called it indicates a bug, so we abort loudly.
 */
static vmscond_t writelocked( const char *fn ) {
    fprintf( stderr,
             "fuse-ods2: write-path symbol '%s' invoked on read-only mount\n",
             fn );
    abort();
}

vmscond_t update_freecount( struct VCBDEV *vcbdev, uint32_t *retcount ) {
    (void)vcbdev; (void)retcount;
    return writelocked( "update_freecount" );
}
vmscond_t update_create( struct VCB *vcb, struct fiddef *did, char *filename,
                         struct fiddef *fid, struct NEWFILE *attrs,
                         struct FCB **fcb ) {
    (void)vcb; (void)did; (void)filename; (void)fid; (void)attrs; (void)fcb;
    return writelocked( "update_create" );
}
vmscond_t update_extend( struct FCB *fcb, uint32_t blocks, unsigned contig ) {
    (void)fcb; (void)blocks; (void)contig;
    return writelocked( "update_extend" );
}
vmscond_t update_truncate( struct FCB *fcb, uint32_t newsize ) {
    (void)fcb; (void)newsize;
    return writelocked( "update_truncate" );
}
vmscond_t deallocfile( struct FCB *fcb ) {
    (void)fcb;
    return writelocked( "deallocfile" );
}
vmscond_t accesserase( struct VCB *vcb, struct fiddef *fid ) {
    (void)vcb; (void)fid;
    return writelocked( "accesserase" );
}
