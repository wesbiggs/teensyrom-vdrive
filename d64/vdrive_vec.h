/* Vector engine for the plain TeensyROM (docs/DESIGN.md 53): the Teensy side of the 6502 agent in vector/vdagent.s.
 *
 * The KERNAL's RAM vectors point into the agent, which the Teensy serves from an IO page. Each call arrives here as a frame
 * (the registers and a few zero-page bytes, in the mailbox window, command VV_C_VEC); the answer is a short script for the agent:
 * POKE / PEEK C64 memory, CONT (more to come) and GO (load the registers and jump: return, chain to the vector we replaced,
 * a KERNAL error exit, or a call of a ROM routine whose result comes back as a frame). OPEN..SAVE, the file table and the
 * serial-bus scan are done here, on top of the byte-level drive (vdrive_mailbox); "not ours" chains to the old vector.
 *
 * Handlers are protothreads: one that needs memory or a ROM call yields and goes on when the agent's next frame arrives.
 * A call that comes in while another waits for a ROM routine (that routine printed through CHROUT, or failed into CLRCHN) runs
 * on a context of its own above it. */
#ifndef VDRIVE_VEC_H
#define VDRIVE_VEC_H
#include "vdrive_io.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VV_C_VEC = 20, VV_C_CONT = 21 };                        /* agent -> Teensy commands (after VIO_FVERIFY_OPEN = 19) */
enum { VV_OP_GO = 0, VV_OP_POKE = 1, VV_OP_PEEK = 2, VV_OP_CONT = 4 };

/* the agent's entries, in its order */
enum { VV_E_INSTALL, VV_E_OPEN, VV_E_CLOSE, VV_E_CHKIN, VV_E_CKOUT, VV_E_CLRCH, VV_E_BASIN, VV_E_BSOUT, VV_E_CLALL,
       VV_E_LOAD, VV_E_SAVE, VV_E_MAIN, VV_NE };

/* the agent's layout (asserted in vector/vdagent.s) */
#define VV_OFS_CTRAMP   0       /* JSR $xxxx (the routine of a CALL), then JSR enter: the result */
#define VV_OFS_OLD      6       /* old_bsout, old_basin, old_getin: the fast paths' chain targets */
#define VV_OFS_ENTRIES  12      /* VV_NE x JSR enter */
#define VV_OFS_F_BSOUT  48
#define VV_OFS_F_BASIN  59
#define VV_OFS_F_GETIN  70
#define VV_OFS_RTS      241
#define VV_NZ           4       /* zero-page bytes in the frame: $90 $98 $99 $9A */

#define VV_LAUNCH 0xA5          /* A at INSTALL from the menu's PRG loader: hook only vectors the program left as the ROM set them */
#define VV_DEPTH 4              /* nested calls (a ROM routine calling back into a vector) */
#define VV_CHUNK 118            /* bytes per POKE / PEEK of file data */

typedef struct {
    uint16_t pc;                /* protothread resume point (0 = start) */
    uint8_t  entry, wait;       /* wait: VV_W_* */
    uint8_t  y, x, a, p;        /* the registers at the call */
    uint8_t  st, nfiles, dfltn, dflto;
    uint8_t  ry, rx, ra, rp, rst;   /* the last CALL's result and ST after it */
    uint8_t  buf[VIO_WIN];      /* the last PEEK */
    /* handler state */
    uint8_t  fnlen, la, sa, fa, verify, idx, n, dev, eoi, stx, b0, b1, left;
    uint16_t fnadr, addr, end, cadr, i;
    uint8_t  data[VIO_WIN];
} vv_ctx_t;

typedef struct {
    vio_t   *v;
    uint16_t base;              /* the agent's address */
    uint16_t old[VV_NE + 1];    /* the vectors we replaced, per entry; [VV_NE] = GETIN (fast path only) */
    uint8_t  depth, sp;
    vv_ctx_t c[VV_DEPTH];
} vvec_t;

void   vvec_init(vvec_t *e, vio_t *v, uint16_t base);
int    vvec_poll(vvec_t *e);    /* the agent's commands; anything else goes to vio_poll. 1 if a command was processed */
size_t vvec_sizeof(void);

#ifdef __cplusplus
}
#endif
#endif
