/*
 * catvms - decode VMS RMS record-formatted text files to plain text.
 *
 * On ODS-2 a "text" file is rarely a flat byte stream.  The classic
 * VMS shape for .TXT/.LIS is VAR (variable-length records, each
 * preceded by a 16-bit little-endian length and padded to an even
 * boundary), with the FAB$M_CR record attribute that tells "TYPE" on
 * VMS to emit an implicit LF after every record.  Other shapes turn
 * up too: VFC (VAR with a fixed control header per record), FIX
 * (constant-size records), STMLF / STM / STMCR (already a stream).
 *
 * fuse-ods2 -o textmode decodes these on the fly when the volume is
 * mounted.  catvms lets you decode files that have already been
 * extracted -- for instance copied off the mount in default (raw)
 * mode, or pulled out by ods2 COPY without /ASCII.
 *
 *   catvms file.txt                # autodetect, default VAR fallback
 *   catvms --var      < file.txt   # force VAR
 *   catvms --vfc=2    file.lis     # VFC with 2-byte fixed control header
 *   catvms --fix=80   card.dat     # FIX, 80-byte records
 *   catvms --stmlf    file.txt     # plain stream, no decoding
 *   catvms --stmcr    file.mac     # CR-delimited stream, translate to LF
 *
 * Exit status: 0 on success, 1 on usage error, 2 on I/O / parse error.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCKSIZE 512

enum mode {
    MODE_AUTO = 0,
    MODE_VAR,
    MODE_VFC,
    MODE_FIX,
    MODE_STMLF,
    MODE_STMCR,
};

struct opts {
    enum mode mode;
    int       vfc_size;
    int       fix_size;
    int       no_lf;       /* suppress trailing LF after each record */
};

/* ------------------------------------------------------ slurp input */

static int slurp( FILE *in, uint8_t **out, size_t *out_len ) {
    size_t   cap = 4096, len = 0;
    uint8_t *buf = malloc( cap );
    if( !buf )
        return -1;
    for( ;; ) {
        if( len == cap ) {
            size_t   ncap = cap * 2;
            uint8_t *nb   = realloc( buf, ncap );
            if( !nb ) {
                free( buf );
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        size_t n = fread( buf + len, 1, cap - len, in );
        len += n;
        if( n == 0 ) {
            if( ferror( in ) ) {
                free( buf );
                return -1;
            }
            break;
        }
    }
    *out     = buf;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------ writers */

static int emit( const uint8_t *p, size_t n ) {
    return fwrite( p, 1, n, stdout ) == n ? 0 : -1;
}

static int emit_byte( uint8_t b ) {
    return fputc( b, stdout ) == EOF ? -1 : 0;
}

/* ------------------------------------------------------ decoders */

static int decode_var( const uint8_t *data, size_t len,
                       int vfc, int add_lf ) {
    /* RMS VAR / VFC: 16-bit LE length prefix, body, pad to even.  In
     * FAB$M_BLK files, 0xFFFF or "next prefix doesn't fit" means jump
     * to the next 512-byte boundary.  We assume FAB$M_BLK because the
     * cost is negligible if it isn't set and being wrong about it is
     * the most common failure mode in the wild. */
    size_t off = 0;
    while( off + 2 <= len ) {
        if( BLOCKSIZE - (off % BLOCKSIZE) < 2 ) {
            off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
            continue;
        }
        uint16_t rsize = (uint16_t)( data[off] | ( data[off + 1] << 8 ) );
        if( rsize == 0xFFFFu ) {
            off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
            continue;
        }
        off += 2;
        if( rsize == 0 ) {
            if( add_lf && emit_byte( '\n' ) < 0 ) return -1;
            if( off & 1 ) off += 1;
            continue;
        }
        if( off + rsize > len )
            break;     /* truncated */

        size_t skip = (size_t)( vfc > 0 ? vfc : 0 );
        if( skip > rsize ) skip = rsize;

        if( rsize - skip > 0 ) {
            if( emit( data + off + skip, rsize - skip ) < 0 ) return -1;
        }
        if( add_lf && emit_byte( '\n' ) < 0 ) return -1;

        off += rsize;
        if( off & 1 ) off += 1;
    }
    return 0;
}

static int decode_fix( const uint8_t *data, size_t len,
                       size_t reclen, int add_lf ) {
    if( reclen == 0 ) {
        fprintf( stderr, "catvms: --fix needs a non-zero record size\n" );
        return -1;
    }
    for( size_t off = 0; off + reclen <= len; off += reclen ) {
        if( emit( data + off, reclen ) < 0 ) return -1;
        if( add_lf && emit_byte( '\n' ) < 0 ) return -1;
    }
    return 0;
}

static int decode_stmlf( const uint8_t *data, size_t len ) {
    return emit( data, len );
}

static int decode_stmcr( const uint8_t *data, size_t len ) {
    /* CR -> LF; leave anything else alone, including embedded LFs that
     * a few "stream" producers put in by mistake. */
    for( size_t i = 0; i < len; ++i ) {
        uint8_t b = data[i] == '\r' ? (uint8_t)'\n' : data[i];
        if( emit_byte( b ) < 0 ) return -1;
    }
    return 0;
}

/* ------------------------------------------------------ autodetect */

/* Try parsing the first ~4 KB as VAR.  Cleanly-parsing means: every
 * record length is plausible (>0, <= 32767), records sit inside the
 * buffer or hit a 0xFFFF block-pad sentinel, and the bodies are mostly
 * printable bytes.  The check is deliberately generous: a single bad
 * record kicks us out and we fall back to STMLF. */
static int looks_like_var( const uint8_t *data, size_t len ) {
    size_t cap     = len < 4096 ? len : 4096;
    size_t off     = 0;
    size_t records = 0;
    size_t printable = 0, total_body = 0;

    while( off + 2 <= cap ) {
        if( BLOCKSIZE - (off % BLOCKSIZE) < 2 ) {
            off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
            continue;
        }
        uint16_t rsize = (uint16_t)( data[off] | ( data[off + 1] << 8 ) );
        if( rsize == 0xFFFFu ) {
            off = ((off / BLOCKSIZE) + 1) * BLOCKSIZE;
            continue;
        }
        off += 2;
        if( rsize > 32767u ) return 0;
        if( off + rsize > len ) return 0;
        for( size_t k = 0; k < rsize; ++k ) {
            uint8_t c = data[off + k];
            if( c == '\t' || c == '\r' || c == '\n' || ( c >= 0x20 && c < 0x7f ) )
                printable++;
            total_body++;
        }
        off += rsize;
        if( off & 1 ) off += 1;
        records++;
        if( records >= 32 ) break;
    }
    if( records == 0 || total_body == 0 ) return 0;
    /* 90% printable is enough to distinguish VAR-text from a binary file
     * that happens to begin with a small length-shaped word. */
    return printable * 10 >= total_body * 9;
}

static enum mode autodetect( const uint8_t *data, size_t len ) {
    if( looks_like_var( data, len ) )
        return MODE_VAR;
    /* CR-only line endings: more CRs than LFs and at least one CR. */
    size_t crs = 0, lfs = 0;
    size_t scan = len < 4096 ? len : 4096;
    for( size_t i = 0; i < scan; ++i ) {
        if( data[i] == '\r' ) crs++;
        if( data[i] == '\n' ) lfs++;
    }
    if( crs > 0 && crs > lfs ) return MODE_STMCR;
    return MODE_STMLF;
}

/* ------------------------------------------------------ CLI */

static void usage( FILE *fp, const char *prog ) {
    fprintf( fp,
        "usage: %s [options] [file]\n"
        "\n"
        "Decode a VMS RMS record-formatted file to plain text.\n"
        "Reads from stdin if no file is given.\n"
        "\n"
        "options:\n"
        "  --auto         autodetect record format (default)\n"
        "  --var          VAR records (16-bit LE length prefix, pad to even)\n"
        "  --vfc=N        VFC records (VAR with N-byte fixed control header)\n"
        "  --fix=N        FIX records of N bytes each\n"
        "  --stmlf | --stm  pass through (already a stream)\n"
        "  --stmcr        stream with CR delimiters; translate CR -> LF\n"
        "  --no-lf        do not append LF after each record\n"
        "  -h, --help     show this help\n",
        prog );
}

static int parse_int_arg( const char *s, int *out ) {
    char *end = NULL;
    long  v   = strtol( s, &end, 10 );
    if( end == s || *end != '\0' || v < 0 || v > 65535 )
        return -1;
    *out = (int)v;
    return 0;
}

int main( int argc, char *argv[] ) {
    struct opts o = { .mode = MODE_AUTO, .vfc_size = 0, .fix_size = 0,
                      .no_lf = 0 };
    const char *path = NULL;

    for( int i = 1; i < argc; ++i ) {
        const char *a = argv[i];
        if( strcmp( a, "-h" ) == 0 || strcmp( a, "--help" ) == 0 ) {
            usage( stdout, argv[0] );
            return 0;
        } else if( strcmp( a, "--auto" ) == 0 ) {
            o.mode = MODE_AUTO;
        } else if( strcmp( a, "--var" ) == 0 ) {
            o.mode = MODE_VAR;
        } else if( strncmp( a, "--vfc=", 6 ) == 0 ) {
            if( parse_int_arg( a + 6, &o.vfc_size ) < 0 ) {
                fprintf( stderr, "catvms: bad --vfc value '%s'\n", a + 6 );
                return 1;
            }
            o.mode = MODE_VFC;
        } else if( strncmp( a, "--fix=", 6 ) == 0 ) {
            if( parse_int_arg( a + 6, &o.fix_size ) < 0 || o.fix_size == 0 ) {
                fprintf( stderr, "catvms: bad --fix value '%s'\n", a + 6 );
                return 1;
            }
            o.mode = MODE_FIX;
        } else if( strcmp( a, "--stmlf" ) == 0 || strcmp( a, "--stm" ) == 0 ) {
            o.mode = MODE_STMLF;
        } else if( strcmp( a, "--stmcr" ) == 0 ) {
            o.mode = MODE_STMCR;
        } else if( strcmp( a, "--no-lf" ) == 0 ) {
            o.no_lf = 1;
        } else if( a[0] == '-' && a[1] != '\0' ) {
            fprintf( stderr, "catvms: unknown option '%s'\n", a );
            usage( stderr, argv[0] );
            return 1;
        } else if( path == NULL ) {
            path = a;
        } else {
            fprintf( stderr, "catvms: too many file arguments\n" );
            return 1;
        }
    }

    FILE *in = stdin;
    if( path != NULL ) {
        in = fopen( path, "rb" );
        if( in == NULL ) {
            fprintf( stderr, "catvms: %s: %s\n", path, strerror( errno ) );
            return 2;
        }
    }

    uint8_t *data = NULL;
    size_t   len  = 0;
    if( slurp( in, &data, &len ) < 0 ) {
        fprintf( stderr, "catvms: read failed: %s\n", strerror( errno ) );
        if( in != stdin ) fclose( in );
        return 2;
    }
    if( in != stdin ) fclose( in );

    enum mode m = o.mode;
    if( m == MODE_AUTO )
        m = autodetect( data, len );

    int rc      = 0;
    int add_lf  = !o.no_lf;
    switch( m ) {
    case MODE_VAR:
        rc = decode_var( data, len, 0, add_lf );
        break;
    case MODE_VFC:
        rc = decode_var( data, len, o.vfc_size, add_lf );
        break;
    case MODE_FIX:
        rc = decode_fix( data, len, (size_t)o.fix_size, add_lf );
        break;
    case MODE_STMLF:
        rc = decode_stmlf( data, len );
        break;
    case MODE_STMCR:
        rc = decode_stmcr( data, len );
        break;
    case MODE_AUTO:
        /* unreachable: autodetect resolved above */
        rc = decode_stmlf( data, len );
        break;
    }

    free( data );
    if( fflush( stdout ) != 0 ) rc = -1;
    return rc < 0 ? 2 : 0;
}
