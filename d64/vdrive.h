/* Virtual IEC drive: the Teensy-side state machine behind the KERNAL mailbox.
 * Units 0/1 = devices 8/9, each backed by an optional mounted d64_t.
 * One mailbox command in, one result out; no I/O other than the d64 and host callbacks. */
#ifndef VDRIVE_H
#define VDRIVE_H
#include "d64.h"
#include "vd_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VD_LISTEN = 1, VD_TALK, VD_SECOND, VD_TKSA, VD_CIOUT, VD_ACPTR, VD_UNLSN, VD_UNTLK };

#define VD_CMDBUF 64
#define VD_HFBUF  64      /* chunk size of the host text file channels */

/* Directory listings are generated a line at a time: no listing buffer. */
typedef struct {
    d64_dir_t it;               /* image directory iterator (unused for host listings) */
    char      pat[17];
    uint8_t   phase;            /* 0 header, 1 entries, 2 footer, 3 done */
    uint8_t   host;             /* listing the host directory (nothing mounted) */
    uint8_t   has_pat;
    uint8_t   lpos, llen;       /* cursor / length of line[] */
    uint16_t  off;              /* bytes emitted so far, load address included */
    uint8_t   line[48];
} vd_dirstream_t;

/* REL channel: the record cursor over a d64_rel_t. One LISTEN session writes one record (the rest is zero-filled at UNLSN);
 * a read sends a record up to its last non-zero byte, with EOI, then moves to the next. */
typedef struct {
    d64_rel_t f;
    uint16_t  rec;              /* current record, 0-based */
    uint8_t   pos;              /* byte within it */
    uint8_t   rlen;             /* bytes a read of the current record sends (0 = not worked out yet) */
    uint8_t   wrote;            /* bytes came in during this LISTEN session */
    uint8_t   over;             /* 1: this record overflowed (51 reported), 2: it could not be made (52 / 72) */
} vd_rel_t;

/* A plain file on the card (nothing mounted): read through the host in 256-byte pieces. */
typedef struct {
    uint32_t left;              /* bytes not yet fetched from the host */
    uint16_t pos, len;          /* cursor / fill of buf */
    uint8_t  buf[D64_SECTOR];
} vd_plain_t;

typedef struct {
    uint8_t  mode;              /* 0 free, 1 file read, 3 file write, 4 directory stream, 5 direct-access buffer (OPEN n,d,n,"#"),
                                   6 / 7 host text file read / write (OPEN n,d,n,"!CONFIG[,W]" / "!STATE"), 8 REL, 9 plain file on the card */
    union { d64_file_t r; d64_wfile_t w; vd_dirstream_t d; struct { uint16_t pos; uint8_t buf[D64_SECTOR]; } b;
            struct { uint8_t kind, pos, len; uint8_t buf[VD_HFBUF]; } h; vd_rel_t l; vd_plain_t p; } u;
} vd_chan_t;

/* Services the host (Teensy glue / test harness) provides for image selection.
 * Paths are passed through unconverted (PETSCII); the host converts. All optional. */
typedef struct {
    void *ctx;
    int  (*mount)(void *ctx, int unit, const char *name, d64_t **out);   /* 0 ok, else DOS error code (e.g. 62) */
    void (*unmount)(void *ctx, int unit);
    int  (*chdir)(void *ctx, int unit, const char *name);                /* "..", subdir; 0 ok else DOS error */
    int  (*dir_open)(void *ctx, int unit, char title[17]);               /* 0 ok */
    int  (*dir_next)(void *ctx, int unit, char name[17], uint32_t *size, int *is_dir); /* 1 entry, 0 end */
    void (*save_devices)(void *ctx, uint8_t pin0, uint8_t pin1);        /* persist after a U0> renumber (0 = automatic) */
    /* A directory listing by path ($/GAMES/G.D64, $GAMES/) must not change what is selected: push remembers the unit's folder and
     * mounted image before the walk, pop (when the listing channel closes) goes back; *out = the image to use again (NULL = none).
     * pop with out == NULL means "keep the new selection": the snapshot is dropped (a file was loaded through the path).
     * Both optional; without them a listing by path leaves the new folder / image selected. $==path never pops. */
    void (*push)(void *ctx, int unit);
    int  (*pop)(void *ctx, int unit, d64_t **out);
    /* Host text files for the configuration utility (OPEN n,d,n,"!CONFIG" read, "!CONFIG,W" write, "!STATE" read only): the unit's disk
     * switching list (/vdrive-config.txt) and a two-line description of what the unit has mounted ("path", "folder"). The host
     * converts the text to / from PETSCII (a line ends with CR on the C64 side). Only one such file is open at a time on the host side.
     * hf_open: kind VD_HF_CONFIG / VD_HF_STATE; 0 ok, else a DOS error code (62 when there is no such file to read). hf_read: up to n bytes,
     * 0 = end of file. hf_write: 0 ok. hf_close: after the last write the host should flush. All optional (a missing hf_open = 62). */
    int  (*hf_open)(void *ctx, int unit, int kind, int write);
    int  (*hf_read)(void *ctx, int unit, uint8_t *buf, int n);
    int  (*hf_write)(void *ctx, int unit, const uint8_t *buf, int n);
    void (*hf_close)(void *ctx, int unit, int write);
    /* "N:NAME.D64,ID" with nothing mounted: make a new all-zero image file of `size` bytes in the current folder (never over an
     * existing file: 63) and mount it like mount(); vdrive.c then writes the header and BAM. 0 ok, else a DOS error code. */
    int  (*create)(void *ctx, int unit, const char *name, uint32_t size, d64_t **out);
    /* A plain file in the current folder, read with nothing mounted (LOAD"GAME.PRG"): one open at a time per unit.
     * plain_open: 0 ok (*size = its length) or a DOS error (62); plain_read: bytes read (<0 error); plain_close: done with it. */
    int  (*plain_open)(void *ctx, int unit, const char *name, uint32_t *size);
    int  (*plain_read)(void *ctx, int unit, uint8_t *buf, uint16_t n);
    void (*plain_close)(void *ctx, int unit);
    /* MD:NAME / RD:NAME with nothing mounted: make / remove (only if empty) a folder in the current folder. 0 ok, else a DOS error:
     * mkdir 63 if the name is taken; rmdir 62 if there is no such folder, 63 if it is not empty. */
    int  (*mkdir)(void *ctx, int unit, const char *name);
    int  (*rmdir)(void *ctx, int unit, const char *name);
} vd_host_t;
enum { VD_HF_CONFIG = 0, VD_HF_STATE = 1 };

struct vdrive_s;
typedef struct {
    struct vdrive_s *owner;     /* back pointer: closing a channel may have to restore the host state */
    uint8_t  index;             /* this unit's number (0/1) */
    uint8_t  restore_ch;        /* channel + 1 whose close restores the folder / image selected before a path listing; 0 = none */
    d64_t   *disk;              /* NULL: no image mounted */
    uint8_t  present;           /* device answers on the bus */
    uint8_t  slot_of[16];       /* channel number -> pool slot + 1 (0 = not open) */
    vd_chan_t slot[VD_NSLOT];
    uint8_t  statbuf[48];       /* error channel (15) text */
    uint8_t  stat_len, stat_pos;
    uint8_t  err, err_t, err_s; /* DOS status */
} vd_unit_t;

typedef struct vdrive_s {
    vd_host_t host;
    vd_unit_t unit[2];
    uint8_t  dev[2];            /* IEC device number each unit answers to now; 0 = none (not claimed on the bus) */
    uint8_t  pin[2];            /* the number the user chose with U0> (kept in EEPROM); 0 = automatic */
    int8_t   act;               /* -1 none, 0/1 unit, 2 = absent device */
    uint8_t  talking;
    uint8_t  sa;
    uint8_t  wlen;
    uint8_t  wbuf[VD_CMDBUF];
} vdrive_t;

void vdrive_init(vdrive_t *v);
void vdrive_attach(vdrive_t *v, int unit, d64_t *disk, int present);
/* pin0/pin1: device numbers the user fixed (8..30), 0 = automatic. Automatic units start on VD_DEV0/VD_DEV1 (or the next
 * free number) and are moved by the start-up scan below. */
void vdrive_set_devices(vdrive_t *v, uint8_t pin0, uint8_t pin1);
/* Start-up scan (the KERNAL probes the real serial bus for 8,9,... after reset):
 *   scan_begin  releases the numbers of all automatic units, so the probes reach the real bus; returns how many present
 *               units still need a number (0 = nothing to scan for).
 *   scan_free   the probe found nothing at `dev`: give it to the next automatic present unit (SD first, then USB).
 *               Returns how many units are still waiting. */
int  vdrive_scan_begin(vdrive_t *v);
int  vdrive_scan_free(vdrive_t *v, uint8_t dev);
int  vdrive_unit_for(const vdrive_t *v, uint8_t dev);                /* unit 0/1 serving this device number, or -1 */
void vdrive_set_host(vdrive_t *v, const vd_host_t *host);
int  vdrive_chan_mode(const vdrive_t *v, int unit, int ch);   /* 0 closed, 1 file-read, 3 file-write, 4 directory, 5 buffer, 6/7 host text, 8 REL, 9 plain file */
int  vdrive_active(const vdrive_t *v);                        /* unit of the open bus transaction: 0/1, 2 absent device, -1 none */
int  vdrive_pending_err(const vdrive_t *v, int unit);         /* DOS error code waiting on the status channel (0 = none) */

/* Returns 1 if the transaction is not ours (fall back to the real IEC bus);
 * 0 if handled, with data and st filled in (st = bits to OR into KERNAL ST). */
int vdrive_mailbox(vdrive_t *v, uint8_t cmd, uint8_t arg, uint8_t *data, uint8_t *st);

size_t vdrive_sizeof(void);

#ifdef __cplusplus
}
#endif
#endif
