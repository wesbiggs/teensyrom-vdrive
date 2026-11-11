#include "vdrive_vec.h"
#include "vd_cfg.h"
#include <string.h>

/* wait states */
enum { VV_W_NONE, VV_W_CALL, VV_W_DATA };

/* the vector each entry replaces ([VV_NE] = GETIN, reached through the fast path only); the same addresses on a C64 and a C128, but
 * for VV_E_MAIN, whose vector is the profile's main_vec */
static const uint16_t vv_vec[VV_NE + 1] = { 0, 0x031A, 0x031C, 0x031E, 0x0320, 0x0322, 0x0324, 0x0326, 0x032C, 0x0330, 0x0332, 0x0302, 0x032A };

/* What differs between the KERNALs (docs/DESIGN.md 56). C64: 901227-03 (the -02, SX-64 and C128 C64-mode ROMs have the same routines
 * there). C128 in 128 mode: 318020-05 with BASIC 7.0 318018-04 / 318019-04; 315078-03, 318034-01 and 325172-01 differ only in
 * $FC80-$FE8B, which none of these touch. */
typedef struct {
    uint16_t stock[VV_NE + 1];  /* the vectors as RESTOR / BASIC's init set them, used until INSTALL has read the real ones */
    uint16_t searching;         /* "SEARCHING FOR name" (direct mode only) */
    uint16_t loading;           /* "LOADING" / "VERIFYING" ($93); the C128's also copies $AE/$AF to $AC/$AD first */
    uint16_t saving;            /* "SAVING name" */
    uint16_t error1;            /* the KERNAL's error exits: error1 + 3 * (n - 1), 1 TOO MANY FILES .. 9 ILLEGAL DEVICE */
    uint16_t files;             /* the file table: LAT, FAT = LAT + 10, SAT = LAT + 20 */
    uint16_t restor;            /* RESTOR's table of the vectors $0314-$0333 */
    uint16_t main_vec;          /* the entry that hooks again after a RESTOR: IMAIN on a C64 (BASIC's READY loop); the C128's BASIC 7
                                   calls IMAIN with bank 14 in ($FF03: the character ROM where I/O, and the agent, would be), so there
                                   it is the warm-start vector $0A00, which RUN/STOP+RESTORE's NMI routine jumps through after RESTOR */
    uint16_t initv_main;        /* where BASIC's init keeps main_vec's value (0: it is stock[VV_E_MAIN]) */
    uint8_t  zp_end;            /* OPEN / LOAD / SAVE PEEK the zero page $AE..zp_end-1 */
} vv_kernal_t;

static const vv_kernal_t vv_kernal[2] = {
    { { 0, 0xF34A, 0xF291, 0xF20E, 0xF250, 0xF333, 0xF157, 0xF1CA, 0xF32F, 0xF4A5, 0xF5ED, 0xA483, 0xF13E },
      0xF5AF, 0xF5D2, 0xF68F, 0xF6FB, 0x0259, 0xFD30, 0x0302, 0xE447 + 2, 0xC5 },
    { { 0, 0xEFBD, 0xF188, 0xF106, 0xF14C, 0xF226, 0xEF06, 0xEF79, 0xF222, 0xF26C, 0xF54E, 0x4003, 0xEEEB },
      0xF50F, 0xFC4A, 0xF5BC, 0xF67C, 0x0362, 0xE073, 0x0A00, 0, 0xC8 },
};
#define KN (&vv_kernal[e->machine == VV_M_C128])

/* the bus scan's calls of the real bus: the jump table, the same on both */
#define K_LISTEN    0xFFB1
#define K_SECOND    0xFF93
#define K_UNLSN     0xFFAE
#define K_ERROR(n)  (uint16_t)(KN->error1 + 3 * ((n) - 1))

/* C128: the MMU's configuration register, and the value for each bank as the KERNAL's GETCFG table ($F7F0) has it, with I/O switched
 * in (bit 0 clear): the agent runs in I/O. Banks 0-3 are RAM, 14 and 15 the system bank. Both RAM banks hold the KERNAL's interrupt
 * trampolines at $FF05 and the CPU vectors, so an interrupt while bank 1 is switched in is served as usual. */
#define MMU_CR 0xFF00
static const uint8_t vv_bank_cfg[16] = { 0x3E, 0x7E, 0xBE, 0xFE, 0x16, 0x56, 0x96, 0xD6, 0x2A, 0x6A, 0xAA, 0xEA, 0x06, 0x0A, 0x00, 0x00 };

#define P_C 0x01
#define P_Z 0x02
#define P_N 0x80

VD_COLD void vvec_init(vvec_t *e, vio_t *v, uint16_t base)
{
    memset((void *)e, 0, sizeof *e);
    e->v = v;
    e->base = base;
    memcpy(e->old, vv_kernal[0].stock, sizeof e->old);
}

VD_COLD void vvec_set_machine(vvec_t *e, uint8_t machine)
{
    e->machine = machine == VV_M_C128 ? VV_M_C128 : VV_M_C64;
    memcpy(e->old, KN->stock, sizeof e->old);
}

VD_COLD size_t vvec_sizeof(void) { return sizeof(vvec_t); }

static uint16_t target_of(const vvec_t *e, int k)
{
    if (k == VV_E_BSOUT) return (uint16_t)(e->base + VV_OFS_F_BSOUT);
    if (k == VV_E_BASIN) return (uint16_t)(e->base + VV_OFS_F_BASIN);
    if (k == VV_NE)      return (uint16_t)(e->base + VV_OFS_F_GETIN);
    return (uint16_t)(e->base + VV_OFS_ENTRIES + 3 * k);
}

/* ---- the script in the window ---- */
static void sc(vvec_t *e, uint8_t b) { if (e->sp < VIO_WIN - 2) e->v->h->window[e->sp++] = b; }

VD_COLD static void sc_poke(vvec_t *e, uint16_t addr, const uint8_t *d, uint8_t n)
{
    sc(e, VV_OP_POKE); sc(e, (uint8_t)addr); sc(e, (uint8_t)(addr >> 8)); sc(e, n);
    for (int i = n - 1; i >= 0; i--) sc(e, d[i]);                    /* the agent stores from the top down */
}
VD_COLD static void sc_poke1(vvec_t *e, uint16_t addr, uint8_t b) { sc_poke(e, addr, &b, 1); }
VD_COLD static void sc_peek(vvec_t *e, uint16_t addr, uint8_t n) { sc(e, VV_OP_PEEK); sc(e, (uint8_t)addr); sc(e, (uint8_t)(addr >> 8)); sc(e, n); }
VD_COLD static void sc_go(vvec_t *e, uint16_t to, uint8_t y, uint8_t p, uint8_t a, uint8_t x)
{
    sc(e, VV_OP_GO); sc(e, y); sc(e, p); sc(e, a); sc(e, x);
    e->v->h->window[VIO_WIN - 2] = (uint8_t)to; e->v->h->window[VIO_WIN - 1] = (uint8_t)(to >> 8);
}
VD_COLD static void sc_ret(vvec_t *e, const vv_ctx_t *c, uint8_t a, uint8_t x, uint8_t y, int carry)
{
    uint8_t p = (uint8_t)((c->p & ~(P_C | P_Z | P_N)) | (carry ? P_C : 0) | (a ? 0 : P_Z) | (a & P_N));
    sc_go(e, (uint16_t)(e->base + VV_OFS_RTS), y, p, a, x);
}
VD_COLD static void sc_chain(vvec_t *e, const vv_ctx_t *c, int k) { sc_go(e, e->old[k], c->y, c->p, c->a, c->x); }
VD_COLD static void sc_error(vvec_t *e, const vv_ctx_t *c, int n) { sc_go(e, K_ERROR(n), c->y, c->p, c->a, c->x); }
VD_COLD static void sc_call(vvec_t *e, const vv_ctx_t *c, uint16_t routine, uint8_t a, uint8_t x, uint8_t y)
{
    uint8_t r[2] = { (uint8_t)routine, (uint8_t)(routine >> 8) };
    sc_poke(e, (uint16_t)(e->base + VV_OFS_CTRAMP + 1), r, 2);
    sc_go(e, (uint16_t)(e->base + VV_OFS_CTRAMP), y, (uint8_t)(c->p & ~P_C), a, x);
}

/* one byte-level drive transaction; returns 1 when the drive says it is not ours. ST bits are ORed into *st */
VD_COLD static int bus(vvec_t *e, uint8_t cmd, uint8_t arg, uint8_t *d, uint8_t *st)
{
    uint8_t dd = 0, s = 0;
    int nm = vdrive_mailbox(&e->v->vd, cmd, arg, &dd, &s);
    if (d) *d = dd;
    if (st) *st |= s;
    return nm;
}

static int ours(vvec_t *e, uint8_t dev) { return dev >= 4 && vdrive_unit_for(&e->v->vd, dev) >= 0; }

/* ---- protothread ---- */
#define PT_BEGIN(c)   switch ((c)->pc) { case 0:
#define PT_END(c)     } (c)->pc = 0; return 0
#define YIELD_(c, w, n) do { (c)->wait = (w); (c)->pc = (uint16_t)((n) + 1); return 1; case (n) + 1:; } while (0)
#define YIELD(c, w)   YIELD_(c, w, __COUNTER__)
#define DONE(c)       do { (c)->pc = 0; return 0; } while (0)
#define CALL(r, A, X, Y) do { sc_call(e, c, (r), (A), (X), (Y)); YIELD(c, VV_W_CALL); } while (0)
#define PEEK(ad, n)   do { sc_peek(e, (ad), (n)); YIELD(c, VV_W_DATA); } while (0)
#define CONT()        do { sc(e, VV_OP_CONT); YIELD(c, VV_W_DATA); } while (0)
#define CHAIN()       do { sc_chain(e, c, c->entry); DONE(c); } while (0)
/* C128: memory in another bank. The caller's configuration is read once per call (the KERNAL is visible, so it is the system bank
 * with I/O, but which RAM bank and ROMs the caller had in is its own business); a PEEK leaves the bank switched in until the next
 * script, whose first op switches back. A C64, or the bank the caller has in already, needs no switch. */
#define MMU_READ()    do { if (e->machine == VV_M_C128 && c->mmu == 0xFF) { PEEK(MMU_CR, 1); c->mmu = c->buf[0]; } } while (0)
#define BANKED(b)     (e->machine == VV_M_C128 && vv_bank_cfg[(b) & 15] != c->mmu)
#define BPEEK(b, ad, n) do { if (BANKED(b)) { sc_poke1(e, MMU_CR, vv_bank_cfg[(b) & 15]); PEEK((ad), (n)); sc_poke1(e, MMU_CR, c->mmu); } \
                             else PEEK((ad), (n)); } while (0)
#define ERROR(n)      do { sc_error(e, c, (n)); DONE(c); } while (0)

/* the zero page as PEEKed from $AE ($AE-$C4 on a C64; to $C7 on a C128: BA, FNBANK) */
#define ZP(a) c->buf[(a) - 0xAE]

/* hook every vector that is not ours yet (c->buf = $0302-$0333 as PEEKed; c->mainv = main_vec's value where it is elsewhere), and
 * give the agent its fast paths' old vectors */
VD_COLD static void install(vvec_t *e, vv_ctx_t *c)
{
    uint8_t img[0x34 - 0x02];
    memcpy(img, c->buf, sizeof img);
    if (KN->main_vec != 0x0302) {
        uint16_t mine = target_of(e, VV_E_MAIN);
        if (c->mainv != mine) {
            uint8_t z[2] = { (uint8_t)mine, (uint8_t)(mine >> 8) };
            e->old[VV_E_MAIN] = c->mainv;
            sc_poke(e, KN->main_vec, z, 2);
        }
    }
    for (int k = 1; k <= VV_NE; k++) {
        if (k == VV_E_INSTALL || (k == VV_E_MAIN && KN->main_vec != 0x0302)) continue;
        int o = vv_vec[k] - 0x0302;
        uint16_t cur = (uint16_t)(img[o] | img[o + 1] << 8), mine = target_of(e, k);
        if (cur == mine) continue;
        e->old[k] = cur;
        img[o] = (uint8_t)mine; img[o + 1] = (uint8_t)(mine >> 8);
    }
    sc_poke(e, 0x031A, img + 0x18, 0x34 - 0x1A);
    sc_poke(e, 0x0302, img, 2);
    uint8_t o3[6] = { (uint8_t)e->old[VV_E_BSOUT], (uint8_t)(e->old[VV_E_BSOUT] >> 8), (uint8_t)e->old[VV_E_BASIN],
                      (uint8_t)(e->old[VV_E_BASIN] >> 8), (uint8_t)e->old[VV_NE], (uint8_t)(e->old[VV_NE] >> 8) };
    sc_poke(e, (uint16_t)(e->base + VV_OFS_OLD), o3, 6);
}

VD_COLD static int vv_run(vvec_t *e, vv_ctx_t *c)
{
    vio_t *v = e->v;
    PT_BEGIN(c);

    /* OPEN, LOAD, SAVE: the file's numbers and name; the bus scan when this run has not had one */
    if (c->entry == VV_E_OPEN || c->entry == VV_E_LOAD || c->entry == VV_E_SAVE) {
        PEEK(0xAE, KN->zp_end - 0xAE);
        if (e->machine == VV_M_C128) { c->ba = ZP(0xC6); c->fnbank = ZP(0xC7); }
        c->fnlen = ZP(0xB7); c->la = ZP(0xB8); c->sa = ZP(0xB9); c->fa = ZP(0xBA);
        c->fnadr = (uint16_t)(ZP(0xBB) | ZP(0xBC) << 8);
        c->addr = (uint16_t)(ZP(0xC1) | ZP(0xC2) << 8);           /* SAVE: start */
        c->end = (uint16_t)(ZP(0xAE) | ZP(0xAF) << 8);            /* SAVE: end */
        c->cadr = (uint16_t)(ZP(0xC3) | ZP(0xC4) << 8);           /* LOAD: the caller's address */
        if (c->fa >= 4 && c->fnlen && v->scan_state == 1) {
            /* which numbers are free on the real bus: LISTEN + SECOND as OPEN does it (DESIGN.md 16), through the KERNAL */
            v->scan_state = 3;
            c->n = (uint8_t)vdrive_scan_begin(&v->vd);
            for (c->dev = 8; c->n && c->dev < 31; c->dev++) {
                sc_poke1(e, 0x90, 0);
                CALL(K_LISTEN, c->dev, 0, 0);
                if (!(c->rst & 0x80)) {
                    CALL(K_SECOND, 0x6F, 0, 0);
                    if (!(c->rst & 0x80)) { CALL(K_UNLSN, 0, 0, 0); continue; }
                }
                c->n = (uint8_t)vdrive_scan_free(&v->vd, c->dev);
            }
            vio_publish_devices(v);
            sc_poke1(e, 0x90, c->st);
        }
    }

    /* handlers (an if chain, not a switch: YIELD's case labels must belong to the protothread's switch) */
    if (c->entry == VV_E_INSTALL) {
        /* which KERNAL: the jump table's LOAD is JMP $F265 on a C128 in 128 mode, JMP $F49E on a C64 (and a C128 in C64 mode). So the
         * same served agent works after a launch into either mode, an Exit to C128 BASIC, GO64 or a reset into the other mode. */
        PEEK(0xFFD5, 3);
        {
            uint8_t m = (c->buf[0] == 0x4C && c->buf[1] == 0x65 && c->buf[2] == 0xF2) ? VV_M_C128 : VV_M_C64;
            if (m != e->machine) vvec_set_machine(e, m);
        }
        if (KN->main_vec != 0x0302) { PEEK(KN->main_vec, 2); c->mainv = (uint16_t)(c->buf[0] | c->buf[1] << 8); }
        PEEK(0x0302, 0x34 - 0x02);
        if (c->a == VV_LAUNCH) {
            /* from the menu's PRG loader, after the program is in memory: hook only if the vectors are still the ones RESTOR / INITV
             * left (the ROM's own tables). A program that loaded over them (an autostart PRG) keeps its own. */
            memcpy(c->data, c->buf, 0x32);
            PEEK((uint16_t)(KN->restor + (0x031A - 0x0314)), 0x34 - 0x1A);
            c->n = (uint8_t)(memcmp(c->buf, c->data + 0x18, 0x34 - 0x1A) == 0);
            if (KN->initv_main) {
                PEEK(KN->initv_main, 2);                      /* BASIC's init table: $0302's value */
                c->n = (uint8_t)(c->n && !memcmp(c->buf, c->data, 2));
            } else c->n = (uint8_t)(c->n && c->mainv == KN->stock[VV_E_MAIN]);
            if (!c->n) { sc_ret(e, c, c->a, c->x, c->y, 1); DONE(c); }
            memcpy(c->buf, c->data, 0x32);
        }
        install(e, c);
        sc_ret(e, c, c->a, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_MAIN) {      /* BASIC's READY loop (C128: its warm start): hook again after a RESTOR (RUN/STOP+RESTORE) */
        PEEK(0x0330, 2);
        if ((uint16_t)(c->buf[0] | c->buf[1] << 8) == e->old[VV_E_LOAD] && e->old[VV_E_LOAD] != target_of(e, VV_E_LOAD)) {
            c->mainv = target_of(e, VV_E_MAIN);                      /* we came through it */
            PEEK(0x0302, 0x34 - 0x02);
            install(e, c);
        }
        CHAIN();
    }

    if (c->entry == VV_E_OPEN) {
        if (c->fa < 4 || (c->sa & 0x80) || !c->fnlen || !ours(e, c->fa)) CHAIN();
        MMU_READ();
        /* the stock OPEN fills the file table; with the name length 0 it sends nothing */
        sc_poke1(e, 0xB7, 0);
        CALL(e->old[VV_E_OPEN], c->a, c->x, c->y);
        sc_poke1(e, 0xB7, c->fnlen);
        if (c->rp & P_C) { sc_ret(e, c, c->ra, c->rx, c->ry, 1); DONE(c); }   /* FILE OPEN, TOO MANY FILES, ... */
        c->stx = 0;
        bus(e, VD_LISTEN, c->fa, 0, &c->stx);
        bus(e, VD_SECOND, (uint8_t)(c->sa | 0xF0), 0, &c->stx);           /* the table's SA is SA | $60 */
        if (c->stx & 0x80) { bus(e, VD_UNLSN, 0, 0, 0); sc_poke1(e, 0x90, c->stx); ERROR(5); }
        for (c->i = 0; c->i < c->fnlen; c->i = (uint16_t)(c->i + c->n)) {
            c->n = (uint8_t)(c->fnlen - c->i > VV_CHUNK ? VV_CHUNK : c->fnlen - c->i);
            BPEEK(c->fnbank, (uint16_t)(c->fnadr + c->i), c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, &c->stx);
        }
        bus(e, VD_UNLSN, 0, 0, &c->stx);
        sc_poke1(e, 0x90, c->stx);
        sc_ret(e, c, c->a, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_CLOSE || c->entry == VV_E_CHKIN || c->entry == VV_E_CKOUT) {   /* CLOSE: A = logical file; CHKIN/CKOUT: X */
        if (c->entry != VV_E_CLOSE) sc_poke1(e, 0x90, 0), c->st = 0;   /* as the stock lookup ($F30F) */
        PEEK(KN->files, 30);                            /* LAT, FAT, SAT */
        c->la = c->entry == VV_E_CLOSE ? c->a : c->x;
        for (c->idx = c->nfiles; c->idx > 0 && c->buf[c->idx - 1] != c->la; c->idx--) {}
        if (!c->idx || c->nfiles > 10) CHAIN();         /* not open: the stock routine reports it */
        c->idx--;
        c->fa = c->buf[10 + c->idx]; c->sa = c->buf[20 + c->idx];
        if (!ours(e, c->fa)) CHAIN();
        c->stx = 0;
        if (c->entry == VV_E_CLOSE) {
            if (c->sa & 0x80) CHAIN();
            /* C128: CLOSE with C set leaves a drive's command channel open on the drive (stock $F1CF), as DCLOSE's U-commands need */
            if (e->machine == VV_M_C128 && (c->p & P_C) && c->fa >= 8 && (c->sa & 0x0F) == 0x0F) CHAIN();
            bus(e, VD_LISTEN, c->fa, 0, 0);
            bus(e, VD_SECOND, (uint8_t)((c->sa & 0xEF) | 0xE0), 0, 0);
            bus(e, VD_UNLSN, 0, 0, 0);
            sc_poke1(e, (uint16_t)(KN->files + 20 + c->idx), (uint8_t)(c->sa | 0x80));   /* now the stock CLOSE only drops the entry */
            CHAIN();
        }
        if (c->entry == VV_E_CHKIN) {
            bus(e, VD_TALK, c->fa, 0, &c->stx);
            if (!(c->sa & 0x80)) bus(e, VD_TKSA, c->sa, 0, &c->stx);
        } else {
            bus(e, VD_LISTEN, c->fa, 0, &c->stx);
            if (!(c->sa & 0x80)) bus(e, VD_SECOND, c->sa, 0, &c->stx);
        }
        {
            uint8_t z[3] = { c->la, c->sa, c->fa };
            sc_poke(e, 0xB8, z, 3);
        }
        sc_poke1(e, 0x90, c->stx);
        if (c->stx & 0x80) ERROR(5);
        sc_poke1(e, c->entry == VV_E_CHKIN ? 0x99 : 0x9A, c->fa);
        sc_ret(e, c, c->fa, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_CLRCH || c->entry == VV_E_CLALL) {
        if (c->dflto >= 4 && !bus(e, VD_UNLSN, 0, 0, 0)) sc_poke1(e, 0x9A, 3);
        if (c->dfltn >= 4 && !bus(e, VD_UNTLK, 0, 0, 0)) sc_poke1(e, 0x99, 0);
        CHAIN();                                        /* the stock routine finds nothing of ours left to end */
    }

    if (c->entry == VV_E_BASIN) {                       /* also GETIN for devices 4 and up */
        uint8_t d = 0, s = 0;
        if (c->dfltn < 4 || c->st || bus(e, VD_ACPTR, 0, &d, &s)) CHAIN();   /* ST set: the stock code returns CR */
        if (s) sc_poke1(e, 0x90, (uint8_t)(c->st | s));
        sc_ret(e, c, d, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_BSOUT) {
        uint8_t s = 0;
        if (c->dflto < 4 || bus(e, VD_CIOUT, c->a, 0, &s)) CHAIN();
        if (s) sc_poke1(e, 0x90, (uint8_t)(c->st | s));
        sc_ret(e, c, c->a, c->x, c->y, 0);              /* A = the byte sent: BASIC's PRINT depends on it */
        DONE(c);
    }

    if (c->entry == VV_E_LOAD) {                        /* A = 0 load, 1 verify */
        if (c->fa < 4 || !c->fnlen || !ours(e, c->fa)) CHAIN();
        MMU_READ();
        c->verify = c->a;
        sc_poke1(e, 0x93, c->verify);
        sc_poke1(e, 0x90, 0);
        CALL(KN->searching, 0, 0, 0);
        sc_poke1(e, 0xB9, 0x60);
        c->stx = 0;
        bus(e, VD_LISTEN, c->fa, 0, &c->stx);
        bus(e, VD_SECOND, 0xF0, 0, &c->stx);
        if (c->stx & 0x80) { bus(e, VD_UNLSN, 0, 0, 0); sc_poke1(e, 0x90, 0x80); ERROR(5); }
        for (c->i = 0; c->i < c->fnlen; c->i = (uint16_t)(c->i + c->n)) {
            c->n = (uint8_t)(c->fnlen - c->i > VV_CHUNK ? VV_CHUNK : c->fnlen - c->i);
            BPEEK(c->fnbank, (uint16_t)(c->fnadr + c->i), c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, 0);
        }
        bus(e, VD_UNLSN, 0, 0, 0);
        c->stx = 0;
        bus(e, VD_TALK, c->fa, 0, &c->stx);
        bus(e, VD_TKSA, 0x60, 0, &c->stx);
        {
            uint8_t s0 = 0, s1 = 0;
            bus(e, VD_ACPTR, 0, &c->b0, &s0);
            if (s0 & 0x02) {                            /* nothing came: FILE NOT FOUND */
                bus(e, VD_UNTLK, 0, 0, 0);
                bus(e, VD_LISTEN, c->fa, 0, 0); bus(e, VD_SECOND, 0xE0, 0, 0); bus(e, VD_UNLSN, 0, 0, 0);
                ERROR(4);
            }
            c->eoi = (s0 & 0x40) != 0;
            c->b1 = 0;
            if (!c->eoi) { bus(e, VD_ACPTR, 0, &c->b1, &s1); c->eoi = (s1 & 0x40) != 0; }
        }
        c->addr = c->sa ? (uint16_t)(c->b0 | c->b1 << 8) : c->cadr;   /* SA 0: the caller's address */
        {
            uint8_t z[2] = { (uint8_t)c->addr, (uint8_t)(c->addr >> 8) };   /* as stock: the C128's message copies it to $AC/$AD */
            sc_poke(e, 0xAE, z, 2);
        }
        c->chunk = BANKED(c->ba) ? VV_CHUNK_B : VV_CHUNK;
        CALL(KN->loading, 0, 0, 0);
        c->stx = 0;
        while (!c->eoi) {
            c->n = 0;
            while (c->n < c->chunk && !c->eoi) {
                uint8_t s = 0, d = 0;
                bus(e, VD_ACPTR, 0, &d, &s);
                if (s & 0x02) { c->eoi = 1; c->stx |= 0x02; break; }   /* a read error (bad sector): the load ends here, ST $42 */
                c->data[c->n++] = d;
                if (s & 0x40) c->eoi = 1;
            }
            if (!c->n) break;
            if (c->verify) {
                BPEEK(c->ba, c->addr, c->n);
                if (memcmp(c->buf, c->data, c->n)) c->stx |= 0x10;     /* VERIFY ERROR; the load goes on, as stock */
            } else if (BANKED(c->ba)) {
                sc_poke1(e, MMU_CR, vv_bank_cfg[c->ba & 15]);
                sc_poke(e, c->addr, c->data, c->n);
                sc_poke1(e, MMU_CR, c->mmu);
                CONT();
            } else {
                sc_poke(e, c->addr, c->data, c->n);
                CONT();
            }
            c->addr = (uint16_t)(c->addr + c->n);
        }
        bus(e, VD_UNTLK, 0, 0, 0);
        bus(e, VD_LISTEN, c->fa, 0, 0); bus(e, VD_SECOND, 0xE0, 0, 0); bus(e, VD_UNLSN, 0, 0, 0);
        {
            uint8_t z[2] = { (uint8_t)c->addr, (uint8_t)(c->addr >> 8) };
            sc_poke(e, 0xAE, z, 2);
        }
        sc_poke1(e, 0x90, (uint8_t)(0x40 | c->stx));
        sc_ret(e, c, c->a, (uint8_t)c->addr, (uint8_t)(c->addr >> 8), 0);
        DONE(c);
    }

    if (c->entry == VV_E_SAVE) {                        /* $C1/$C2 start, $AE/$AF end */
        if (c->fa < 4 || !c->fnlen || !ours(e, c->fa)) CHAIN();
        MMU_READ();
        sc_poke1(e, 0xB9, 0x61);
        c->stx = 0;
        bus(e, VD_LISTEN, c->fa, 0, &c->stx);
        bus(e, VD_SECOND, 0xF1, 0, &c->stx);
        if (c->stx & 0x80) { bus(e, VD_UNLSN, 0, 0, 0); sc_poke1(e, 0x90, 0x80); ERROR(5); }
        for (c->i = 0; c->i < c->fnlen; c->i = (uint16_t)(c->i + c->n)) {
            c->n = (uint8_t)(c->fnlen - c->i > VV_CHUNK ? VV_CHUNK : c->fnlen - c->i);
            BPEEK(c->fnbank, (uint16_t)(c->fnadr + c->i), c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, 0);
        }
        bus(e, VD_UNLSN, 0, 0, 0);
        CALL(KN->saving, 0, 0, 0);
        bus(e, VD_LISTEN, c->fa, 0, 0);
        bus(e, VD_SECOND, 0x61, 0, 0);
        bus(e, VD_CIOUT, (uint8_t)c->addr, 0, 0);
        bus(e, VD_CIOUT, (uint8_t)(c->addr >> 8), 0, 0);
        while (c->addr < c->end) {
            c->n = (uint8_t)(c->end - c->addr > VV_CHUNK ? VV_CHUNK : c->end - c->addr);
            BPEEK(c->ba, c->addr, c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, 0);
            c->addr = (uint16_t)(c->addr + c->n);
        }
        bus(e, VD_UNLSN, 0, 0, 0);
        bus(e, VD_LISTEN, c->fa, 0, 0); bus(e, VD_SECOND, 0xE1, 0, 0); bus(e, VD_UNLSN, 0, 0, 0);
        sc_ret(e, c, c->a, c->x, c->y, 0);
        DONE(c);
    }

    sc_ret(e, c, c->a, c->x, c->y, 0);
    PT_END(c);
}

VD_COLD int vvec_poll(vvec_t *e)
{
    vio_hot_t *h = e->v->h;
    if (!h->pending) return 0;
    uint8_t cmd = h->cmd;
    if (cmd != VV_C_VEC && cmd != VV_C_CONT) return vio_poll(e->v);
    const uint8_t *w = h->window;
    vv_ctx_t *c = 0;
    e->sp = 0;
    /* contexts that wait for anything but a ROM routine's result are left over (RUN/STOP+RESTORE in the middle of a handler) */
    if (cmd == VV_C_VEC)
        while (e->depth && e->c[e->depth - 1].wait != VV_W_CALL) e->depth--;
    if (cmd == VV_C_VEC && w[4] == (uint8_t)(e->base + VV_OFS_CTRAMP + 5)) {   /* a ROM routine's result */
        if (e->depth) {
            c = &e->c[e->depth - 1];
            c->ry = w[0]; c->rx = w[1]; c->ra = w[2]; c->rp = w[3]; c->rst = w[5];
        }
    } else if (cmd == VV_C_VEC) {
        uint8_t k = (uint8_t)(w[4] - (uint8_t)(e->base + VV_OFS_ENTRIES + 2));
        if (k % 3 == 0 && k / 3 < VV_NE) {
            if (e->depth == VV_DEPTH) e->depth = 0;
            c = &e->c[e->depth++];
            memset((void *)c, 0, sizeof *c);
            c->entry = (uint8_t)(k / 3);
            c->mmu = 0xFF;
            c->y = w[0]; c->x = w[1]; c->a = w[2]; c->p = w[3];
            c->st = w[5]; c->nfiles = w[6]; c->dfltn = w[7]; c->dflto = w[8];
        }
    } else if (e->depth && e->c[e->depth - 1].wait == VV_W_DATA) {             /* PEEK data, or after CONT */
        c = &e->c[e->depth - 1];
        memcpy(c->buf, w, VIO_WIN);
    }
    if (!c) {
        /* nothing to answer this (a frame from nowhere): return to the caller with C set */
        vv_ctx_t z;
        memset(&z, 0, sizeof z);
        sc_ret(e, &z, 5, 0, 0, 1);
    } else if (!vv_run(e, c)) {
        e->depth--;                                     /* finished: it was the top */
    }
    h->pending = 0;
    h->stat = VIO_OK;
    return 1;
}
