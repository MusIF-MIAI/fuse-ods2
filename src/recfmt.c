/*
 * recfmt.c - RMS record-format decoder for -o textmode.
 *
 * ODS-2 stores text files in a few different on-disk shapes:
 *
 *   STMLF / STM  - already a stream, optionally LF-delimited.  Pass
 *                  through unchanged.
 *   STMCR        - stream with CR delimiters.  Translate CR->LF.
 *   VAR          - each record has a 16-bit length prefix (LE),
 *                  followed by 'len' bytes, padded to a word boundary.
 *                  When the FAB$M_CR attribute is set we append LF
 *                  after each record (this is what 'TYPE' does on VMS).
 *   VFC          - VAR with a fat$b_vfcsize-byte fixed control header
 *                  immediately after the length prefix.  We skip the
 *                  control header when emitting text.
 *   FIX          - records of fat$w_rsize bytes, no length prefix.
 *                  With FAB$M_CR we add LF after every record.
 *
 * The FAB$M_BLK attribute means records do not span block boundaries:
 * if the next length prefix would not fit we skip to the next 512-byte
 * block boundary.
 *
 * Strategy: decode the whole file into a malloc'd buffer the first
 * time anyone asks (the typical text file is small), keyed by FID in
 * a small global cache.  ops_getattr returns the cached length;
 * ops_read returns slices of the cached buffer.  The cache lives for
 * the lifetime of the mount.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ods2_lookup.h"
#include "recfmt.h"

#include "access.h"
#include "f11def.h"
#include "ods2.h"
#include "rms.h"
#include "ssdef.h"
#include "stsdef.h"

#ifndef BLOCKSIZE
#define BLOCKSIZE 512
#endif

/* ------------------------------------------------------ raw byte source */

/* Pulls (count) bytes starting at logical byte offset 'off' inside the
 * file, copying them into 'dst'.  Returns the number of bytes actually
 * delivered, which may be less than 'count' at end of file.
 */
static size_t raw_read( struct FCB *fcb, off_t off, void *dst, size_t count ) {
    uint32_t efblk = F11SWAP( fcb->head->fh2$w_recattr.fat$l_efblk );
    uint16_t ffb   = F11WORD( fcb->head->fh2$w_recattr.fat$w_ffbyte );
    off_t    fsize = (efblk == 0) ? 0
                                  : (off_t)(efblk - 1) * BLOCKSIZE + ffb;
    if( off < 0 || off >= fsize )
        return 0;
    if( (off_t)count > fsize - off )
        count = (size_t)(fsize - off);

    size_t  done = 0;
    while( done < count ) {
        uint32_t vbn       = (uint32_t)((off + (off_t)done) / BLOCKSIZE) + 1;
        uint32_t block_off = (uint32_t)((off + (off_t)done) % BLOCKSIZE);

        struct VIOC *vioc = NULL;
        char        *buf  = NULL;
        uint32_t     blocks = 0;
        if( !$SUCCESSFUL( accesschunk( fcb, vbn, &vioc, &buf, &blocks, 0 )) )
            break;
        if( blocks == 0 ) blocks = 1;
        size_t avail = (size_t)blocks * BLOCKSIZE - block_off;
        size_t want  = count - done;
        size_t n     = avail < want ? avail : want;
        memcpy( (char *)dst + done, buf + block_off, n );
        deaccesschunk( vioc, 0, 0, 1 );
        done += n;
    }
    return done;
}

/* Convenience: read 1, 2 bytes */
static int raw_byte( struct FCB *fcb, off_t off, uint8_t *out ) {
    return raw_read( fcb, off, out, 1 ) == 1;
}
static int raw_word_le( struct FCB *fcb, off_t off, uint16_t *out ) {
    uint8_t b[2];
    if( raw_read( fcb, off, b, 2 ) != 2 ) return 0;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

/* ------------------------------------------------------ decoder */

struct dynbuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
};

static int dyn_push( struct dynbuf *b, const void *src, size_t n ) {
    if( b->len + n > b->cap ) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while( ncap < b->len + n ) ncap *= 2;
        uint8_t *nd = realloc( b->data, ncap );
        if( !nd ) return 0;
        b->data = nd;
        b->cap  = ncap;
    }
    memcpy( b->data + b->len, src, n );
    b->len += n;
    return 1;
}

static int dyn_byte( struct dynbuf *b, uint8_t v ) {
    return dyn_push( b, &v, 1 );
}

/* Decode the whole file into 'out'.  Returns 1 on success. */
static int decode_full( struct FCB *fcb, struct dynbuf *out ) {
    uint32_t efblk = F11SWAP( fcb->head->fh2$w_recattr.fat$l_efblk );
    uint16_t ffb   = F11WORD( fcb->head->fh2$w_recattr.fat$w_ffbyte );
    off_t    fsize = (efblk == 0) ? 0
                                  : (off_t)(efblk - 1) * BLOCKSIZE + ffb;

    uint8_t  rtype   = fcb->head->fh2$w_recattr.fat$b_rtype & 0x0fu;
    uint8_t  rattrib = fcb->head->fh2$w_recattr.fat$b_rattrib;
    uint16_t fix_sz  = F11WORD( fcb->head->fh2$w_recattr.fat$w_rsize );
    uint8_t  vfc_sz  = fcb->head->fh2$w_recattr.fat$b_vfcsize;
    int      add_lf  = (rattrib & FAB$M_CR) != 0;
    int      blk_rec = (rattrib & FAB$M_BLK) != 0;

    out->data = NULL;
    out->len  = 0;
    out->cap  = 0;
    if( fsize == 0 )
        return 1;

    switch( rtype ) {
    case FAB$C_STM:
    case FAB$C_STMLF: {
        /* Pure pass-through. */
        uint8_t  buf[4096];
        off_t    off = 0;
        while( off < fsize ) {
            size_t n = raw_read( fcb, off,
                                 buf,
                                 (size_t)((fsize - off) > (off_t)sizeof buf
                                          ? sizeof buf
                                          : fsize - off) );
            if( n == 0 ) break;
            if( !dyn_push( out, buf, n ) ) return 0;
            off += (off_t)n;
        }
        return 1;
    }

    case FAB$C_STMCR: {
        /* CR -> LF. */
        uint8_t  buf[4096];
        off_t    off = 0;
        while( off < fsize ) {
            size_t n = raw_read( fcb, off, buf,
                                 (size_t)((fsize - off) > (off_t)sizeof buf
                                          ? sizeof buf
                                          : fsize - off) );
            if( n == 0 ) break;
            for( size_t i = 0; i < n; ++i )
                if( buf[i] == '\r' ) buf[i] = '\n';
            if( !dyn_push( out, buf, n ) ) return 0;
            off += (off_t)n;
        }
        return 1;
    }

    case FAB$C_FIX: {
        if( fix_sz == 0 ) return 1;
        uint8_t *rec = malloc( fix_sz );
        if( !rec ) return 0;
        for( off_t off = 0; off + fix_sz <= fsize; off += fix_sz ) {
            if( raw_read( fcb, off, rec, fix_sz ) != fix_sz ) break;
            if( !dyn_push( out, rec, fix_sz ) ) { free(rec); return 0; }
            if( add_lf && !dyn_byte( out, '\n' ) ) { free(rec); return 0; }
        }
        free( rec );
        return 1;
    }

    case FAB$C_VAR:
    case FAB$C_VFC: {
        off_t off = 0;
        while( off + 2 <= fsize ) {
            /* When records can't span block boundaries and the prefix
             * doesn't fit in this block, skip to the next 512-aligned
             * boundary. */
            if( blk_rec && (off % BLOCKSIZE) > BLOCKSIZE - 2 ) {
                off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
                continue;
            }
            uint16_t rsize;
            if( !raw_word_le( fcb, off, &rsize ) ) break;

            /* 0xffff is the standard "rest of block is padding" mark. */
            if( rsize == 0xffff ) {
                off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
                continue;
            }
            off += 2;
            if( rsize == 0 ) {
                /* Empty record: still emit LF when CR attribute set. */
                if( add_lf && !dyn_byte( out, '\n' ) ) return 0;
                /* Padded to even length (no body, but advance 0). */
                if( off & 1 ) off += 1;
                continue;
            }

            uint16_t skip = (rtype == FAB$C_VFC) ? vfc_sz : 0;
            if( skip > rsize ) skip = rsize;

            /* Skip the VFC fixed control header. */
            off_t body_off = off + skip;
            uint16_t body_len = rsize - skip;

            /* If FAB$M_BLK and body would span boundary, the file is
             * malformed in textmode terms; fall back to truncating at
             * the boundary. */
            if( blk_rec && (body_off / BLOCKSIZE) !=
                ((body_off + body_len - 1) / BLOCKSIZE) ) {
                /* leave body_len as is; raw_read will gracefully give
                 * what it can. */
            }

            uint8_t *rec = malloc( body_len ? body_len : 1 );
            if( !rec ) return 0;
            size_t got = raw_read( fcb, body_off, rec, body_len );
            if( got > 0 && !dyn_push( out, rec, got ) ) { free(rec); return 0; }
            free( rec );
            if( add_lf && !dyn_byte( out, '\n' ) ) return 0;

            off += rsize;
            if( off & 1 ) off += 1;        /* pad to word boundary */
        }
        return 1;
    }

    default: {
        /* Undefined or unknown record type: pass the raw bytes through,
         * same as STMLF.  Better than dropping the file silently. */
        uint8_t  buf[4096];
        off_t    off = 0;
        while( off < fsize ) {
            size_t n = raw_read( fcb, off, buf,
                                 (size_t)((fsize - off) > (off_t)sizeof buf
                                          ? sizeof buf
                                          : fsize - off) );
            if( n == 0 ) break;
            if( !dyn_push( out, buf, n ) ) return 0;
            off += (off_t)n;
        }
        return 1;
    }
    }
}

/* ------------------------------------------------------ size cache */

struct cache_entry {
    struct fiddef       fid;
    uint8_t            *data;
    size_t              len;
    struct cache_entry *next;
};

static struct cache_entry *cache_head = NULL;

static int fid_eq( const struct fiddef *a, const struct fiddef *b ) {
    return a->fid$w_num == b->fid$w_num &&
           a->fid$w_seq == b->fid$w_seq &&
           a->fid$b_rvn == b->fid$b_rvn &&
           a->fid$b_nmx == b->fid$b_nmx;
}

static struct cache_entry *cache_lookup( const struct fiddef *fid ) {
    for( struct cache_entry *e = cache_head; e; e = e->next )
        if( fid_eq( &e->fid, fid ) )
            return e;
    return NULL;
}

static struct cache_entry *cache_insert( const struct fiddef *fid,
                                         uint8_t *data, size_t len ) {
    struct cache_entry *e = malloc( sizeof *e );
    if( !e ) { free( data ); return NULL; }
    e->fid  = *fid;
    e->data = data;
    e->len  = len;
    e->next = cache_head;
    cache_head = e;
    return e;
}

void recfmt_clear_cache( void ) {
    while( cache_head ) {
        struct cache_entry *e = cache_head;
        cache_head = e->next;
        free( e->data );
        free( e );
    }
}

/* Returns the cached entry, decoding on first access. */
static struct cache_entry *get_or_decode( struct FCB *fcb ) {
    struct cache_entry *e = cache_lookup( &fcb->head->fh2$w_fid );
    if( e )
        return e;
    struct dynbuf out = { 0 };
    if( !decode_full( fcb, &out ) ) {
        free( out.data );
        return NULL;
    }
    return cache_insert( &fcb->head->fh2$w_fid, out.data, out.len );
}

/* ------------------------------------------------------ public API */

int recfmt_textmode_eligible( const struct HEAD *head ) {
    /* Skip directories: they are technically VAR but we never want to
     * present them as text. */
    if( F11LONG( head->fh2$l_filechar ) & FH2$M_DIRECTORY )
        return 0;
    uint8_t rtype = head->fh2$w_recattr.fat$b_rtype & 0x0fu;
    /* Anything except UDF is legal; UDF means "no record structure on
     * disk", which we handle by passing through anyway, so allow it. */
    (void)rtype;
    return 1;
}

ssize_t recfmt_logical_size( struct FCB *fcb ) {
    struct cache_entry *e = get_or_decode( fcb );
    if( !e ) return -1;
    return (ssize_t)e->len;
}

ssize_t recfmt_read( struct FCB *fcb, off_t off, char *buf, size_t size ) {
    struct cache_entry *e = get_or_decode( fcb );
    if( !e ) return -1;
    if( off < 0 || (size_t)off >= e->len )
        return 0;
    size_t avail = e->len - (size_t)off;
    if( size > avail ) size = avail;
    memcpy( buf, e->data + off, size );
    return (ssize_t)size;
}
