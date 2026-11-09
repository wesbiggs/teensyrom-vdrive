/* Mailbox register engine for the TR+ virtual drive.
 *
 *   IO1 ($DE10, offsets below are relative to it): +0 CMD (W: trigger)  +1 ARG (RW)  +2 DATA (R)  +3 STAT (R)  +4 ST (R)  +5 LEN (RW)
 *                                                  +6 DEV0 (R)  +7 DEV1 (R): IEC device number of the SD / USB drive now (0 = none)
 *                                                  +8 BORDER (R): request for the KERNAL's IRQ routine, see VIO_BRD_* (disk-switch feedback)
 *                                                  +9 ID0, +10 ID1 (R): VIO_ID0_VAL / VIO_ID1_VAL while the handler answers (the KERNAL's reset hook, see VIO_MENU)
 *   IO2 ($DF80): 128-byte window (RW): file name in, data blocks in/out. $DF00-$DF7F is not ours: the REU's registers live
 *                there in the "VDrive+REU" and "VDrive+Swift+REU" handlers (IOH_VDriveREU.c); the others leave it unanswered.
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
 * KERNAL that does not understand SCANREQ just times out on that one call and the defaults stay.
 * The TR menu probes the bus itself before it starts a program (vio_probe_begin / vio_probe_free, the same steps driven from the
 * menu's registers), so DEV0/DEV1 are right from the program's first instruction; after that probe nothing more is asked. */
#ifndef VDRIVE_IO_H
#define VDRIVE_IO_H
#include "vdrive.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VIO_CMD = 0, VIO_ARG = 1, VIO_DATA = 2, VIO_STAT = 3, VIO_ST = 4, VIO_LEN = 5, VIO_DEV0 = 6, VIO_DEV1 = 7, VIO_BORDER = 8, VIO_ID0 = 9, VIO_ID1 = 10, VIO_KEY = 11, VIO_NREGS = 12 };
#define VIO_BUSY    0x80
#define VIO_OK      0x5A
#define VIO_NOTMINE 0xA5
/* BORDER register values (exact: the 6502 reads it twice and ignores anything else, e.g. $DE from a late answer): */
#define VIO_BRD_IDLE    0x80   /* nothing to do */
#define VIO_BRD_SET     0xC0   /* | colour 0..15: set the border to it */
#define VIO_BRD_SAVE    0xB0   /* remember the current border colour */
#define VIO_BRD_RESTORE 0xD0   /* set the border back to the remembered colour */
/* ID registers: the KERNAL's reset entry ($FCE2, also SYS64738) sends VIO_MENU only when it reads these. A launch from the TR menu
 * resets the C64 too, while the menu's own IO1 handler still answers: its $DE19/$DE1A are a SID's init address (0 without a SID),
 * and no tune starts in I/O space, so $D6xx never shows there. Unanswered, the bus floats to $DE. */
#define VIO_ID0_VAL 0xD6
#define VIO_ID1_VAL 0x64
/* KEY register (W; reads back the last value until the main loop takes it): the plain TR's keyboard hook (vector/vdhooks.s) writes the
 * key matrix code of a key newly pressed with CTRL+C= held. VIO_KEY_SD / VIO_KEY_USB (F1, F3) switch that unit to the next image of its
 * /vdrive-config.txt, as the TR+'s Alt button does; anything else is ignored. See vio_take_key. */
#define VIO_KEY_SD  0x04
#define VIO_KEY_USB 0x05
#define VIO_WIN     128   /* window size: names, fast LOAD / SAVE blocks (a power of two, see vio_io2_read) */
#define VIO_WIN_OFS 0x80  /* the window starts at IO2 + this ($DF80) */
#define VIO_SCANREQ 0x3C   /* answer to the first LISTEN / TALK / fast LOAD / fast SAVE of a run: "scan the bus, then ask again" (see VIO_SCAN) */

/* Commands 1..8 are VD_LISTEN..VD_UNTLK (byte-level). Fast paths: */
enum {
    VIO_FLOAD_OPEN = 9,    /* ARG=device, window[0..LEN) = name (LEN < VIO_WIN). DATA: 0 ok (window[0..1] = load address), 4 not found, 5 no device */
    VIO_FLOAD_BLK  = 10,   /* DATA = byte count (0..VIO_WIN) now in window; ST=$40 on the last block */
    VIO_FSAVE_OPEN = 11,   /* ARG=device, window[0..LEN) = name. DATA: 0 ok else DOS error code (63, 72, ...) or 5 */
    VIO_FSAVE_BLK  = 12,   /* LEN bytes (at most VIO_WIN) from window are appended to the file */
    VIO_FSAVE_CLOSE= 13,   /* DATA: 0 ok else DOS error code */
    VIO_SCAN       = 14,   /* start of the reset-time bus scan: releases the automatic units' numbers. DATA = units that need a number */
    VIO_FREE       = 15,   /* ARG = a device number nobody answered at; it goes to the next unit that needs one. DATA = units still waiting */
    VIO_MENU       = 16,   /* back to the TR menu: answered OK, then the Teensy writes back what is unsaved and resets the C64 into its menu
                              (VDrive Config's Q; the KERNAL's cold start, SYS64738, once it has read VIO_ID0_VAL / VIO_ID1_VAL) */
    VIO_FLOAD_DMA  = 17    /* the next part of a fast LOAD, ARG/LEN = the address its first byte goes to (the KERNAL's $AE/$AF). Answered as
                              VIO_FLOAD_BLK (DATA = bytes in the window), or, when the host can DMA and the address allows it (vio_dma_ok), with
                              DATA = 0: up to dma_max bytes were written into C64 RAM by DMA and the end pointer into $AE/$AF (VIO_DMA_PTR).
                              ST as VIO_FLOAD_BLK. A failed DMA is answered NOTMINE (the KERNAL reports an error) */
};
#define VIO_DMA_PTR 0xAE   /* the KERNAL's LOAD pointer ($AE/$AF), written by DMA after each DMA part */
/* DMA only to RAM the CPU's own stores would reach just the same: not $0000-$01FF (the CPU port at $00/$01, zero page, the stack the
 * KERNAL is running on) and not $D000-$DFFF (I/O or the character ROM, depending on $01; the window path writes there with the CPU) */
#define vio_dma_ok(a) ((a) >= 0x0200 && ((a) < 0xD000 || (a) >= 0xE000))

/* Registers + window: the only memory the bus ISR reads or writes (~140 bytes). Keep in DTCM. */
typedef struct {
    volatile uint8_t cmd, arg, data, stat, st, len, dev0, dev1, border, id0, id1, key, pending;   /* cmd..key are in register order (see the static_assert in IOH_VDrive.c) */
    uint8_t window[VIO_WIN];
} vio_hot_t;

/* Drive state and fast-path bookkeeping: main loop only. */
typedef struct {
    vdrive_t vd;                       /* must stay first: callers pass a vio_t* where a vdrive_t* is wanted */
    vio_hot_t *h;
    uint8_t fl_active, fl_eof, fl_dev, fs_active, fs_dev;
    uint8_t scan_state;                /* 0 not needed, 1 to be requested, 2 requested, 3 done */
    uint8_t menu_req;                  /* VIO_MENU was received (see vio_take_menu) */
    /* VIO_FLOAD_DMA (vio_set_dma; vio_init clears them, so no DMA): write len bytes at C64 address addr, 1 = done */
    int     (*dma_write)(void *ctx, uint16_t addr, const uint8_t *buf, uint16_t len);
    void     *dma_ctx;
    uint8_t  *dma_buf;                 /* dma_max bytes, main loop only */
    uint16_t  dma_max;
} vio_t;

void    vio_init(vio_t *v, vio_hot_t *hot);
uint8_t vio_io1_read(const vio_hot_t *h, uint8_t off);          /* ISR */
void    vio_io1_write(vio_hot_t *h, uint8_t off, uint8_t val);  /* ISR */
uint8_t vio_io2_read(const vio_hot_t *h, uint8_t off);          /* ISR; off = window index (IO2 address - VIO_WIN_OFS), wraps */
void    vio_io2_write(vio_hot_t *h, uint8_t off, uint8_t val);  /* ISR */
int     vio_poll(vio_t *v);                  /* 1 if a command was processed */
int     vio_arm_scan(vio_t *v);              /* call once the units and pinned numbers are set: 1 if the KERNAL will be asked to scan the bus */
int     vio_probe_begin(vio_t *v);           /* the menu's probe before launch: releases the automatic numbers, no SCANREQ afterwards; returns units that need a number */
int     vio_probe_free(vio_t *v, uint8_t dev);   /* nobody answered at dev during that probe (as VIO_FREE); returns units still waiting */
int     vio_take_menu(vio_t *v);             /* 1 once after VIO_MENU: the host should return to its menu */
int     vio_take_key(vio_t *v);              /* the KEY register's last write, once (and the register cleared); 0 = none */
void    vio_set_dma(vio_t *v, int (*write)(void *ctx, uint16_t addr, const uint8_t *buf, uint16_t len), void *ctx, uint8_t *buf, uint16_t max);
void    vio_publish_devices(vio_t *v);       /* copy the current device numbers to the DEV0/DEV1 registers (call after vdrive_set_devices) */

size_t  vio_sizeof(void);
size_t  vio_hot_sizeof(void);

#ifdef __cplusplus
}
#endif
#endif
