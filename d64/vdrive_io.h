/* Mailbox register engine for the TR+ virtual drive.
 *
 *   IO1 ($DE00): +0 CMD (W: trigger)  +1 ARG (RW)  +2 DATA (R)  +3 STAT (R)  +4 ST (R)  +5 LEN (RW)
 *   IO2 ($DF00): 256-byte window (RW): file name in, data blocks in/out
 *
 * ISR side (vio_io*_read/write) is O(1) and never touches storage. Everything
 * else runs from vio_poll() in the main loop. Portable C: host tests and the
 * Teensy glue (IOH_VDrive.c) use the same code. */
#ifndef VDRIVE_IO_H
#define VDRIVE_IO_H
#include "vdrive.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VIO_CMD = 0, VIO_ARG = 1, VIO_DATA = 2, VIO_STAT = 3, VIO_ST = 4, VIO_LEN = 5, VIO_NREGS = 6 };
#define VIO_BUSY    0x80
#define VIO_NOTMINE 0x40

/* Commands 1..8 are VD_LISTEN..VD_UNTLK (byte-level). Fast paths: */
enum {
    VIO_FLOAD_OPEN = 9,    /* ARG=device, window[0..LEN) = name. DATA: 0 ok (window[0..1] = load address), 4 not found, 5 no device */
    VIO_FLOAD_BLK  = 10,   /* DATA = byte count (0..255) now in window; ST=$40 on the last block */
    VIO_FSAVE_OPEN = 11,   /* ARG=device, window[0..LEN) = name. DATA: 0 ok else DOS error code (63, 72, ...) or 5 */
    VIO_FSAVE_BLK  = 12,   /* LEN bytes from window are appended to the file */
    VIO_FSAVE_CLOSE= 13    /* DATA: 0 ok else DOS error code */
};

typedef struct {
    vdrive_t vd;
    volatile uint8_t cmd, arg, data, stat, st, len, pending;
    uint8_t window[256];
    uint8_t fl_active, fl_eof, fl_dev, fs_active, fs_dev;
} vio_t;

void    vio_init(vio_t *v);
uint8_t vio_io1_read(const vio_t *v, uint8_t off);
void    vio_io1_write(vio_t *v, uint8_t off, uint8_t val);
uint8_t vio_io2_read(const vio_t *v, uint8_t off);
void    vio_io2_write(vio_t *v, uint8_t off, uint8_t val);
int     vio_poll(vio_t *v);                  /* 1 if a command was processed */

size_t  vio_sizeof(void);

#ifdef __cplusplus
}
#endif
#endif
