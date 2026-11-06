#include "vdrive_vec.h"
#include "vd_cfg.h"
#include <string.h>

/* wait states */
enum { VV_W_NONE, VV_W_CALL, VV_W_DATA };

/* the vector each entry replaces ([VV_NE] = GETIN, reached through the fast path only) and the stock 901227-03 values, used until
 * INSTALL has read the real ones */
static const uint16_t vv_vec[VV_NE + 1] = { 0, 0x031A, 0x031C, 0x031E, 0x0320, 0x0322, 0x0324, 0x0326, 0x032C, 0x0330, 0x0332, 0x0302, 0x032A };
static const uint16_t vv_stock[VV_NE + 1] = { 0, 0xF34A, 0xF291, 0xF20E, 0xF250, 0xF333, 0xF157, 0xF1CA, 0xF32F, 0xF4A5, 0xF5ED, 0xA483, 0xF13E };

/* KERNAL routines called through the agent */
#define K_SEARCHING 0xF5AF   /* "SEARCHING FOR name" (direct mode only) */
#define K_LOADING   0xF5D2   /* "LOADING" / "VERIFYING" ($93) */
#define K_SAVING    0xF68F   /* "SAVING name" */
#define K_LISTEN    0xFFB1   /* the real bus (the bus scan) */
#define K_SECOND    0xFF93
#define K_UNLSN     0xFFAE
#define K_ERROR(n)  (uint16_t)(0xF6FB + 3 * ((n) - 1))   /* the KERNAL's error exits: 1 TOO MANY FILES .. 9 ILLEGAL DEVICE */

#define P_C 0x01
#define P_Z 0x02
#define P_N 0x80

VD_COLD void vvec_init(vvec_t *e, vio_t *v, uint16_t base)
{
    memset((void *)e, 0, sizeof *e);
    e->v = v;
    e->base = base;
    memcpy(e->old, vv_stock, sizeof e->old);
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
#define ERROR(n)      do { sc_error(e, c, (n)); DONE(c); } while (0)

/* the zero page as PEEKed from $AE (23 bytes, $AE-$C4) */
#define ZP(a) c->buf[(a) - 0xAE]

/* hook every vector that is not ours yet (c->buf = $0302-$0333 as PEEKed), and give the agent its fast paths' old vectors */
VD_COLD static void install(vvec_t *e, vv_ctx_t *c)
{
    uint8_t img[0x34 - 0x02];
    memcpy(img, c->buf, sizeof img);
    for (int k = 1; k <= VV_NE; k++) {
        if (k == VV_E_INSTALL) continue;
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
        PEEK(0xAE, 0xC5 - 0xAE);
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
        PEEK(0x0302, 0x34 - 0x02);
        if (c->a == VV_LAUNCH) {
            /* from the menu's PRG loader, after the program is in memory: hook only if the vectors are still the ones RESTOR / INITV
             * left (the ROM's own tables). A program that loaded over them (an autostart PRG) keeps its own. */
            memcpy(c->data, c->buf, 0x32);
            PEEK(0xFD30 + (0x031A - 0x0314), 0x34 - 0x1A);
            c->n = (uint8_t)(memcmp(c->buf, c->data + 0x18, 0x34 - 0x1A) == 0);
            PEEK(0xE447 + 2, 2);                               /* BASIC's INITV table: $0302's value */
            if (!c->n || memcmp(c->buf, c->data, 2)) { sc_ret(e, c, c->a, c->x, c->y, 1); DONE(c); }
            memcpy(c->buf, c->data, 0x32);
        }
        install(e, c);
        sc_ret(e, c, c->a, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_MAIN) {                                     /* BASIC's READY loop: hook again after a RESTOR (RUN/STOP+RESTORE) */
        PEEK(0x0330, 2);
        if ((uint16_t)(c->buf[0] | c->buf[1] << 8) == e->old[VV_E_LOAD] && e->old[VV_E_LOAD] != target_of(e, VV_E_LOAD)) {
            PEEK(0x0302, 0x34 - 0x02);
            install(e, c);
        }
        CHAIN();
    }

    if (c->entry == VV_E_OPEN) {
        if (c->fa < 4 || (c->sa & 0x80) || !c->fnlen || !ours(e, c->fa)) CHAIN();
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
            PEEK((uint16_t)(c->fnadr + c->i), c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, &c->stx);
        }
        bus(e, VD_UNLSN, 0, 0, &c->stx);
        sc_poke1(e, 0x90, c->stx);
        sc_ret(e, c, c->a, c->x, c->y, 0);
        DONE(c);
    }

    if (c->entry == VV_E_CLOSE || c->entry == VV_E_CHKIN || c->entry == VV_E_CKOUT) {   /* CLOSE: A = logical file; CHKIN/CKOUT: X */
        if (c->entry != VV_E_CLOSE) sc_poke1(e, 0x90, 0), c->st = 0;   /* as the stock lookup ($F30F) */
        PEEK(0x0259, 30);                               /* LAT, FAT, SAT */
        c->la = c->entry == VV_E_CLOSE ? c->a : c->x;
        for (c->idx = c->nfiles; c->idx > 0 && c->buf[c->idx - 1] != c->la; c->idx--) {}
        if (!c->idx || c->nfiles > 10) CHAIN();         /* not open: the stock routine reports it */
        c->idx--;
        c->fa = c->buf[10 + c->idx]; c->sa = c->buf[20 + c->idx];
        if (!ours(e, c->fa)) CHAIN();
        c->stx = 0;
        if (c->entry == VV_E_CLOSE) {
            if (c->sa & 0x80) CHAIN();
            bus(e, VD_LISTEN, c->fa, 0, 0);
            bus(e, VD_SECOND, (uint8_t)((c->sa & 0xEF) | 0xE0), 0, 0);
            bus(e, VD_UNLSN, 0, 0, 0);
            sc_poke1(e, (uint16_t)(0x026D + c->idx), (uint8_t)(c->sa | 0x80));   /* now the stock CLOSE only drops the entry */
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
        c->verify = c->a;
        sc_poke1(e, 0x93, c->verify);
        sc_poke1(e, 0x90, 0);
        CALL(K_SEARCHING, 0, 0, 0);
        sc_poke1(e, 0xB9, 0x60);
        c->stx = 0;
        bus(e, VD_LISTEN, c->fa, 0, &c->stx);
        bus(e, VD_SECOND, 0xF0, 0, &c->stx);
        if (c->stx & 0x80) { bus(e, VD_UNLSN, 0, 0, 0); sc_poke1(e, 0x90, 0x80); ERROR(5); }
        for (c->i = 0; c->i < c->fnlen; c->i = (uint16_t)(c->i + c->n)) {
            c->n = (uint8_t)(c->fnlen - c->i > VV_CHUNK ? VV_CHUNK : c->fnlen - c->i);
            PEEK((uint16_t)(c->fnadr + c->i), c->n);
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
        CALL(K_LOADING, 0, 0, 0);
        c->stx = 0;
        while (!c->eoi) {
            c->n = 0;
            while (c->n < VV_CHUNK && !c->eoi) {
                uint8_t s = 0, d = 0;
                bus(e, VD_ACPTR, 0, &d, &s);
                if (s & 0x02) { c->eoi = 1; c->stx |= 0x02; break; }   /* a read error (bad sector): the load ends here, ST $42 */
                c->data[c->n++] = d;
                if (s & 0x40) c->eoi = 1;
            }
            if (!c->n) break;
            if (c->verify) {
                PEEK(c->addr, c->n);
                if (memcmp(c->buf, c->data, c->n)) c->stx |= 0x10;     /* VERIFY ERROR; the load goes on, as stock */
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
        sc_poke1(e, 0xB9, 0x61);
        c->stx = 0;
        bus(e, VD_LISTEN, c->fa, 0, &c->stx);
        bus(e, VD_SECOND, 0xF1, 0, &c->stx);
        if (c->stx & 0x80) { bus(e, VD_UNLSN, 0, 0, 0); sc_poke1(e, 0x90, 0x80); ERROR(5); }
        for (c->i = 0; c->i < c->fnlen; c->i = (uint16_t)(c->i + c->n)) {
            c->n = (uint8_t)(c->fnlen - c->i > VV_CHUNK ? VV_CHUNK : c->fnlen - c->i);
            PEEK((uint16_t)(c->fnadr + c->i), c->n);
            for (int j = 0; j < c->n; j++) bus(e, VD_CIOUT, c->buf[j], 0, 0);
        }
        bus(e, VD_UNLSN, 0, 0, 0);
        CALL(K_SAVING, 0, 0, 0);
        bus(e, VD_LISTEN, c->fa, 0, 0);
        bus(e, VD_SECOND, 0x61, 0, 0);
        bus(e, VD_CIOUT, (uint8_t)c->addr, 0, 0);
        bus(e, VD_CIOUT, (uint8_t)(c->addr >> 8), 0, 0);
        while (c->addr < c->end) {
            c->n = (uint8_t)(c->end - c->addr > VV_CHUNK ? VV_CHUNK : c->end - c->addr);
            PEEK(c->addr, c->n);
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
