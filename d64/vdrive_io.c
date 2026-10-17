#include "vdrive_io.h"
#include "vd_cfg.h"
#include <string.h>

VD_COLD void vio_init(vio_t *v)
{
    memset((void *)v, 0, sizeof *v);
    vdrive_init(&v->vd);
}

uint8_t vio_io1_read(const vio_t *v, uint8_t off)
{
    switch (off) {
    case VIO_ARG:  return v->arg;
    case VIO_DATA: return v->data;
    case VIO_STAT: return v->stat;
    case VIO_ST:   return v->st;
    case VIO_LEN:  return v->len;
    default:       return 0;
    }
}

VD_COLD void vio_io1_write(vio_t *v, uint8_t off, uint8_t val)
{
    switch (off) {
    case VIO_ARG: v->arg = val; break;
    case VIO_LEN: v->len = val; break;
    case VIO_CMD:
        v->cmd = val;
        v->stat = VIO_BUSY;          /* visible to the 6502 before it can read STAT */
        v->pending = 1;
        break;
    default: break;
    }
}

uint8_t vio_io2_read(const vio_t *v, uint8_t off) { return v->window[off]; }
void    vio_io2_write(vio_t *v, uint8_t off, uint8_t val) { v->window[off] = val; }

/* ---- fast paths, built from the byte-level commands ---- */
VD_COLD static int bus(vio_t *v, uint8_t c, uint8_t a, uint8_t *data)
{
    uint8_t st = 0, d = 0;
    vdrive_mailbox(&v->vd, c, a, &d, &st);
    if (data) *data = d;
    return st;
}

VD_COLD static void send_name_open(vio_t *v, uint8_t dev, uint8_t sa)
{
    bus(v, VD_LISTEN, dev, 0);
    bus(v, VD_SECOND, sa, 0);
    for (unsigned i = 0; i < v->len; i++) bus(v, VD_CIOUT, v->window[i], 0);
    bus(v, VD_UNLSN, 0, 0);
}

VD_COLD static void close_chan(vio_t *v, uint8_t dev, uint8_t sa)
{
    bus(v, VD_LISTEN, dev, 0); bus(v, VD_SECOND, sa, 0); bus(v, VD_UNLSN, 0, 0);
}

VD_COLD static void fload_open(vio_t *v)
{
    uint8_t dev = v->arg, b0, b1 = 0;
    v->fl_active = 0;
    if (bus(v, VD_LISTEN, dev, 0) & 0x80) { v->data = 5; return; }
    bus(v, VD_UNLSN, 0, 0);                         /* finish probe transaction */
    send_name_open(v, dev, 0xF0);
    bus(v, VD_TALK, dev, 0); bus(v, VD_TKSA, 0x60, 0);
    int st0 = bus(v, VD_ACPTR, 0, &b0);
    if (st0 & 0x02) {                               /* nothing came back: file not found */
        bus(v, VD_UNTLK, 0, 0); close_chan(v, dev, 0xE0);
        v->data = 4; return;
    }
    v->fl_eof = (st0 & 0x40) != 0;
    if (!v->fl_eof) { int st1 = bus(v, VD_ACPTR, 0, &b1); v->fl_eof = (st1 & 0x40) != 0; }
    bus(v, VD_UNTLK, 0, 0);
    v->window[0] = b0; v->window[1] = b1;
    v->fl_active = 1; v->fl_dev = dev;
    v->data = 0;
}

VD_COLD static void fload_blk(vio_t *v)
{
    uint8_t n = 0, d;
    v->data = 0; v->st = 0x40;                       /* default: EOI, empty */
    if (!v->fl_active) return;
    if (v->fl_eof) { close_chan(v, v->fl_dev, 0xE0); v->fl_active = 0; return; }
    int eof = 0;
    bus(v, VD_TALK, v->fl_dev, 0); bus(v, VD_TKSA, 0x60, 0);
    while (n < 255) {
        int st = bus(v, VD_ACPTR, 0, &d);
        if (st & 0x02) { eof = 1; break; }
        v->window[n++] = d;
        if (st & 0x40) { eof = 1; break; }
    }
    bus(v, VD_UNTLK, 0, 0);
    v->data = n;
    v->st = eof ? 0x40 : 0;
    if (eof) { close_chan(v, v->fl_dev, 0xE0); v->fl_active = 0; }
}

VD_COLD static void fsave_open(vio_t *v)
{
    uint8_t dev = v->arg;
    v->fs_active = 0;
    if (bus(v, VD_LISTEN, dev, 0) & 0x80) { v->data = 5; return; }
    bus(v, VD_UNLSN, 0, 0);
    send_name_open(v, dev, 0xF1);
    int unit = vdrive_unit_for(&v->vd, dev);
    if (vdrive_chan_mode(&v->vd, unit, 1) == 3) { v->fs_active = 1; v->fs_dev = dev; v->data = 0; }
    else { int e = vdrive_pending_err(&v->vd, unit); v->data = (uint8_t)(e ? e : 74); }
}

VD_COLD static void fsave_blk(vio_t *v)
{
    if (!v->fs_active) return;
    bus(v, VD_LISTEN, v->fs_dev, 0); bus(v, VD_SECOND, 0x61, 0);
    for (unsigned i = 0; i < v->len; i++) bus(v, VD_CIOUT, v->window[i], 0);
    bus(v, VD_UNLSN, 0, 0);
}

VD_COLD static void fsave_close(vio_t *v)
{
    v->data = 0;
    if (!v->fs_active) return;
    int unit = vdrive_unit_for(&v->vd, v->fs_dev);
    int ok = vdrive_chan_mode(&v->vd, unit, 1) == 3;
    close_chan(v, v->fs_dev, 0xE1);
    if (!ok) { int e = vdrive_pending_err(&v->vd, unit); v->data = (uint8_t)(e ? e : 74); }
    v->fs_active = 0;
}

VD_COLD int vio_poll(vio_t *v)
{
    if (!v->pending) return 0;
    uint8_t c = v->cmd, notmine = 0;
    v->data = 0; v->st = 0;
    if (c >= VD_LISTEN && c <= VD_UNTLK) {
        uint8_t d, st;
        notmine = (uint8_t)vdrive_mailbox(&v->vd, c, v->arg, &d, &st);
        v->data = d; v->st = st;
    } else switch (c) {
        case VIO_FLOAD_OPEN:  if (vdrive_unit_for(&v->vd, v->arg) >= 0) fload_open(v);  else notmine = 1; break;
        case VIO_FLOAD_BLK:   fload_blk(v); break;
        case VIO_FSAVE_OPEN:  if (vdrive_unit_for(&v->vd, v->arg) >= 0) fsave_open(v);  else notmine = 1; break;
        case VIO_FSAVE_BLK:   fsave_blk(v); break;
        case VIO_FSAVE_CLOSE: fsave_close(v); break;
        default: notmine = 1;
    }
    v->pending = 0;                                  /* order: results first ... */
    v->stat = notmine ? VIO_NOTMINE : 0;             /* ... then release BUSY */
    return 1;
}

VD_COLD size_t vio_sizeof(void) { return sizeof(vio_t); }
