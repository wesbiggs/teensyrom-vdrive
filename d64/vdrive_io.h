/* Mailbox register engine for the TR+ virtual drive.
 *
 *   IO1 ($DE10, offsets below are relative to it): +0 CMD (W: trigger)  +1 ARG (RW)  +2 DATA (R)  +3 STAT (R)  +4 ST (R)  +5 LEN (RW)
 *                                                  +6 DEV0 (R)  +7 DEV1 (R): IEC device number of the SD / USB drive now (0 = none)
 *                                                  +8 BORDER (R): request for the KERNAL's IRQ routine, see VIO_BRD_* (disk-switch feedback)
 *   IO2 ($DF00): 256-byte window (RW): file name in, data blocks in/out
 *
 * ISR side (vio_io*_read/write) is O(1) and never touches storage. Everything
 * else runs from vio_poll() in the main loop. Portable C: host tests and the
 * Teensy glue (IOH_VDrive.c) use the same code.
 *
 * Everything the bus ISR touches lives in the small vio_hot_t, which the firmware keeps in fast RAM1
 * (DTCM); the large drive state (vio_t) can sit in slower RAM2 because only the main loop uses it.
 *
 * STAT is a handshake token, not a flag byte: BUSY ($80) from the moment CMD is written, then
 * OK ($5A) or NOTMINE ($A5) when the answer is complete. The 6502 waits for exactly one of the two, so a
 * floating-bus misread (which returns the previous bus byte, $DE here) can never look like "done".
 *
 * Bus scan: the Teensy cannot see the serial bus, so when a unit has no fixed device number it asks the 6502 to probe it. It does
 * so lazily, on the first transaction of a run (the VDrive handler only becomes active once the program has been loaded): that
 * command is answered with SCANREQ instead of being executed; the KERNAL sends SCAN, then FREE for each number where its
 * LISTEN found nobody, until the answer says nobody is waiting, and then repeats the original command. Asked once only: a
 * KERNAL that does not understand SCANREQ just times out on that one call and the defaults stay. */
#ifndef VDRIVE_IO_H
#define VDRIVE_IO_H
#include "vdrive.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VIO_CMD = 0, VIO_ARG = 1, VIO_DATA = 2, VIO_STAT = 3, VIO_ST = 4, VIO_LEN = 5, VIO_DEV0 = 6, VIO_DEV1 = 7, VIO_BORDER = 8, VIO_NREGS = 9 };
#define VIO_BUSY    0x80
#define VIO_OK      0x5A
#define VIO_NOTMINE 0xA5
/* BORDER register values (exact: the 6502 reads it twice and ignores anything else, e.g. $DE from a late answer): */
#define VIO_BRD_IDLE    0x80   /* nothing to do */
#define VIO_BRD_SET     0xC0   /* | colour 0..15: set the border to it */
#define VIO_BRD_SAVE    0xB0   /* remember the current border colour */
#define VIO_BRD_RESTORE 0xD0   /* set the border back to the remembered colour */
#define VIO_SCANREQ 0x3C   /* answer to the first LISTEN / TALK / fast LOAD / fast SAVE of a run: "scan the bus, then ask again" (see VIO_SCAN) */

/* Commands 1..8 are VD_LISTEN..VD_UNTLK (byte-level). Fast paths: */
enum {
    VIO_FLOAD_OPEN = 9,    /* ARG=device, window[0..LEN) = name. DATA: 0 ok (window[0..1] = load address), 4 not found, 5 no device */
    VIO_FLOAD_BLK  = 10,   /* DATA = byte count (0..255) now in window; ST=$40 on the last block */
    VIO_FSAVE_OPEN = 11,   /* ARG=device, window[0..LEN) = name. DATA: 0 ok else DOS error code (63, 72, ...) or 5 */
    VIO_FSAVE_BLK  = 12,   /* LEN bytes from window are appended to the file */
    VIO_FSAVE_CLOSE= 13,   /* DATA: 0 ok else DOS error code */
    VIO_SCAN       = 14,   /* start of the reset-time bus scan: releases the automatic units' numbers. DATA = units that need a number */
    VIO_FREE       = 15    /* ARG = a device number nobody answered at; it goes to the next unit that needs one. DATA = units still waiting */
};

/* Registers + window: the only memory the bus ISR reads or writes (~270 bytes). Keep in DTCM. */
typedef struct {
    volatile uint8_t cmd, arg, data, stat, st, len, dev0, dev1, border, pending;   /* cmd..border are in register order (see the static_assert in IOH_VDrive.c) */
    uint8_t window[256];
} vio_hot_t;

/* Drive state and fast-path bookkeeping: main loop only. */
typedef struct {
    vdrive_t vd;                       /* must stay first: callers pass a vio_t* where a vdrive_t* is wanted */
    vio_hot_t *h;
    uint8_t fl_active, fl_eof, fl_dev, fs_active, fs_dev;
    uint8_t scan_state;                /* 0 not needed, 1 to be requested, 2 requested, 3 done */
} vio_t;

void    vio_init(vio_t *v, vio_hot_t *hot);
uint8_t vio_io1_read(const vio_hot_t *h, uint8_t off);          /* ISR */
void    vio_io1_write(vio_hot_t *h, uint8_t off, uint8_t val);  /* ISR */
uint8_t vio_io2_read(const vio_hot_t *h, uint8_t off);          /* ISR */
void    vio_io2_write(vio_hot_t *h, uint8_t off, uint8_t val);  /* ISR */
int     vio_poll(vio_t *v);                  /* 1 if a command was processed */
int     vio_arm_scan(vio_t *v);              /* call once the units and pinned numbers are set: 1 if the KERNAL will be asked to scan the bus */
void    vio_publish_devices(vio_t *v);       /* copy the current device numbers to the DEV0/DEV1 registers (call after vdrive_set_devices) */

size_t  vio_sizeof(void);
size_t  vio_hot_sizeof(void);

#ifdef __cplusplus
}
#endif
#endif
