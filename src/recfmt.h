/*
 * recfmt.h - public API of the -o textmode decoder.
 */

#ifndef FUSE_ODS2_RECFMT_H
#define FUSE_ODS2_RECFMT_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "access.h"
#include "f11def.h"

/* True if this file's record format is sensible to expose decoded.
 * Currently always true except for directories. */
int     recfmt_textmode_eligible( const struct HEAD *head );

/* Decoded length in bytes (after stripping VAR/VFC headers and
 * inserting LF where the FAB$M_CR attribute requires it).  Returns
 * -1 on memory error.  Triggers a one-time decode on first call. */
ssize_t recfmt_logical_size( struct FCB *fcb );

/* Random-access read against the decoded view.  Returns the number
 * of bytes copied, 0 at EOF, -1 on error. */
ssize_t recfmt_read( struct FCB *fcb, off_t off, char *buf, size_t size );

/* Drop the per-FID decoded cache.  Called from dismount(). */
void    recfmt_clear_cache( void );

#endif /* FUSE_ODS2_RECFMT_H */
