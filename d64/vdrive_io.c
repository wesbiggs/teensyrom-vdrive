#include "vdrive_io.h"
#include "vd_cfg.h"
#include <string.h>

VD_COLD void vio_init(vio_t *v, vio_hot_t *hot)
{
    memset((void *)hot, 0, sizeof *hot);
    memset((void *)v, 0, sizeof *v);
    v->h = hot;
    hot->stat = VIO_OK;                   /* idle, nothing pending */
    hot->border = VIO_BRD_IDLE;
    hot->id0 = VIO_ID0_VAL;
    hot->id1 = VIO_ID1_VAL;
    vdrive_init(&v->vd);
    vio_publish_devices(v);
}

uint8_t vio_io1_read(const vio_hot_t *h, uint8_t off)
{
    switch (off) {
    case VIO_ARG:  return h->arg;
    case VIO_DATA: return h->data;
    case VIO_STAT: return h->stat;
    case VIO_ST:   return h->st;
    case VIO_LEN:  return h->len;
    case VIO_DEV0: return h->dev0;
    case VIO_DEV1: return h->dev1;
    case VIO_BORDER: return h->border;
    case VIO_ID0:  return h->id0;
    case VIO_ID1:  return h->id1;
    default:       return 0;
    }
}

void vio_io1_write(vio_hot_t *h, uint8_t off, uint8_t val)
{
    switch (off) {
    case VIO_ARG: h->arg = val; break;
    case VIO_LEN: h->len = val; break;
    case VIO_CMD:
        h->cmd = val;
        h->stat = VIO_BUSY;          /* visible to the 6502 before it can read STAT */
        h->pending = 1;
        break;
    default: break;
    }
}

uint8_t vio_io2_read(const vio_hot_t *h, uint8_t off) { return h->window[off & (VIO_WIN - 1)]; }
void    vio_io2_write(vio_hot_t *h, uint8_t off, uint8_t val) { h->window[off & (VIO_WIN - 1)] = val; }

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
    for (unsigned i = 0; i < v->h->len && i < VIO_WIN; i++) bus(v, VD_CIOUT, v->h->window[i], 0);
    bus(v, VD_UNLSN, 0, 0);
}

VD_COLD static void close_chan(vio_t *v, uint8_t dev, uint8_t sa)
{
    bus(v, VD_LISTEN, dev, 0); bus(v, VD_SECOND, sa, 0); bus(v, VD_UNLSN, 0, 0);
}

VD_COLD static void fload_open(vio_t *v)
{
    uint8_t dev = v->h->arg, b0, b1 = 0;
    v->fl_active = 0; v->fl_verify = 0;
    if (bus(v, VD_LISTEN, dev, 0) & 0x80) { v->h->data = 5; return; }
    bus(v, VD_UNLSN, 0, 0);                         /* finish probe transaction */
    send_name_open(v, dev, 0xF0);
    bus(v, VD_TALK, dev, 0); bus(v, VD_TKSA, 0x60, 0);
    int st0 = bus(v, VD_ACPTR, 0, &b0);
    if (st0 & 0x02) {                               /* nothing came back: file not found */
        bus(v, VD_UNTLK, 0, 0); close_chan(v, dev, 0xE0);
        v->h->data = 4; return;
    }
    v->fl_eof = (st0 & 0x40) != 0;
    if (!v->fl_eof) { int st1 = bus(v, VD_ACPTR, 0, &b1); v->fl_eof = (st1 & 0x40) != 0; }
    bus(v, VD_UNTLK, 0, 0);
    v->h->window[0] = b0; v->h->window[1] = b1;
    v->fl_active = 1; v->fl_dev = dev;
    v->h->data = 0;
}

/* The next bytes of the file being loaded, at most max, into buf. Returns the count; *st = the ST to answer ($40 last part, $42 a read
 * error ended it, 0 more to come). At the end the channel is closed. */
VD_COLD static unsigned fload_read(vio_t *v, uint8_t *buf, unsigned max, uint8_t *st_out)
{
    unsigned n = 0;
    uint8_t d;
    *st_out = 0x40;                                         /* default: EOI, empty */
    if (!v->fl_active) return 0;
    if (v->fl_eof) { close_chan(v, v->fl_dev, 0xE0); v->fl_active = 0; return 0; }
    int eof = 0, tmo = 0;
    bus(v, VD_TALK, v->fl_dev, 0); bus(v, VD_TKSA, 0x60, 0);
    while (n < max) {
        int st = bus(v, VD_ACPTR, 0, &d);
        if (st & 0x02) { eof = 1; tmo = 1; break; }       /* a read error (bad sector): the load ends here, ST shows $42 */
        buf[n++] = d;
        if (st & 0x40) { eof = 1; break; }
    }
    bus(v, VD_UNTLK, 0, 0);
    *st_out = (uint8_t)(eof ? (tmo ? 0x42 : 0x40) : 0);
    if (eof) { close_chan(v, v->fl_dev, 0xE0); v->fl_active = 0; }
    return n;
}

VD_COLD static void fload_blk(vio_t *v)
{
    uint8_t st;
    v->h->data = (uint8_t)fload_read(v, v->h->window, VIO_WIN, &st);
    v->h->st = st;
}

VD_COLD static void fload_abandon(vio_t *v)
{
    if (v->fl_active) { close_chan(v, v->fl_dev, 0xE0); v->fl_active = 0; }
}

VD_COLD static int dma_can(const vio_t *v, int need_read)
{
    return v->dma_write && v->dma_buf && v->dma_max && (!need_read || v->dma_read);
}

/* VIO_FLOAD_DMA after VIO_FVERIFY_OPEN: the next part compared with RAM at addr. 1 = not mine (a DMA read or write failed, or an address
 * the open did not allow): the KERNAL reports an error rather than a VERIFY result it did not get */
VD_COLD static int fverify_dma(vio_t *v, uint32_t addr)
{
    if (!dma_can(v, 1) || !vio_dma_rd_ok(addr)) { fload_abandon(v); return 1; }
    uint32_t room = 0xD000 - addr;
    uint8_t st, mem[256];
    unsigned n = fload_read(v, v->dma_buf, room < v->dma_max ? room : v->dma_max, &st);
    v->h->data = 0;
    for (unsigned off = 0; off < n; off += sizeof mem) {
        unsigned k = n - off < sizeof mem ? n - off : (unsigned)sizeof mem;
        if (!v->dma_read(v->dma_ctx, (uint16_t)(addr + off), mem, (uint16_t)k)) { fload_abandon(v); return 1; }
        if (memcmp(mem, v->dma_buf + off, k)) st |= 0x10;           /* VERIFY ERROR, and on to the end as the stock loop goes */
    }
    v->h->st = st;
    if (n) {
        uint32_t end = addr + n;
        uint8_t ptr[2] = { (uint8_t)end, (uint8_t)(end >> 8) };
        if (!v->dma_write(v->dma_ctx, VIO_DMA_PTR, ptr, 2)) { fload_abandon(v); return 1; }
    }
    return 0;
}

/* VIO_FLOAD_DMA: returns 1 (not mine) when a DMA write failed; the KERNAL then reports an error instead of loading stale bytes. */
VD_COLD static int fload_dma(vio_t *v)
{
    uint32_t addr = (uint32_t)v->h->arg | (uint32_t)v->h->len << 8;
    if (v->fl_verify) return fverify_dma(v, addr);
    if (!v->dma_write || !v->dma_buf || !v->dma_max || !vio_dma_ok(addr)) { fload_blk(v); return 0; }
    uint32_t room = (addr < 0xD000 ? 0xD000 : 0x10000) - addr;   /* stop at I/O and at the top of memory: the window takes over there */
    uint8_t st;
    unsigned n = fload_read(v, v->dma_buf, room < v->dma_max ? room : v->dma_max, &st);
    v->h->data = 0; v->h->st = st;
    if (n) {
        uint32_t end = addr + n;
        uint8_t ptr[2] = { (uint8_t)end, (uint8_t)(end >> 8) };
        if (!v->dma_write(v->dma_ctx, (uint16_t)addr, v->dma_buf, (uint16_t)n) || !v->dma_write(v->dma_ctx, VIO_DMA_PTR, ptr, 2)) {
            fload_abandon(v);
            return 1;
        }
    }
    return 0;
}

/* VIO_FVERIFY_OPEN: 1 = not mine (the KERNAL's stock VERIFY). Taken only when every byte of the file can be compared by DMA, so the
 * file is read through once for its length and then opened again. */
VD_COLD static int fverify_open(vio_t *v)
{
    if (!dma_can(v, 1)) return 1;
    uint8_t name[VIO_WIN], nlen = v->h->len, sa, at[2];
    memcpy(name, v->h->window, sizeof name);                /* fload_open puts the load address over the name */
    if (!v->dma_read(v->dma_ctx, VIO_DMA_SA, &sa, 1) || !v->dma_read(v->dma_ctx, VIO_DMA_ADDR, at, 2)) return 1;
    fload_open(v);
    if (v->h->data) return 0;                               /* not found / no device: reported as for a LOAD */
    uint32_t start = sa ? (uint32_t)v->h->window[0] | (uint32_t)v->h->window[1] << 8 : (uint32_t)at[0] | (uint32_t)at[1] << 8;
    uint32_t len = 0;
    while (v->fl_active) {
        uint8_t st;
        unsigned n = fload_read(v, v->dma_buf, v->dma_max, &st);   /* closes the channel at the end */
        if (!n) break;
        len += n;
    }
    fload_abandon(v);
    if (!vio_dma_rd_ok(start) || start + len > 0xD000) return 1;
    memcpy(v->h->window, name, sizeof name);
    v->h->len = nlen;
    fload_open(v);
    if (v->h->data) return 0;
    v->fl_verify = 1;
    return 0;
}

VD_COLD void vio_set_dma(vio_t *v, int (*write)(void *ctx, uint16_t addr, const uint8_t *buf, uint16_t len),
                         int (*read)(void *ctx, uint16_t addr, uint8_t *buf, uint16_t len), void *ctx, uint8_t *buf, uint16_t max)
{
    v->dma_write = write; v->dma_read = read; v->dma_ctx = ctx; v->dma_buf = buf; v->dma_max = max;
}

VD_COLD static void fsave_open(vio_t *v)
{
    uint8_t dev = v->h->arg;
    v->fs_active = 0; v->fs_fail = 0;
    if (bus(v, VD_LISTEN, dev, 0) & 0x80) { v->h->data = 5; return; }
    bus(v, VD_UNLSN, 0, 0);
    send_name_open(v, dev, 0xF1);
    int unit = vdrive_unit_for(&v->vd, dev);
    if (vdrive_chan_mode(&v->vd, unit, 1) == 3) { v->fs_active = 1; v->fs_dev = dev; v->h->data = 0; }
    else { int e = vdrive_pending_err(&v->vd, unit); v->h->data = (uint8_t)(e ? e : 74); }
}

VD_COLD static void fsave_bytes(vio_t *v, const uint8_t *buf, unsigned n)
{
    bus(v, VD_LISTEN, v->fs_dev, 0); bus(v, VD_SECOND, 0x61, 0);
    for (unsigned i = 0; i < n; i++) bus(v, VD_CIOUT, buf[i], 0);
    bus(v, VD_UNLSN, 0, 0);
}

/* 1 = not mine: the save was abandoned (fsave_dma) */
VD_COLD static int fsave_blk(vio_t *v)
{
    if (v->fs_fail) return 1;
    if (!v->fs_active) return 0;
    fsave_bytes(v, v->h->window, v->h->len < VIO_WIN ? v->h->len : VIO_WIN);
    return 0;
}

/* VIO_FSAVE_DMA. 1 = not mine (abandoned: the new $AC/$AD could not be written, so the KERNAL would send the same bytes again) */
VD_COLD static int fsave_dma(vio_t *v)
{
    v->h->data = 1;                                         /* nothing done: send a window block */
    if (v->fs_fail) return 1;
    if (!v->fs_active || !dma_can(v, 1)) return 0;
    uint8_t p[4];
    if (!v->dma_read(v->dma_ctx, VIO_DMA_SAVE, p, 4)) return 0;
    uint32_t cur = (uint32_t)p[0] | (uint32_t)p[1] << 8, end = (uint32_t)p[2] | (uint32_t)p[3] << 8;
    if (end <= cur || !vio_dma_rd_ok(cur)) return 0;        /* the end, or an address the CPU must read (the window path) */
    uint32_t n = end - cur, room = 0xD000 - cur;
    if (n > room) n = room;
    if (n > v->dma_max) n = v->dma_max;
    if (!v->dma_read(v->dma_ctx, (uint16_t)cur, v->dma_buf, (uint16_t)n)) return 0;   /* nothing changed: the window instead */
    uint32_t nxt = cur + n;
    uint8_t ptr[2] = { (uint8_t)nxt, (uint8_t)(nxt >> 8) };
    if (!v->dma_write(v->dma_ctx, VIO_DMA_SAVE, ptr, 2)) {
        close_chan(v, v->fs_dev, 0xE1);                     /* what came so far stays in the file, as after a save cut short */
        v->fs_active = 0; v->fs_fail = 1;
        return 1;
    }
    fsave_bytes(v, v->dma_buf, n);
    v->h->data = 0;
    return 0;
}

/* 1 = not mine: the save was abandoned (fsave_dma) */
VD_COLD static int fsave_close(vio_t *v)
{
    v->h->data = 0;
    if (v->fs_fail) { v->fs_fail = 0; return 1; }
    if (!v->fs_active) return 0;
    int unit = vdrive_unit_for(&v->vd, v->fs_dev);
    int ok = vdrive_chan_mode(&v->vd, unit, 1) == 3;
    close_chan(v, v->fs_dev, 0xE1);
    if (!ok) { int e = vdrive_pending_err(&v->vd, unit); v->h->data = (uint8_t)(e ? e : 74); }
    v->fs_active = 0;
    return 0;
}

VD_COLD void vio_publish_devices(vio_t *v) { v->h->dev0 = v->vd.dev[0]; v->h->dev1 = v->vd.dev[1]; }

VD_COLD int vio_arm_scan(vio_t *v)
{
    v->scan_state = 0;
    for (int i = 0; i < 2; i++)
        if (!v->vd.pin[i] && v->vd.unit[i].present) v->scan_state = 1;
    return v->scan_state;
}

VD_COLD int vio_probe_begin(vio_t *v)
{
    v->scan_state = 3;
    int need = vdrive_scan_begin(&v->vd);
    vio_publish_devices(v);
    return need;
}

VD_COLD int vio_probe_free(vio_t *v, uint8_t dev)
{
    int left = vdrive_scan_free(&v->vd, dev);
    vio_publish_devices(v);
    return left;
}

VD_COLD int vio_take_menu(vio_t *v)
{
    int r = v->menu_req;
    v->menu_req = 0;
    return r;
}

VD_COLD int vio_poll(vio_t *v)
{
    if (!v->h->pending) return 0;
    uint8_t c = v->h->cmd, notmine = 0, ask = 0;
    v->h->data = 0; v->h->st = 0;
    if (c == VD_LISTEN || c == VD_TALK || c == VIO_FLOAD_OPEN || c == VIO_FSAVE_OPEN || c == VIO_FVERIFY_OPEN) {   /* a transaction starts */
        if (v->scan_state == 1) { v->scan_state = 2; ask = 1; }
        else if (v->scan_state == 2) v->scan_state = 3;                                    /* asked, not answered: this KERNAL does not scan */
    }
    if (ask) {
        /* answer SCANREQ below, execute nothing */
    } else if (c >= VD_LISTEN && c <= VD_UNTLK) {
        uint8_t d, st;
        notmine = (uint8_t)vdrive_mailbox(&v->vd, c, v->h->arg, &d, &st);
        v->h->data = d; v->h->st = st;
    } else switch (c) {
        case VIO_FLOAD_OPEN:  if (vdrive_unit_for(&v->vd, v->h->arg) >= 0) fload_open(v);  else notmine = 1; break;
        case VIO_FLOAD_BLK:   fload_blk(v); break;
        case VIO_FLOAD_DMA:   notmine = (uint8_t)fload_dma(v); break;
        case VIO_FSAVE_OPEN:  if (vdrive_unit_for(&v->vd, v->h->arg) >= 0) fsave_open(v);  else notmine = 1; break;
        case VIO_FSAVE_BLK:   notmine = (uint8_t)fsave_blk(v); break;
        case VIO_FSAVE_CLOSE: notmine = (uint8_t)fsave_close(v); break;
        case VIO_FSAVE_DMA:   notmine = (uint8_t)fsave_dma(v); break;
        case VIO_FVERIFY_OPEN: if (vdrive_unit_for(&v->vd, v->h->arg) >= 0) notmine = (uint8_t)fverify_open(v); else notmine = 1; break;
        case VIO_SCAN:        v->scan_state = 3; v->h->data = (uint8_t)vdrive_scan_begin(&v->vd); break;
        case VIO_FREE:        v->h->data = (uint8_t)vdrive_scan_free(&v->vd, v->h->arg); break;
        case VIO_MENU:        v->menu_req = 1; break;
        default: notmine = 1;
    }
    vio_publish_devices(v);                             /* a U0> or a scan step may have moved a unit */
    v->h->pending = 0;                                  /* order: results first ... */
    v->h->stat = ask ? VIO_SCANREQ : notmine ? VIO_NOTMINE : VIO_OK;   /* ... then publish the answer token */
    return 1;
}

VD_COLD size_t vio_sizeof(void) { return sizeof(vio_t); }
VD_COLD size_t vio_hot_sizeof(void) { return sizeof(vio_hot_t); }
