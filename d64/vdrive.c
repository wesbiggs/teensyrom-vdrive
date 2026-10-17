#include "vdrive.h"
#include "vd_cfg.h"
#include <string.h>

#define ST_TIMEOUT 0x02
#define ST_EOI     0x40
#define ST_NODEV   0x80

/* ---- status channel ---------------------------------------------------- */
static const struct { uint8_t code; const char *msg; } MSGS[] = {
    {0, " OK"}, {1, "FILES SCRATCHED"}, {26, "WRITE PROTECT ON"}, {30, "SYNTAX ERROR"},
    {31, "SYNTAX ERROR"}, {33, "SYNTAX ERROR"}, {62, "FILE NOT FOUND"}, {63, "FILE EXISTS"},
    {70, "NO CHANNEL"}, {72, "DISK FULL"}, {74, "DRIVE NOT READY"},
};

VD_COLD static void set_err(vd_unit_t *u, uint8_t code, uint8_t t, uint8_t s)
{ u->err = code; u->err_t = t; u->err_s = s; }

static uint8_t *put2(uint8_t *p, unsigned n) { *p++ = (uint8_t)('0' + n / 10 % 10); *p++ = (uint8_t)('0' + n % 10); return p; }

/* Render "NN,MESSAGE,TT,SS\r" into statbuf and clear the error. */
VD_COLD static uint16_t status_text(vd_unit_t *u)
{
    uint8_t *p = u->statbuf;
    const char *m = "";
    for (unsigned i = 0; i < sizeof MSGS / sizeof MSGS[0]; i++) if (MSGS[i].code == u->err) m = MSGS[i].msg;
    p = put2(p, u->err); *p++ = ',';
    while (*m) *p++ = (uint8_t)*m++;
    *p++ = ','; p = put2(p, u->err_t); *p++ = ','; p = put2(p, u->err_s); *p++ = '\r';
    uint16_t n = (uint16_t)(p - u->statbuf);
    set_err(u, 0, 0, 0);
    u->stat_len = (uint8_t)n; u->stat_pos = 0;
    return n;
}

VD_COLD static void err_from(vd_unit_t *u, int r)
{
    switch (r) {
    case D64_E_FULL:    set_err(u, 72, 0, 0); break;
    case D64_E_EXISTS:  set_err(u, 63, 0, 0); break;
    case D64_E_NAME:    set_err(u, 33, 0, 0); break;
    case D64_E_RO:
    case D64_E_LOCKED:  set_err(u, 26, 0, 0); break;
    case D64_E_NOTFOUND:set_err(u, 62, 0, 0); break;
    default:            set_err(u, 74, 0, 0); break;
    }
}

/* ---- channels ---------------------------------------------------------- */
VD_COLD static vd_chan_t *chan_find(vd_unit_t *u, int ch)
{
    uint8_t i = u->slot_of[ch];
    return i ? &u->slot[i - 1] : NULL;
}

VD_COLD static void chan_close(vd_unit_t *u, int ch)
{
    vd_chan_t *c = chan_find(u, ch);
    if (!c) return;
    if (c->mode == 3 && u->disk) {
        int r = d64_close_write(u->disk, &c->u.w);
        if (r) err_from(u, r);
    }
    c->mode = 0;
    u->slot_of[ch] = 0;
}

VD_COLD static vd_chan_t *chan_alloc(vd_unit_t *u, int ch)
{
    chan_close(u, ch);
    for (int i = 0; i < VD_NSLOT; i++) {
        int used = 0;
        for (int k = 0; k < 16; k++) if (u->slot_of[k] == i + 1) used = 1;
        if (!used) { u->slot_of[ch] = (uint8_t)(i + 1); u->slot[i].mode = 0; return &u->slot[i]; }
    }
    set_err(u, 70, 0, 0);                              /* NO CHANNEL */
    return NULL;
}

VD_COLD static void close_all(vd_unit_t *u) { for (int i = 0; i < 16; i++) chan_close(u, i); }

/* name[0..len) -> split off a leading "0:" style drive prefix; returns pointer to name proper. */
VD_COLD static const char *strip_prefix(const char *p, const char *end)
{
    for (const char *q = p; q < end && q < p + 3; q++) if (*q == ':') return q + 1;
    return p;
}

typedef struct { char name[17]; int has_name; uint8_t type; char mode; int replace; int ok; } open_spec_t;

VD_COLD static open_spec_t parse_open(const uint8_t *raw, uint8_t len)
{
    open_spec_t o; memset(&o, 0, sizeof o);
    char buf[VD_CMDBUF + 1];
    memcpy(buf, raw, len); buf[len] = 0;
    const char *p = buf, *end = buf + len;
    if (*p == '@') { o.replace = 1; p++; }
    p = strip_prefix(p, end);
    const char *comma = p;
    while (comma < end && *comma != ',') comma++;
    size_t nl = (size_t)(comma - p);
    if (nl > 16) { o.ok = 0; return o; }
    memcpy(o.name, p, nl); o.name[nl] = 0; o.has_name = nl > 0;
    o.ok = 1;
    while (comma < end) {
        const char *f = comma + 1;
        switch (*f) {
        case 'P': o.type = 2; break;
        case 'S': o.type = 1; break;
        case 'U': o.type = 3; break;
        case 'L': o.type = 4; break;
        case 'R': case 'W': case 'A': o.mode = *f; break;
        default: break;
        }
        comma = f;
        while (comma < end && *comma != ',') comma++;
    }
    return o;
}

static const char *const DIRTYPES[8] = { "DEL", "SEQ", "PRG", "USR", "REL", "???", "???", "???" };

VD_COLD static int has_d64_suffix(const char *s)
{
    size_t l = strlen(s);
    if (l < 4) return 0;
    const char *e = s + l - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'd' && e[2] == '6' && e[3] == '4';
}

/* One line of the directory per call into d->line; sets d->phase = 3 and llen = 0 when finished. */
VD_COLD static void dir_gen(vdrive_t *v, int unit, vd_unit_t *u, vd_dirstream_t *d)
{
    d64_bas_t b;
    char name[17], id[3], dos[3];
    d->lpos = 0; d->llen = 0;
    while (d->phase != 3) {
        d64_bas_begin_at(&b, d->line, sizeof d->line, d->off);
        if (d->phase == 0) {
            if (d->host) {
                char title[17];
                if (v->host.dir_open(v->host.ctx, unit, title)) { d->phase = 3; return; }
                { char idn[3] = { (char)('0' + v->dev[unit] / 10 % 10), (char)('0' + v->dev[unit] % 10), 0 };   /* shows the device number */
                  d64_bas_header(&b, title, idn, "2A"); }
            } else {
                if (d64_header(u->disk, name, id, dos) || d64_dir_first(u->disk, &d->it)) { d->phase = 3; return; }
                d64_bas_header(&b, name, id, dos);
            }
            d->phase = 1;
        } else if (d->phase == 1) {
            int got = 0;
            for (;;) {
                if (d->host) {
                    uint32_t size; int is_dir;
                    if (v->host.dir_next(v->host.ctx, unit, name, &size, &is_dir) <= 0) break;
                    if (d->has_pat && !d64_match(name, d->pat)) continue;
                    uint32_t blocks = is_dir ? 0 : (size + 253) / 254;
                    d64_bas_entry(&b, (uint16_t)(blocks > 65535 ? 65535 : blocks), name,
                                  is_dir ? "DIR" : has_d64_suffix(name) ? "D64" : "PRG", 1, 0);
                    got = 1; break;
                } else {
                    d64_entry_t e;
                    int r = d64_dir_next(u->disk, &d->it, &e);
                    if (r < 0) { d->phase = 3; return; }       /* damaged directory: stop listing */
                    if (r == 0) break;
                    if (d->has_pat && !d64_match(e.name, d->pat)) continue;
                    d64_bas_entry(&b, e.blocks, e.name, DIRTYPES[e.type & 7], (e.type & 0x80) != 0, (e.type & 0x40) != 0);
                    got = 1; break;
                }
            }
            if (!got) { d->phase = 2; continue; }
        } else {
            int fr = d->host ? 0 : d64_blocks_free(u->disk);
            if (fr < 0) fr = 0;
            d64_bas_footer(&b, (uint16_t)fr);
            d64_bas_end(&b);
            d->phase = 3;
        }
        d->llen = (uint8_t)b.len;
        d->off = (uint16_t)(d->off + b.len);
        return;
    }
}

VD_COLD static void open_dir(vdrive_t *v, int unit, vd_unit_t *u, int ch, const uint8_t *raw, uint8_t len)
{
    vd_chan_t *c = chan_alloc(u, ch);
    if (!c) return;
    vd_dirstream_t *d = &c->u.d;
    memset(d, 0, sizeof *d);
    d->host = u->disk == NULL;
    size_t n = 0;
    uint8_t i = 1;
    while (i < len && raw[i] >= '0' && raw[i] <= '9') i++;
    if (i < len && raw[i] == ':') i++;
    while (i < len && raw[i] != ',' && n < 16) d->pat[n++] = (char)raw[i++];
    d->pat[n] = 0; d->has_pat = n != 0;
    c->mode = 4;
    dir_gen(v, unit, u, d);                            /* first line now, so failures report at OPEN time */
    if (d->llen == 0) { chan_close(u, ch); set_err(u, 74, 0, 0); }
}

VD_COLD static void chan_open(vdrive_t *v, int unit, vd_unit_t *u, int ch, const uint8_t *raw, uint8_t len)
{
    chan_close(u, ch);
    if (len && raw[0] == '$' && (u->disk || (v->host.dir_open && v->host.dir_next))) { open_dir(v, unit, u, ch, raw, len); return; }
    if (!u->disk) { set_err(u, 74, 0, 0); return; }
    open_spec_t o = parse_open(raw, len);
    if (!o.ok || !o.has_name) { set_err(u, 33, 0, 0); return; }
    char mode = o.mode ? o.mode : (ch == 1 ? 'W' : 'R');
    if (mode != 'R' && mode != 'W') { set_err(u, 33, 0, 0); return; }   /* append not supported */
    vd_chan_t *c = chan_alloc(u, ch);
    if (!c) return;
    if (mode == 'R') {
        d64_entry_t e;
        int r = d64_find(u->disk, o.name, &e);
        if (r < 0) { err_from(u, r); chan_close(u, ch); return; }
        if (r == 0) { set_err(u, 62, 0, 0); chan_close(u, ch); return; }
        d64_file_open(u->disk, &e, &c->u.r);
        c->mode = 1;
    } else {
        uint8_t type = o.type ? o.type : (ch == 1 ? 2 : 1);
        int r = d64_create(u->disk, o.name, type, o.replace, &c->u.w);
        if (r) { err_from(u, r); chan_close(u, ch); return; }
        c->mode = 3;
    }
}

/* ---- command channel ---------------------------------------------------- */
VD_COLD static void cmd_cd(vdrive_t *v, int unit, vd_unit_t *u, const char *p, const char *end)
{
    if (p < end && *p == ':') p++;
    char name[VD_CMDBUF]; size_t n = (size_t)(end - p);
    memcpy(name, p, n); name[n] = 0;
    const vd_host_t *h = &v->host;
    int up = (!strcmp(name, "..") || !strcmp(name, "_") || (n == 1 && (uint8_t)name[0] == 0x5F));
    int root = (n == 1 && name[0] == '/');                     /* CD:/ = unmount (if mounted) and go to the card root */
    if (u->disk && (up || root)) {
        close_all(u); u->disk = NULL;
        if (h->unmount) h->unmount(h->ctx, unit);
        if (root && h->chdir) h->chdir(h->ctx, unit, "/");
    } else if (n && has_d64_suffix(name)) {
        d64_t *d = NULL;
        if (!h->mount) { set_err(u, 33, 0, 0); return; }
        int r = h->mount(h->ctx, unit, name, &d);
        if (r) set_err(u, (uint8_t)r, 0, 0);
        else { close_all(u); u->disk = d; }
    } else {
        if (!h->chdir) { set_err(u, 33, 0, 0); return; }
        int r = h->chdir(h->ctx, unit, name);
        if (r) set_err(u, (uint8_t)r, 0, 0);
    }
}

/* "U0>"+CHR$(n): this drive answers as device n from now on (n = 0 restores the default). Persisted via the host. */
VD_COLD static void cmd_renumber(vdrive_t *v, int unit, vd_unit_t *u, uint8_t n)
{
    uint8_t def[2] = { VD_DEV0, VD_DEV1 };
    uint8_t eff = n ? n : def[unit];
    if (eff < 8 || eff > 30 || eff == v->dev[1 - unit]) { set_err(u, 33, 0, 0); return; }
    v->dev[unit] = eff;
    if (v->host.save_devices)
        v->host.save_devices(v->host.ctx, v->dev[0] == def[0] ? 0 : v->dev[0], v->dev[1] == def[1] ? 0 : v->dev[1]);
}

VD_COLD static void exec_command(vdrive_t *v, int unit, vd_unit_t *u, uint8_t *cmd, uint8_t len)
{
    /* OPEN 15,d,15,"U0>"+CHR$(n) sends 4 bytes; PRINT#15 adds a CR, making 5 */
    if ((len == 4 || (len == 5 && cmd[4] == '\r')) && cmd[0] == 'U' && cmd[1] == '0' && cmd[2] == '>') { cmd_renumber(v, unit, u, cmd[3]); return; }
    while (len && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n')) len--;
    if (!len) return;
    cmd[len] = 0;
    if (len >= 2 && cmd[0] == 'C' && cmd[1] == 'D') { cmd_cd(v, unit, u, (const char *)cmd + 2, (const char *)cmd + len); return; }
    if (!u->disk) { set_err(u, 74, 0, 0); return; }
    const char *p = (const char *)cmd + 1, *end = (const char *)cmd + len;
    while (p < end && *p >= '0' && *p <= '9') p++;       /* optional drive number */
    switch (cmd[0]) {
    case 'S': {
        if (p < end && *p == ':') p++;
        int total = 0;
        while (p <= end) {
            const char *q = p;
            while (q < end && *q != ',') q++;
            char pat[18]; size_t n = (size_t)(q - p);
            if (n && n <= 16) {
                memcpy(pat, p, n); pat[n] = 0;
                int r = d64_scratch(u->disk, pat);
                if (r < 0) { err_from(u, r); return; }
                total += r;
            }
            p = q + 1;
        }
        set_err(u, 1, (uint8_t)total, 0);
        break; }
    case 'N': {
        if (p < end && *p == ':') p++;
        const char *comma = p;
        while (comma < end && *comma != ',') comma++;
        char name[17], id[3];
        size_t n = (size_t)(comma - p);
        if (n < 1 || n > 16) { set_err(u, 33, 0, 0); break; }
        memcpy(name, p, n); name[n] = 0;
        int r;
        if (comma < end && end - comma - 1 == 2) { id[0] = comma[1]; id[1] = comma[2]; id[2] = 0; r = d64_format(u->disk, name, id); }
        else if (comma == end) r = d64_format(u->disk, name, NULL);
        else { set_err(u, 33, 0, 0); break; }
        if (r) err_from(u, r);
        break; }
    case 'R': {
        if (p < end && *p == ':') p++;
        const char *eq = p;
        while (eq < end && *eq != '=') eq++;
        char nn[17], on[17];
        size_t a = (size_t)(eq - p), b = (size_t)(end - eq - 1);
        if (eq == end || a < 1 || a > 16 || b < 1 || b > 16) { set_err(u, 34, 0, 0); break; }
        memcpy(nn, p, a); nn[a] = 0; memcpy(on, eq + 1, b); on[b] = 0;
        int r = d64_rename(u->disk, on, nn);
        if (r) err_from(u, r);
        break; }
    case 'U':                                          /* UI / UJ etc.: nothing to do */
        break;
    case 'I': case 'V':                                /* initialize / validate: nothing to do */
        break;
    default:
        set_err(u, 31, 0, 0);
    }
}

/* ---- public ------------------------------------------------------------- */
VD_COLD void vdrive_init(vdrive_t *v)
{
    memset(v, 0, sizeof *v);
    v->act = -1;
    v->dev[0] = VD_DEV0; v->dev[1] = VD_DEV1;
}

VD_COLD void vdrive_attach(vdrive_t *v, int unit, d64_t *disk, int present)
{
    vd_unit_t *u = &v->unit[unit];
    close_all(u);
    u->disk = disk; u->present = (uint8_t)present;
    set_err(u, 0, 0, 0);
}

VD_COLD void vdrive_set_devices(vdrive_t *v, uint8_t dev0, uint8_t dev1) { v->dev[0] = dev0; v->dev[1] = dev1; }
VD_COLD int vdrive_unit_for(const vdrive_t *v, uint8_t dev) { return dev == v->dev[0] ? 0 : dev == v->dev[1] ? 1 : -1; }
VD_COLD void vdrive_set_host(vdrive_t *v, const vd_host_t *host) { v->host = *host; }
VD_COLD int vdrive_chan_mode(const vdrive_t *v, int unit, int ch)
{
    uint8_t i = v->unit[unit].slot_of[ch];
    return i ? v->unit[unit].slot[i - 1].mode : 0;
}
VD_COLD int vdrive_pending_err(const vdrive_t *v, int unit) { return v->unit[unit].err; }
VD_COLD size_t vdrive_sizeof(void) { return sizeof(vdrive_t); }

VD_COLD static int read_byte(vdrive_t *v, int unit, vd_unit_t *u, vd_chan_t *c, uint8_t *b, int *eoi)
{
    if (c->mode == 1) {
        int n = d64_file_read(u->disk, &c->u.r, b, 1);
        if (n <= 0) return 0;
        *eoi = d64_file_eof(&c->u.r);
        return 1;
    }
    if (c->mode == 4) {
        vd_dirstream_t *d = &c->u.d;
        if (d->lpos >= d->llen) dir_gen(v, unit, u, d);
        if (d->lpos >= d->llen) return 0;
        *b = d->line[d->lpos++];
        *eoi = d->lpos >= d->llen && d->phase == 3;
        return 1;
    }
    return 0;
}

VD_COLD int vdrive_mailbox(vdrive_t *v, uint8_t cmd, uint8_t arg, uint8_t *data, uint8_t *st)
{
    *data = 0; *st = 0;
    if (cmd == VD_LISTEN || cmd == VD_TALK) {
        v->talking = (cmd == VD_TALK);
        v->wlen = 0;
        int unit = vdrive_unit_for(v, arg);
        if (unit >= 0) {
            v->act = (int8_t)unit;
            if (!v->unit[v->act].present) { v->act = 2; *st = ST_NODEV; }
            return 0;
        }
        v->act = -1;
        return 1;
    }
    if (v->act < 0) return 1;                          /* not our transaction */
    if (v->act == 2) return 0;                         /* absent device: swallow quietly */
    vd_unit_t *u = &v->unit[v->act];
    int ch = v->sa & 0xF, op = v->sa & 0xF0;

    switch (cmd) {
    case VD_SECOND: case VD_TKSA:
        v->sa = arg; v->wlen = 0;
        if (cmd == VD_TKSA && (arg & 0xF) == 15 && u->stat_pos >= u->stat_len) status_text(u);
        return 0;
    case VD_CIOUT:
        if (op == 0x60 && ch != 15) {
            vd_chan_t *c = chan_find(u, ch);
            if (c && c->mode == 3) {
                int r = d64_write(u->disk, &c->u.w, &arg, 1);
                if (r < 0) { err_from(u, r); c->mode = 0; chan_close(u, ch); *st = ST_TIMEOUT; }   /* write already aborted: just release the slot */
            }
        } else if (v->wlen < VD_CMDBUF - 1) {
            v->wbuf[v->wlen++] = arg;
        }
        return 0;
    case VD_UNLSN:
        if (op == 0xF0) {
            if (ch == 15) exec_command(v, v->act, u, v->wbuf, v->wlen);
            else chan_open(v, v->act, u, ch, v->wbuf, v->wlen);
        } else if (op == 0x60 && ch == 15) {
            exec_command(v, v->act, u, v->wbuf, v->wlen);
        } else if (op == 0xE0) {
            if (ch == 15) close_all(u); else chan_close(u, ch);
        }
        v->wlen = 0; v->act = -1;
        return 0;
    case VD_ACPTR: {
        int eoi = 0;
        if (ch == 15) {                                /* error channel */
            if (u->stat_pos < u->stat_len) { *data = u->statbuf[u->stat_pos++]; if (u->stat_pos >= u->stat_len) *st = ST_EOI; }
            else { *data = '\r'; *st = ST_EOI; }
            return 0;
        }
        vd_chan_t *c = chan_find(u, ch);
        if (!c || !read_byte(v, v->act, u, c, data, &eoi)) { *st = ST_TIMEOUT | ST_EOI; return 0; }
        if (eoi) *st = ST_EOI;
        return 0; }
    case VD_UNTLK:
        v->act = -1;
        return 0;
    }
    return 1;
}
