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
    {21, "READ ERROR"}, {25, "WRITE ERROR"}, {65, "NO BLOCK"}, {66, "ILLEGAL TRACK OR SECTOR"}, {70, "NO CHANNEL"}, {72, "DISK FULL"}, {73, "TRVDRIVE V1.0"}, {74, "DRIVE NOT READY"},
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

VD_COLD static void close_all(vd_unit_t *u);

/* Undo the temporary selection of a listing by path: the folder / image that was selected before is selected again. */
VD_COLD static void restore_selection(vd_unit_t *u)
{
    u->restore_ch = 0;
    close_all(u);                                                 /* anything opened on the temporary image meanwhile goes with it */
    vdrive_t *v = u->owner;
    d64_t *d = NULL;
    u->disk = NULL;
    if (v && v->host.pop) v->host.pop(v->host.ctx, u->index, &d);
    u->disk = d;
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
    if (u->restore_ch == ch + 1) restore_selection(u);
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

typedef struct { char name[VD_CMDBUF]; int has_name; uint8_t type; char mode; int replace; int ok; } open_spec_t;

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

/* 64, 71 or 81 for a name ending in .D64 / .D71 / .D81 (PETSCII or ASCII, either case), else 0 */
VD_COLD static int image_kind(const char *s)
{
    size_t l = strlen(s);
    if (l < 4) return 0;
    const uint8_t *e = (const uint8_t *)s + l - 4;
    if ((e[0] & 0x7F) != '.' || ((e[1] & 0x7F) | 0x20) != 'd') return 0;
    if (e[2] == '6' && e[3] == '4') return 64;
    if (e[2] == '7' && e[3] == '1') return 71;
    if (e[2] == '8' && e[3] == '1') return 81;
    return 0;
}

VD_COLD static const char *image_tag(const char *s) { int k = image_kind(s); return k == 71 ? "D71" : k == 81 ? "D81" : "D64"; }

VD_COLD static void leave_image(vdrive_t *v, int unit, vd_unit_t *u)
{
    if (!u->disk) return;
    close_all(u); u->disk = NULL;
    if (v->host.unmount) v->host.unmount(v->host.ctx, unit);
}

/* Follow "[/]dir/dir/image.d64[/rest]" on the host: a leading "/" starts at the card root, ".." (or the left arrow "_") goes
 * up, a component with an image suffix is mounted, any other component is a folder. A component with a wildcard is not
 * entered: it ends the walk as the pattern. *rest points at what is left (the file name after an image, a pattern, or ""
 * when the path ended on the folder / image). Returns 0 or a DOS error code. Modifies p. */
VD_COLD static int path_walk(vdrive_t *v, int unit, vd_unit_t *u, char *p, char **rest)
{
    const vd_host_t *h = &v->host;
    int r;
    *rest = p + strlen(p);
    if (!h->chdir || !h->mount) return 33;
    if (*p == '/') { leave_image(v, unit, u); if ((r = h->chdir(h->ctx, unit, "/"))) return r; }
    while (*p) {
        char *c = p, *sl = strchr(p, '/');
        if (sl) { *sl = 0; p = sl + 1; } else p += strlen(p);
        if (!*c) continue;                                                  /* "//" or a trailing "/" */
        if (strpbrk(c, "*?")) { if (sl) *sl = '/'; *rest = c; return 0; }
        if (!strcmp(c, "..") || !strcmp(c, "_")) {
            if (u->disk) leave_image(v, unit, u);                           /* up from an image = unmount */
            else if ((r = h->chdir(h->ctx, unit, ".."))) return r;
        } else if (image_kind(c)) {
            d64_t *d = NULL;
            leave_image(v, unit, u);
            if ((r = h->mount(h->ctx, unit, c, &d))) return r;
            u->disk = d;
            *rest = p;
            return 0;
        } else {
            leave_image(v, unit, u);
            if ((r = h->chdir(h->ctx, unit, c))) return r;
        }
    }
    return 0;
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
                                  is_dir ? "DIR" : image_kind(name) ? image_tag(name) : "PRG", 1, 0);
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

VD_COLD static void open_dir_pat(vdrive_t *v, int unit, vd_unit_t *u, int ch, const char *pat)
{
    vd_chan_t *c = chan_alloc(u, ch);
    if (!c) return;
    vd_dirstream_t *d = &c->u.d;
    memset(d, 0, sizeof *d);
    d->host = u->disk == NULL;
    size_t n = 0;
    while (pat[n] && n < 16) { d->pat[n] = pat[n]; n++; }
    d->pat[n] = 0; d->has_pat = n != 0;
    c->mode = 4;
    dir_gen(v, unit, u, d);                            /* first line now, so failures report at OPEN time */
    if (d->llen == 0) { chan_close(u, ch); set_err(u, 74, 0, 0); }
}

/* A listing by path only shows what is there: the folder / image selected before is restored when its channel closes.
 * begin_temp remembers it (first closing an earlier listing that is still open), end_temp arms the restore for channel ch. */
VD_COLD static void begin_temp(vdrive_t *v, int unit, vd_unit_t *u)
{
    if (u->restore_ch) chan_close(u, u->restore_ch - 1);
    v->host.push(v->host.ctx, unit);
}
VD_COLD static void end_temp(vd_unit_t *u, int ch)
{
    if (chan_find(u, ch)) u->restore_ch = (uint8_t)(ch + 1);     /* restored by that channel's close */
    else restore_selection(u);                                    /* the listing never opened */
}

/* "$[n][:]pattern": the directory of the mounted image, or of the current host folder. A pattern with a "/" in it (or, with
 * nothing mounted, ending in an image suffix) is a path: LOAD"$/GAMES/DEMO.D64" mounts the image and lists it,
 * LOAD"$GAMES/" lists that folder, LOAD"$GAMES/DEMO.D64/S*" lists the matching files in the image.
 * "$==path" (also "$:==" / "$0:==") is always a path, even without a "/" or an image suffix: $==GAMES enters the folder GAMES,
 * $==T a folder called T (a single "=" is a CBM listing modifier, $=T / $=P, and is left alone). */
VD_COLD static void open_dir(vdrive_t *v, int unit, vd_unit_t *u, int ch, const uint8_t *raw, uint8_t len)
{
    char spec[VD_CMDBUF];
    size_t n = 0;
    uint8_t i = 1;
    while (i < len && raw[i] >= '0' && raw[i] <= '9') i++;
    if (i < len && raw[i] == ':') i++;
    int force = i + 1 < len && raw[i] == '=' && raw[i + 1] == '=';     /* "$==path": always a path (a single "=" is a CBM listing modifier: $=T, $=P) */
    if (force) i += 2;
    while (i < len && raw[i] != ',' && n < sizeof spec - 1) spec[n++] = (char)raw[i++];
    spec[n] = 0;
    char *pat = spec;
    if (force || strchr(spec, '/') || (!u->disk && !strpbrk(spec, "*?") && image_kind(spec))) {
        int temp = !force && v->host.push && v->host.pop;
        if (temp) begin_temp(v, unit, u);
        int r = path_walk(v, unit, u, spec, &pat);
        if (r) { set_err(u, (uint8_t)r, 0, 0); if (temp) restore_selection(u); return; }
        open_dir_pat(v, unit, u, ch, pat);
        if (temp) end_temp(u, ch);
        return;
    }
    open_dir_pat(v, unit, u, ch, pat);
}
VD_COLD static void chan_open(vdrive_t *v, int unit, vd_unit_t *u, int ch, const uint8_t *raw, uint8_t len)
{
    chan_close(u, ch);
    if (len && raw[0] == '$' && (u->disk || (v->host.dir_open && v->host.dir_next))) { open_dir(v, unit, u, ch, raw, len); return; }
    if (len && raw[0] == '#') {                                      /* direct-access buffer: U1/U2/B-P work on it */
        vd_chan_t *c = chan_alloc(u, ch);
        if (!c) return;
        c->mode = 5; c->u.b.pos = 0;
        memset(c->u.b.buf, 0, sizeof c->u.b.buf);
        return;
    }
    open_spec_t o = parse_open(raw, len);
    if (!o.has_name) { set_err(u, u->disk ? 33 : 74, 0, 0); return; }
    char mode = o.mode ? o.mode : (ch == 1 ? 'W' : 'R');
    if (mode != 'R' && mode != 'W') { set_err(u, 33, 0, 0); return; }   /* append not supported */
    /* "DIR/IMAGE.D64/FILE" (or a bare "IMAGE.D64" read with nothing mounted): walk there first, the file name is what is left */
    if (strchr(o.name, '/') || (mode == 'R' && !u->disk && image_kind(o.name))) {
        char *rest;
        int temp = mode == 'R' && v->host.push && v->host.pop;
        if (temp) begin_temp(v, unit, u);
        int r = path_walk(v, unit, u, o.name, &rest);
        if (r) { set_err(u, (uint8_t)r, 0, 0); if (temp) restore_selection(u); return; }
        if (!*rest) {                                                         /* it named a folder / image: show what is in it, nothing stays selected */
            open_dir_pat(v, unit, u, ch, "");
            if (temp) end_temp(u, ch);
            return;
        }
        memmove(o.name, rest, strlen(rest) + 1);                              /* a file through a path: the folder / image stay selected for the program */
        if (temp) v->host.pop(v->host.ctx, unit, NULL);                       /* (out == NULL: keep the selection, drop the snapshot) */
    }
    if (strlen(o.name) > 16) { set_err(u, 33, 0, 0); return; }
    if (!u->disk) { set_err(u, 74, 0, 0); return; }
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
/* CD[n]:path -- same path syntax as a LOAD name (see path_walk): CD:/GAMES/DEMO.D64, CD:.., CD:_ (left arrow), CD:/ (root). */
VD_COLD static void cmd_cd(vdrive_t *v, int unit, vd_unit_t *u, const char *p, const char *end)
{
    const char *q = p;
    while (q < end && *q >= '0' && *q <= '9') q++;
    if (q < end && *q == ':') p = q + 1;                         /* "CD:" and "CD0:" */
    char path[VD_CMDBUF]; size_t n = (size_t)(end - p);
    if (n >= sizeof path) { set_err(u, 33, 0, 0); return; }
    memcpy(path, p, n); path[n] = 0;
    if (!n) return;
    char *rest;
    int r = path_walk(v, unit, u, path, &rest);
    if (r) set_err(u, (uint8_t)r, 0, 0);
    else if (*rest) set_err(u, 33, 0, 0);                        /* something after the image / a wildcard: not a directory */
}

/* UI / UJ / U9: reset the drive. Closes every channel, keeps the mounted image, answers the "73" power-on message. */
VD_COLD static void cmd_reset(vd_unit_t *u)
{
    close_all(u);
    set_err(u, 73, 0, 0);
}

/* Device numbers: a unit uses its pinned number, or else the default / next free one until the start-up scan moves it. */
VD_COLD static void settle(vdrive_t *v)
{
    for (int i = 0; i < 2; i++) {
        uint8_t n = v->pin[i];
        if (!n) {
            n = i ? VD_DEV1 : VD_DEV0;
            while (n <= 30 && (n == v->pin[0] || n == v->pin[1] || (i == 1 && n == v->dev[0]))) n++;
            if (n > 30) n = 0;
        }
        v->dev[i] = n;
    }
}

/* "U0>"+CHR$(n): this drive answers as device n from now on (n = 0: automatic again). Persisted via the host. */
VD_COLD static void cmd_renumber(vdrive_t *v, int unit, vd_unit_t *u, uint8_t n)
{
    if (n && (n < 8 || n > 30 || n == v->dev[1 - unit])) { set_err(u, 33, 0, 0); return; }
    v->pin[unit] = n;
    settle(v);
    if (v->host.save_devices) v->host.save_devices(v->host.ctx, v->pin[0], v->pin[1]);
}

/* Up to max decimal numbers from p (any non-digit separates them: "5,0,18,0", " 5 0 18 0", ":5;0;18;0"); returns how many. */
VD_COLD static int parse_nums(const char *p, const char *end, int *v, int max)
{
    int n = 0;
    while (p < end && n < max) {
        if (*p < '0' || *p > '9') { p++; continue; }
        int x = 0;
        while (p < end && *p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); if (x > 999) x = 999; p++; }
        v[n++] = x;
    }
    return n;
}

/* U1/UA (B-R) and U2/UB (B-W): "U1:ch drive track sector" copies a whole sector to / from the buffer channel `ch`.
 * B-R / B-W are the same here: the sector is transferred whole and the buffer pointer starts at 0. */
VD_COLD static void cmd_block_io(vd_unit_t *u, const char *p, const char *end, int write)
{
    int a[4];
    if (parse_nums(p, end, a, 4) != 4) { set_err(u, 30, 0, 0); return; }
    if (a[0] < 0 || a[0] > 14 || a[0] == 15) { set_err(u, 70, 0, 0); return; }
    vd_chan_t *c = chan_find(u, a[0]);
    if (!c || c->mode != 5) { set_err(u, 70, 0, 0); return; }
    if (a[2] > 255 || a[3] > 255 || d64_sector_offset(u->disk, (uint8_t)a[2], (uint8_t)a[3]) < 0) { set_err(u, 66, (uint8_t)a[2], (uint8_t)a[3]); return; }
    uint8_t t = (uint8_t)a[2], s = (uint8_t)a[3];
    if (write) {
        int r = d64_block_write(u->disk, t, s, c->u.b.buf);
        if (r == D64_E_RO) set_err(u, 26, t, s);
        else if (r) set_err(u, 25, t, s);
    } else {
        if (d64_block_read(u->disk, t, s, c->u.b.buf)) set_err(u, 21, t, s);
        c->u.b.pos = 0;
    }
}

/* B-P:ch,pos moves the buffer pointer; B-A / B-F:drive,track,sector allocate / free a sector in the BAM. */
VD_COLD static void cmd_buffer(vd_unit_t *u, char op, const char *p, const char *end)
{
    int a[3];
    if (op == 'P') {
        if (parse_nums(p, end, a, 2) != 2) { set_err(u, 30, 0, 0); return; }
        vd_chan_t *c = a[0] <= 14 ? chan_find(u, a[0]) : NULL;
        if (!c || c->mode != 5) { set_err(u, 70, 0, 0); return; }
        c->u.b.pos = (uint16_t)(a[1] & 0xFF);
        return;
    }
    if (parse_nums(p, end, a, 3) != 3) { set_err(u, 30, 0, 0); return; }
    if (a[1] > 255 || a[2] > 255) { set_err(u, 66, (uint8_t)a[1], (uint8_t)a[2]); return; }
    uint8_t t = (uint8_t)a[1], s = (uint8_t)a[2], nt = 0, ns = 0;
    int r = op == 'A' ? d64_block_alloc(u->disk, t, s, &nt, &ns) : d64_block_free(u->disk, t, s);
    if (r == D64_E_IO) set_err(u, 66, t, s);
    else if (r == D64_E_EXISTS) set_err(u, 65, nt, ns);
    else if (r) err_from(u, r);
}

VD_COLD static void exec_command(vdrive_t *v, int unit, vd_unit_t *u, uint8_t *cmd, uint8_t len)
{
    /* OPEN 15,d,15,"U0>"+CHR$(n) sends 4 bytes; PRINT#15 adds a CR, making 5 */
    if ((len == 4 || (len == 5 && cmd[4] == '\r')) && cmd[0] == 'U' && cmd[1] == '0' && cmd[2] == '>') { cmd_renumber(v, unit, u, cmd[3]); return; }
    while (len && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n')) len--;
    if (!len) return;
    cmd[len] = 0;
    if (len >= 2 && cmd[0] == 'C' && cmd[1] == 'D') { cmd_cd(v, unit, u, (const char *)cmd + 2, (const char *)cmd + len); return; }
    if (cmd[0] == 'U' && len == 2 && (cmd[1] == 'I' || cmd[1] == 'J' || cmd[1] == '9')) { cmd_reset(u); return; }
    if (cmd[0] == 'U' && len == 3 && cmd[1] == 'I' && (cmd[2] == '+' || cmd[2] == '-')) return;   /* VIC-20 / C64 bus speed: nothing to do */
    if (!u->disk) { set_err(u, 74, 0, 0); return; }
    if (cmd[0] == 'U' && len >= 2 && (cmd[1] == '1' || cmd[1] == 'A' || cmd[1] == '2' || cmd[1] == 'B')) {
        cmd_block_io(u, (const char *)cmd + 2, (const char *)cmd + len, cmd[1] == '2' || cmd[1] == 'B'); return;
    }
    if (cmd[0] == 'B' && len >= 3 && cmd[1] == '-') {
        const char *q = (const char *)cmd + 3, *e = (const char *)cmd + len;
        switch (cmd[2]) {
        case 'R': case 'W': cmd_block_io(u, q, e, cmd[2] == 'W'); return;
        case 'P': case 'A': case 'F': cmd_buffer(u, (char)cmd[2], q, e); return;
        default: set_err(u, 31, 0, 0); return;
        }
    }
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
    for (int i = 0; i < 2; i++) { v->unit[i].owner = v; v->unit[i].index = (uint8_t)i; }
    settle(v);
}

VD_COLD void vdrive_attach(vdrive_t *v, int unit, d64_t *disk, int present)
{
    vd_unit_t *u = &v->unit[unit];
    close_all(u);
    u->disk = disk; u->present = (uint8_t)present;
    set_err(u, 0, 0, 0);
}

VD_COLD void vdrive_set_devices(vdrive_t *v, uint8_t pin0, uint8_t pin1)
{
    if (pin0 < 8 || pin0 > 30) pin0 = 0;                /* 0 (unset) or garbage: automatic */
    if (pin1 < 8 || pin1 > 30 || pin1 == pin0) pin1 = 0;
    v->pin[0] = pin0; v->pin[1] = pin1;
    settle(v);
}

VD_COLD int vdrive_scan_begin(vdrive_t *v)
{
    int need = 0;
    for (int i = 0; i < 2; i++)
        if (!v->pin[i]) { v->dev[i] = 0; need += v->unit[i].present != 0; }
    return need;
}

VD_COLD int vdrive_scan_free(vdrive_t *v, uint8_t dev)
{
    int left = 0;
    if (dev >= 8 && dev <= 30 && dev != v->pin[0] && dev != v->pin[1] && dev != v->dev[0] && dev != v->dev[1])
        for (int i = 0; i < 2; i++)
            if (!v->pin[i] && v->unit[i].present && !v->dev[i]) { v->dev[i] = dev; break; }
    for (int i = 0; i < 2; i++) left += !v->pin[i] && v->unit[i].present && !v->dev[i];
    return left;
}

VD_COLD int vdrive_unit_for(const vdrive_t *v, uint8_t dev) { return dev && dev == v->dev[0] ? 0 : dev && dev == v->dev[1] ? 1 : -1; }
VD_COLD void vdrive_set_host(vdrive_t *v, const vd_host_t *host) { v->host = *host; }
VD_COLD int vdrive_chan_mode(const vdrive_t *v, int unit, int ch)
{
    uint8_t i = v->unit[unit].slot_of[ch];
    return i ? v->unit[unit].slot[i - 1].mode : 0;
}
VD_COLD int vdrive_pending_err(const vdrive_t *v, int unit) { return v->unit[unit].err; }
VD_COLD int vdrive_active(const vdrive_t *v) { return v->act; }
VD_COLD size_t vdrive_sizeof(void) { return sizeof(vdrive_t); }

VD_COLD static int read_byte(vdrive_t *v, int unit, vd_unit_t *u, vd_chan_t *c, uint8_t *b, int *eoi)
{
    if (c->mode == 1) {
        int n = d64_file_read(u->disk, &c->u.r, b, 1);
        if (n <= 0) return 0;
        *eoi = d64_file_eof(&c->u.r);
        return 1;
    }
    if (c->mode == 5) {
        if (c->u.b.pos >= D64_SECTOR) return 0;
        *b = c->u.b.buf[c->u.b.pos++];
        *eoi = c->u.b.pos >= D64_SECTOR;
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
            else if (c && c->mode == 5 && c->u.b.pos < D64_SECTOR) c->u.b.buf[c->u.b.pos++] = arg;     /* buffer: full buffer ignores the rest */
        } else if (v->wlen < VD_CMDBUF - 1) {
            v->wbuf[v->wlen++] = arg;
        }
        return 0;
    case VD_UNLSN:
        if (op == 0xF0 || op == 0xE0 || (op == 0x60 && ch == 15))
            u->stat_len = u->stat_pos = 0;             /* a new DOS operation replaces the status message: whatever was left
                                                          unread is dropped (as on a real drive / in VICE), so callers need
                                                          not drain it. Reading it in pieces between operations still works. */
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
