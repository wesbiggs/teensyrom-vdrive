/* Disk image engine (read + write) for .d64 (1541, 35/40 tracks), .d71 (1571, 70 tracks) and .d81 (1581, 80 tracks).
 * Portable C99, no malloc: all state lives in caller structs and sectors are fetched through a callback, so the same
 * code runs on the Teensy (SD/USB file backend) and on the host (tests, VICE mock). Names keep the d64_ prefix. */
#ifndef D64_H
#define D64_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D64_SECTOR 256

/* Fetch one 256-byte sector. Return 0 on success. */
typedef int (*d64_read_fn)(void *ctx, uint8_t track, uint8_t sector, uint8_t *buf);

/* Store one 256-byte sector. Return 0 on success. NULL = read-only disk. */
typedef int (*d64_write_fn)(void *ctx, uint8_t track, uint8_t sector, const uint8_t *buf);

enum {
    D64_OK = 0,
    D64_E_IO = -1,       /* sector read/write failed or link out of range */
    D64_E_LOOP = -2,     /* chain loop */
    D64_E_FULL = -3,     /* disk (or directory) full   -> DOS 72 */
    D64_E_EXISTS = -4,   /* file exists                -> DOS 63 */
    D64_E_NAME = -5,     /* bad file name              -> DOS 33 */
    D64_E_RO = -6,       /* read-only disk             -> DOS 26 */
    D64_E_LOCKED = -7,   /* file is locked             -> DOS 26 */
    D64_E_NOTFOUND = -8  /* file not found             -> DOS 62 */
};

enum { D64_FMT_D64 = 0, D64_FMT_D71 = 1, D64_FMT_D81 = 2 };

typedef struct {
    d64_read_fn  read;
    d64_write_fn write;
    void       *ctx;
    uint8_t     tracks;          /* 35 or 40 (d64), 70 (d71), 80 (d81) */
    uint8_t     fmt;             /* D64_FMT_* */
} d64_t;

typedef struct {
    uint8_t  type;               /* raw type byte: bit7 closed, bit6 locked, low 3 = DEL/SEQ/PRG/USR/REL */
    uint8_t  track, sector;      /* first data sector */
    uint16_t blocks;
    char     name[17];           /* PETSCII, $A0 padding stripped, NUL terminated */
    uint8_t  dir_t, dir_s, dir_slot;   /* where this entry lives (for update/scratch) */
} d64_entry_t;

typedef struct {
    uint8_t track, sector;       /* next directory sector in chain */
    uint8_t cur_t, cur_s;        /* sector held in buf */
    uint8_t slot;                /* next entry (0-7) in buf */
    uint8_t guard;
    uint8_t buf[D64_SECTOR];
} d64_dir_t;

typedef struct {
    uint8_t  next_t, next_s;     /* next sector in chain (0,x = none) */
    uint8_t  loaded;             /* buf holds a sector */
    uint16_t pos, end;           /* read cursor / one past last valid byte in buf */
    uint16_t guard;
    uint8_t  buf[D64_SECTOR];
} d64_file_t;

typedef struct {
    uint8_t  type;               /* DEL/SEQ/PRG/USR (low 3 bits) */
    uint8_t  dir_t, dir_s, dir_slot;
    uint8_t  first_t, first_s;
    uint8_t  cur_t, cur_s;       /* sector being filled */
    uint16_t pos;                /* next free index in buf (2..256) */
    uint16_t blocks;             /* sectors allocated so far */
    uint8_t  buf[D64_SECTOR];
} d64_wfile_t;

/* Memory-image backend (host tests / mock). Writable if data is. */
typedef struct { uint8_t *data; size_t len; } d64_mem_t;
void d64_mem_init(d64_mem_t *m, uint8_t *data, size_t len);
int  d64_open_mem(d64_t *d, d64_mem_t *m);          /* image size selects the format */
int  d64_open(d64_t *d, d64_read_fn rd, void *ctx, uint8_t tracks);   /* tracks: 35/40 = d64, 70 = d71, 80 = d81 */
int  d64_open_size(d64_t *d, d64_read_fn rd, void *ctx, size_t file_size);   /* format from the file size (error bytes allowed); -1 if too small */

int  d64_sectors_in_track(const d64_t *d, uint8_t track);   /* 0 if invalid */
long d64_sector_offset(const d64_t *d, uint8_t track, uint8_t sector); /* byte offset in image, -1 if invalid */

/* Disk header: name (16 PETSCII, stripped), id (2), dos type (2), NUL terminated. */
int  d64_header(d64_t *d, char name[17], char id[3], char dos[3]);
int  d64_blocks_free(d64_t *d);                     /* 1541 convention: track 18 not counted */

int  d64_dir_first(d64_t *d, d64_dir_t *it);
int  d64_dir_next(d64_t *d, d64_dir_t *it, d64_entry_t *e);   /* 1 entry, 0 end, <0 error */

/* CBM wildcard match: '?' any one char, '*' rest of name. */
int  d64_match(const char *name, const char *pattern);
int  d64_find(d64_t *d, const char *pattern, d64_entry_t *e); /* 1 found, 0 not found, <0 error */

int  d64_file_open(d64_t *d, const d64_entry_t *e, d64_file_t *f);
int  d64_file_read(d64_t *d, d64_file_t *f, uint8_t *out, size_t n); /* bytes read; 0 = EOF; <0 error */
int  d64_file_eof(const d64_file_t *f);             /* 1 when no more bytes will follow */

/* BASIC directory listing (load address $0401), optional name pattern (NULL = all).
 * Returns length written, or 0 if cap is too small. */
size_t d64_build_dir(d64_t *d, const char *pattern, uint8_t *out, size_t cap);

/* ---- write support ----
 * d64_create allocates the first sector and a directory slot (file shows as
 * unclosed "splat" until d64_close_write). replace!=0 scratches an existing
 * file of that name first (DOS "@:"). */
int  d64_create(d64_t *d, const char *name, uint8_t type, int replace, d64_wfile_t *w);
int  d64_write(d64_t *d, d64_wfile_t *w, const uint8_t *data, size_t n); /* n or <0; on D64_E_FULL the file is removed */
int  d64_close_write(d64_t *d, d64_wfile_t *w);
int  d64_scratch_entry(d64_t *d, const d64_entry_t *e);          /* frees chain, marks entry deleted */
int  d64_scratch(d64_t *d, const char *pattern);                 /* returns files scratched or <0 */
int  d64_rename(d64_t *d, const char *oldname, const char *newname);
int  d64_format(d64_t *d, const char *name, const char *id);     /* id==NULL: quick (BAM + directory only) */

/* ---- raw blocks (direct access: U1/U2, B-A, B-F) ----
 * Sector numbers are checked against the geometry: D64_E_IO for an illegal track/sector or a failed transfer. */
int  d64_block_read(d64_t *d, uint8_t t, uint8_t s, uint8_t *buf);
int  d64_block_write(d64_t *d, uint8_t t, uint8_t s, const uint8_t *buf);   /* D64_E_RO on a read-only image */
/* Mark t/s used in the BAM. 0 = allocated; D64_E_EXISTS = already used, nt and ns = the next free sector (0/0 = none);
 * D64_E_IO = illegal. */
int  d64_block_alloc(d64_t *d, uint8_t t, uint8_t s, uint8_t *nt, uint8_t *ns);
int  d64_block_free(d64_t *d, uint8_t t, uint8_t s);                          /* mark t/s free in the BAM */

/* Generic BASIC-listing builder (also used for host directory listings). */
typedef struct { uint8_t *p; size_t cap, len, base; uint16_t link_at; int ok; } d64_bas_t;
void   d64_bas_begin(d64_bas_t *b, uint8_t *out, size_t cap);
/* Incremental form: build one line at a time into a small buffer. base = bytes already emitted
 * (load address included) so the line link pointers stay right; base==0 also emits the load address. */
void   d64_bas_begin_at(d64_bas_t *b, uint8_t *out, size_t cap, size_t base);
void   d64_bas_header(d64_bas_t *b, const char *name, const char *id, const char *dos);
void   d64_bas_entry(d64_bas_t *b, uint16_t blocks, const char *name, const char *type3, int closed, int locked);
void   d64_bas_footer(d64_bas_t *b, uint16_t free_blocks);
size_t d64_bas_end(d64_bas_t *b);                    /* length, or 0 if the buffer overflowed */

size_t d64_sizeof_d64(void);  size_t d64_sizeof_mem(void);
size_t d64_sizeof_dir(void);  size_t d64_sizeof_wfile(void);  size_t d64_sizeof_file(void);  size_t d64_sizeof_entry(void);

#ifdef __cplusplus
}
#endif
#endif
