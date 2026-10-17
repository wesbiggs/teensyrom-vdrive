#include "d64.h"
#include "vd_cfg.h"
#include <string.h>

VD_COLD static int spt(uint8_t t)
{
    if (t >= 1 && t <= 17)  return 21;
    if (t >= 18 && t <= 24) return 19;
    if (t >= 25 && t <= 30) return 18;
    if (t >= 31 && t <= 40) return 17;
    return 0;
}
VD_COLD int d64_sectors_in_track(uint8_t t) { return spt(t); }

VD_COLD long d64_sector_offset(const d64_t *d, uint8_t t, uint8_t s)
{
    if (t < 1 || t > d->tracks || s >= spt(t)) return -1;
    long off = 0;
    for (uint8_t i = 1; i < t; i++) off += spt(i);
    return (off + s) * D64_SECTOR;
}

VD_COLD static int mem_write(void *ctx, uint8_t t, uint8_t s, const uint8_t *buf)
{
    d64_mem_t *m = (d64_mem_t *)ctx;
    uint8_t tracks = (m->len >= 196608) ? 40 : 35;
    d64_t tmp = { 0, 0, 0, tracks };
    long off = d64_sector_offset(&tmp, t, s);
    if (off < 0 || (size_t)off + D64_SECTOR > m->len) return -1;
    memcpy(m->data + off, buf, D64_SECTOR);
    return 0;
}

VD_COLD static int mem_read(void *ctx, uint8_t t, uint8_t s, uint8_t *buf)
{
    d64_mem_t *m = (d64_mem_t *)ctx;
    uint8_t tracks = (m->len >= 196608) ? 40 : 35;
    d64_t tmp = { 0, 0, 0, tracks };
    long off = d64_sector_offset(&tmp, t, s);
    if (off < 0 || (size_t)off + D64_SECTOR > m->len) return -1;
    memcpy(buf, m->data + off, D64_SECTOR);
    return 0;
}
VD_COLD void d64_mem_init(d64_mem_t *m, uint8_t *data, size_t len) { m->data = data; m->len = len; }
VD_COLD int d64_open(d64_t *d, d64_read_fn rd, void *ctx, uint8_t tracks)
{
    d->read = rd; d->write = NULL; d->ctx = ctx; d->tracks = (tracks == 40) ? 40 : 35;
    return 0;
}
VD_COLD int d64_open_mem(d64_t *d, d64_mem_t *m)
{
    if (m->len < 174848) return -1;
    int r = d64_open(d, mem_read, m, m->len >= 196608 ? 40 : 35);
    d->write = mem_write;
    return r;
}

VD_COLD static int rd(d64_t *d, uint8_t t, uint8_t s, uint8_t *b)
{
    if (d64_sector_offset(d, t, s) < 0) return D64_E_IO;
    return d->read(d->ctx, t, s, b) ? D64_E_IO : 0;
}
VD_COLD static int wr(d64_t *d, uint8_t t, uint8_t s, const uint8_t *b)
{
    if (!d->write) return D64_E_RO;
    if (d64_sector_offset(d, t, s) < 0) return D64_E_IO;
    return d->write(d->ctx, t, s, b) ? D64_E_IO : 0;
}

VD_COLD static void strip(char *dst, const uint8_t *src, int n)
{
    int len = n;
    while (len > 0 && src[len - 1] == 0xA0) len--;
    memcpy(dst, src, len);
    dst[len] = 0;
}

VD_COLD int d64_header(d64_t *d, char name[17], char id[3], char dos[3])
{
    uint8_t b[D64_SECTOR];
    if (rd(d, 18, 0, b)) return -1;
    strip(name, b + 0x90, 16);
    id[0] = b[0xA2]; id[1] = b[0xA3]; id[2] = 0;
    dos[0] = b[0xA5]; dos[1] = b[0xA6]; dos[2] = 0;
    return 0;
}

VD_COLD int d64_blocks_free(d64_t *d)
{
    uint8_t b[D64_SECTOR];
    if (rd(d, 18, 0, b)) return -1;
    int free_ = 0;
    for (int t = 1; t <= 35; t++) if (t != 18) free_ += b[4 * t];
    return free_;
}

VD_COLD int d64_dir_first(d64_t *d, d64_dir_t *it)
{
    uint8_t b[D64_SECTOR];
    if (rd(d, 18, 0, b)) return -1;
    it->track = b[0] ? b[0] : 18;
    it->sector = b[0] ? b[1] : 1;
    it->slot = 8;               /* force load on first next() */
    it->guard = 0;
    return 0;
}

VD_COLD int d64_dir_next(d64_t *d, d64_dir_t *it, d64_entry_t *e)
{
    for (;;) {
        if (it->slot >= 8) {
            if (it->track == 0) return 0;
            if (++it->guard > 40) return -2;               /* loop in chain */
            if (rd(d, it->track, it->sector, it->buf)) return -1;
            it->cur_t = it->track; it->cur_s = it->sector;
            it->track = it->buf[0]; it->sector = it->buf[1];   /* next sector in chain */
            it->slot = 0;
        }
        const uint8_t *p = it->buf + it->slot * 32;
        it->slot++;
        if (p[2] == 0) continue;                           /* scratched / empty */
        e->type = p[2];
        e->track = p[3]; e->sector = p[4];
        e->blocks = p[30] | (p[31] << 8);
        strip(e->name, p + 5, 16);
        e->dir_t = it->cur_t; e->dir_s = it->cur_s; e->dir_slot = (uint8_t)(it->slot - 1);
        return 1;
    }
}

VD_COLD int d64_match(const char *name, const char *pat)
{
    for (;; name++, pat++) {
        if (*pat == '*') return 1;
        if (*pat == 0) return *name == 0;
        if (*name == 0) return 0;
        if (*pat != '?' && *pat != *name) return 0;
    }
}

VD_COLD int d64_find(d64_t *d, const char *pat, d64_entry_t *e)
{
    d64_dir_t it;
    int r = d64_dir_first(d, &it);
    if (r) return r;
    while ((r = d64_dir_next(d, &it, e)) > 0)
        if ((e->type & 7) != 0 || (e->type & 0x80)) if (d64_match(e->name, pat)) return 1;
    return r;
}

VD_COLD int d64_file_open(d64_t *d, const d64_entry_t *e, d64_file_t *f)
{
    (void)d;
    f->next_t = e->track; f->next_s = e->sector;
    f->loaded = 0; f->pos = f->end = 0; f->guard = 0;
    return 0;
}

VD_COLD int d64_file_eof(const d64_file_t *f)
{
    return f->pos >= f->end && f->next_t == 0;
}

VD_COLD int d64_file_read(d64_t *d, d64_file_t *f, uint8_t *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (f->pos >= f->end) {
            if (f->next_t == 0) break;                       /* EOF (or empty file) */
            if (++f->guard > 800) return -2;                 /* chain loop */
            if (rd(d, f->next_t, f->next_s, f->buf)) return -1;
            uint8_t lt = f->buf[0], ls = f->buf[1];
            f->pos = 2;
            f->end = lt ? 256 : (uint16_t)(ls < 1 ? 2 : ls + 1);   /* last sector: ls = index of last used byte */
            f->next_t = lt; f->next_s = ls;
            f->loaded = 1;
            continue;
        }
        size_t c = (size_t)(f->end - f->pos);
        if (c > n - got) c = n - got;
        memcpy(out + got, f->buf + f->pos, c);
        f->pos = (uint16_t)(f->pos + c); got += c;
    }
    return (int)got;
}

/* ---- write support ------------------------------------------------------ */

VD_COLD static int next_data_track(uint8_t t)
{   /* allocation order: 17..1 then 19..35 (nearest the directory first); 0 = exhausted */
    if (t == 0) return 17;
    if (t >= 2 && t <= 17) return t - 1;
    if (t == 1) return 19;
    if (t >= 19 && t <= 34) return t + 1;
    return 0;
}

/* Allocate one sector. In: *t,*s = hint (t==0: none). dirmode: track 18 only, never sector 0. */
VD_COLD static int alloc_sector(d64_t *d, uint8_t *t, uint8_t *s, int interleave, int dirmode)
{
    uint8_t bam[D64_SECTOR];
    int r = rd(d, 18, 0, bam);
    if (r) return r;
    uint8_t tr; int hint;
    if (dirmode)   { tr = 18; hint = (*t == 18) ? *s + interleave : 1; }
    else if (*t)   { tr = *t; hint = *s + interleave; }
    else           { tr = next_data_track(0); hint = 0; }
    for (;;) {
        if (tr == 0 || tr > 35) return D64_E_FULL;
        if ((tr != 18 || dirmode) && bam[4 * tr] > 0) {
            int n = spt(tr);
            for (int i = 0; i < n; i++) {
                int sec = (hint + i) % n;
                if (dirmode && sec == 0) continue;
                if (bam[4 * tr + 1 + sec / 8] & (1 << (sec % 8))) {
                    bam[4 * tr + 1 + sec / 8] &= (uint8_t)~(1 << (sec % 8));
                    bam[4 * tr]--;
                    r = wr(d, 18, 0, bam);
                    if (r) return r;
                    *t = tr; *s = (uint8_t)sec;
                    return 0;
                }
            }
        }
        if (dirmode) return D64_E_FULL;
        tr = (uint8_t)next_data_track(tr);
        hint = 0;
    }
}

VD_COLD static int free_sector(d64_t *d, uint8_t t, uint8_t s)
{
    uint8_t bam[D64_SECTOR];
    if (t < 1 || t > 35 || s >= spt(t)) return D64_E_IO;
    int r = rd(d, 18, 0, bam);
    if (r) return r;
    uint8_t bit = (uint8_t)(1 << (s % 8)), *p = &bam[4 * t + 1 + s / 8];
    if (!(*p & bit)) { *p |= bit; bam[4 * t]++; }
    return wr(d, 18, 0, bam);
}

VD_COLD static int valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 1 || l > 16) return 0;
    for (; *n; n++) if (*n == '*' || *n == '?' || *n == ',' || *n == ':' || *n == '=' || *n == '"') return 0;
    return 1;
}

/* Update directory entry bytes in place. */
VD_COLD static int dir_update(d64_t *d, uint8_t dt, uint8_t ds, uint8_t slot, const uint8_t *entry30 /* NULL: leave */,
                      int type, int blocks)
{
    uint8_t b[D64_SECTOR];
    int r = rd(d, dt, ds, b);
    if (r) return r;
    uint8_t *p = b + slot * 32;
    if (entry30) memcpy(p + 2, entry30, 30);
    if (type >= 0) p[2] = (uint8_t)type;
    if (blocks >= 0) { p[30] = (uint8_t)blocks; p[31] = (uint8_t)(blocks >> 8); }
    return wr(d, dt, ds, b);
}

VD_COLD int d64_scratch_entry(d64_t *d, const d64_entry_t *e)
{
    if (!d->write) return D64_E_RO;
    if (e->type & 0x40) return D64_E_LOCKED;
    uint8_t t = e->track, s = e->sector, b[D64_SECTOR];
    for (int guard = 0; t && guard < 800; guard++) {
        int r = rd(d, t, s, b);
        if (r) break;                       /* broken chain: free what we can */
        uint8_t nt = b[0], ns = b[1];
        if ((r = free_sector(d, t, s))) return r;
        t = nt; s = ns;
    }
    return dir_update(d, e->dir_t, e->dir_s, e->dir_slot, NULL, 0, -1);
}

VD_COLD int d64_scratch(d64_t *d, const char *pat)
{
    d64_dir_t it; d64_entry_t e;
    int r = d64_dir_first(d, &it), n = 0;
    if (r) return r;
    while ((r = d64_dir_next(d, &it, &e)) > 0) {
        if (!d64_match(e.name, pat)) continue;
        r = d64_scratch_entry(d, &e);
        if (r == D64_E_LOCKED) continue;
        if (r) return r;
        n++;
    }
    return r < 0 ? r : n;
}

VD_COLD int d64_rename(d64_t *d, const char *oldn, const char *newn)
{
    d64_entry_t e, x;
    if (!valid_name(newn)) return D64_E_NAME;
    if (!d->write) return D64_E_RO;
    int r = d64_find(d, oldn, &e);
    if (r < 0) return r;
    if (r == 0) return D64_E_NOTFOUND;
    if ((r = d64_find(d, newn, &x)) < 0) return r;
    if (r == 1) return D64_E_EXISTS;
    uint8_t b[D64_SECTOR];
    if ((r = rd(d, e.dir_t, e.dir_s, b))) return r;
    uint8_t *p = b + e.dir_slot * 32 + 5;
    size_t l = strlen(newn);
    memset(p, 0xA0, 16); memcpy(p, newn, l);
    return wr(d, e.dir_t, e.dir_s, b);
}

/* Find a free directory slot, extending the directory if necessary. */
VD_COLD static int dir_slot_alloc(d64_t *d, uint8_t *dt, uint8_t *ds, uint8_t *slot)
{
    uint8_t b[D64_SECTOR], t, s;
    int r = rd(d, 18, 0, b);
    if (r) return r;
    t = b[0] ? b[0] : 18; s = b[0] ? b[1] : 1;
    for (int guard = 0; guard < 40; guard++) {
        if ((r = rd(d, t, s, b))) return r;
        for (int i = 0; i < 8; i++)
            if (b[i * 32 + 2] == 0) { *dt = t; *ds = s; *slot = (uint8_t)i; return 0; }
        if (b[0] == 0) {                       /* last dir sector, full: extend */
            uint8_t nt = t, ns = s;
            if ((r = alloc_sector(d, &nt, &ns, 3, 1))) return r;
            b[0] = nt; b[1] = ns;
            if ((r = wr(d, t, s, b))) return r;
            memset(b, 0, sizeof b); b[1] = 0xFF;
            if ((r = wr(d, nt, ns, b))) return r;
            *dt = nt; *ds = ns; *slot = 0;
            return 0;
        }
        t = b[0]; s = b[1];
    }
    return D64_E_LOOP;
}

VD_COLD int d64_create(d64_t *d, const char *name, uint8_t type, int replace, d64_wfile_t *w)
{
    d64_entry_t e;
    if (!d->write) return D64_E_RO;
    if (!valid_name(name)) return D64_E_NAME;
    int r = d64_find(d, name, &e);
    if (r < 0) return r;
    if (r == 1) {
        if (!replace) return D64_E_EXISTS;
        if ((r = d64_scratch_entry(d, &e))) return r;
    }
    uint8_t ft = 0, fs = 0, dt, ds, slot;
    if ((r = alloc_sector(d, &ft, &fs, 10, 0))) return r;
    if ((r = dir_slot_alloc(d, &dt, &ds, &slot))) { free_sector(d, ft, fs); return r; }

    uint8_t ent[30];
    memset(ent, 0, sizeof ent);
    ent[0] = type & 7;                         /* unclosed until close */
    ent[1] = ft; ent[2] = fs;
    memset(ent + 3, 0xA0, 16); memcpy(ent + 3, name, strlen(name));
    if ((r = dir_update(d, dt, ds, slot, ent, -1, -1))) { free_sector(d, ft, fs); return r; }

    w->type = type & 7; w->dir_t = dt; w->dir_s = ds; w->dir_slot = slot;
    w->first_t = w->cur_t = ft; w->first_s = w->cur_s = fs;
    w->pos = 2; w->blocks = 1;
    memset(w->buf, 0, sizeof w->buf);
    return 0;
}

VD_COLD static void abort_write(d64_t *d, d64_wfile_t *w)
{   /* free exactly the sectors we allocated, then drop the directory entry */
    uint8_t t = w->first_t, s = w->first_s, b[D64_SECTOR];
    for (int i = 0; i < w->blocks; i++) {
        uint8_t nt = 0, ns = 0;
        if (i + 1 < w->blocks && rd(d, t, s, b) == 0) { nt = b[0]; ns = b[1]; }
        free_sector(d, t, s);
        if (!nt) break;
        t = nt; s = ns;
    }
    dir_update(d, w->dir_t, w->dir_s, w->dir_slot, NULL, 0, -1);
}

VD_COLD int d64_write(d64_t *d, d64_wfile_t *w, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (w->pos == D64_SECTOR) {
            uint8_t nt = w->cur_t, ns = w->cur_s;
            int r = alloc_sector(d, &nt, &ns, 10, 0);
            if (r == 0) {
                w->buf[0] = nt; w->buf[1] = ns;
                r = wr(d, w->cur_t, w->cur_s, w->buf);
                if (r) free_sector(d, nt, ns);       /* don't leak the sector we just took */
            }
            if (r) { abort_write(d, w); return r; }
            w->cur_t = nt; w->cur_s = ns; w->blocks++;
            memset(w->buf, 0, sizeof w->buf);
            w->pos = 2;
        }
        w->buf[w->pos++] = data[i];
    }
    return (int)n;
}

VD_COLD int d64_close_write(d64_t *d, d64_wfile_t *w)
{
    w->buf[0] = 0; w->buf[1] = (uint8_t)(w->pos - 1);   /* index of last used byte */
    int r = wr(d, w->cur_t, w->cur_s, w->buf);
    if (r) return r;
    return dir_update(d, w->dir_t, w->dir_s, w->dir_slot, NULL, 0x80 | w->type, w->blocks);
}

VD_COLD int d64_format(d64_t *d, const char *name, const char *id)
{
    uint8_t b[D64_SECTOR];
    uint8_t idb[2];
    if (!d->write) return D64_E_RO;
    if (!valid_name(name)) return D64_E_NAME;
    if (id) {
        if (strlen(id) != 2) return D64_E_NAME;
        idb[0] = (uint8_t)id[0]; idb[1] = (uint8_t)id[1];
        memset(b, 0, sizeof b);
        for (uint8_t t = 1; t <= d->tracks; t++)
            for (int sct = 0; sct < spt(t); sct++)
                if ((t != 18 || sct > 1)) { int r = wr(d, t, (uint8_t)sct, b); if (r) return r; }
    } else {
        int r = rd(d, 18, 0, b);
        if (r) return r;
        idb[0] = b[0xA2]; idb[1] = b[0xA3];
    }
    memset(b, 0, sizeof b);
    b[0] = 18; b[1] = 1; b[2] = 'A';
    for (int t = 1; t <= 35; t++) {
        int n = spt(t);
        uint8_t *p = &b[4 * t];
        p[0] = (uint8_t)n;
        for (int sct = 0; sct < n; sct++) p[1 + sct / 8] |= (uint8_t)(1 << (sct % 8));
    }
    b[4 * 18] -= 2; b[4 * 18 + 1] &= 0xFC;                  /* 18/0 and 18/1 in use */
    memset(b + 0x90, 0xA0, 16); memcpy(b + 0x90, name, strlen(name));
    b[0xA0] = b[0xA1] = 0xA0;
    b[0xA2] = idb[0]; b[0xA3] = idb[1];
    b[0xA4] = 0xA0; b[0xA5] = '2'; b[0xA6] = 'A';
    b[0xA7] = b[0xA8] = b[0xA9] = b[0xAA] = 0xA0;
    int r = wr(d, 18, 0, b);
    if (r) return r;
    memset(b, 0, sizeof b); b[1] = 0xFF;
    return wr(d, 18, 1, b);
}

/* ---- BASIC listing ------------------------------------------------------ */
VD_COLD static void put(d64_bas_t *b, uint8_t c)
{
    if (b->len >= b->cap) { b->ok = 0; return; }
    b->p[b->len++] = c;
}
VD_COLD static void line_begin(d64_bas_t *b, uint16_t num)
{
    b->link_at = (uint16_t)b->len;
    put(b, 0); put(b, 0);                              /* link patched in line_end */
    put(b, num & 0xFF); put(b, num >> 8);
}
VD_COLD static void line_end(d64_bas_t *b)
{
    put(b, 0);
    uint16_t next = (uint16_t)(0x0401 + b->base + b->len - 2);   /* -2: load address bytes come first */
    if (b->ok) { b->p[b->link_at] = next & 0xFF; b->p[b->link_at + 1] = next >> 8; }
}
VD_COLD static void puts_(d64_bas_t *b, const char *s) { while (*s) put(b, (uint8_t)*s++); }

VD_COLD void d64_bas_begin_at(d64_bas_t *b, uint8_t *out, size_t cap, size_t base)
{
    b->p = out; b->cap = cap; b->len = 0; b->base = base; b->ok = 1; b->link_at = 0;
    if (base == 0) { put(b, 0x01); put(b, 0x04); }      /* load address $0401 */
}

VD_COLD void d64_bas_begin(d64_bas_t *b, uint8_t *out, size_t cap) { d64_bas_begin_at(b, out, cap, 0); }

VD_COLD void d64_bas_header(d64_bas_t *b, const char *name, const char *id, const char *dos)
{
    line_begin(b, 0);
    put(b, 0x12); put(b, '"');                         /* reverse video */
    int n = (int)strlen(name);
    for (int i = 0; i < 16; i++) put(b, i < n ? (uint8_t)name[i] : 0x20);
    put(b, '"'); put(b, ' '); puts_(b, id); put(b, ' '); puts_(b, dos);
    line_end(b);
}

VD_COLD void d64_bas_entry(d64_bas_t *b, uint16_t blocks, const char *name, const char *type3, int closed, int locked)
{
    line_begin(b, blocks);
    int pad = blocks < 10 ? 3 : blocks < 100 ? 2 : blocks < 1000 ? 1 : 0;
    for (int i = 0; i < pad; i++) put(b, ' ');
    put(b, '"');
    int l = (int)strlen(name);
    for (int i = 0; i < l; i++) put(b, (uint8_t)name[i]);
    put(b, '"');
    for (int i = l; i < 16; i++) put(b, ' ');
    put(b, closed ? ' ' : '*');
    puts_(b, type3);
    put(b, locked ? '<' : ' ');
    line_end(b);
}

VD_COLD void d64_bas_footer(d64_bas_t *b, uint16_t free_blocks)
{
    line_begin(b, free_blocks);
    puts_(b, "BLOCKS FREE.             ");
    line_end(b);
}

VD_COLD size_t d64_bas_end(d64_bas_t *b)
{
    put(b, 0); put(b, 0);                              /* end of program */
    return b->ok ? b->len : 0;
}

static const char *const TYPES[8] = { "DEL", "SEQ", "PRG", "USR", "REL", "???", "???", "???" };

VD_COLD size_t d64_build_dir(d64_t *d, const char *pattern, uint8_t *out, size_t cap)
{
    d64_bas_t b;
    char name[17], id[3], dos[3];
    d64_bas_begin(&b, out, cap);
    if (d64_header(d, name, id, dos)) return 0;
    d64_bas_header(&b, name, id, dos);

    d64_dir_t it; d64_entry_t e;
    if (d64_dir_first(d, &it)) return 0;
    int r;
    while ((r = d64_dir_next(d, &it, &e)) > 0) {
        if (pattern && !d64_match(e.name, pattern)) continue;
        d64_bas_entry(&b, e.blocks, e.name, TYPES[e.type & 7], (e.type & 0x80) != 0, (e.type & 0x40) != 0);
    }
    if (r < 0) return 0;

    int fr = d64_blocks_free(d);
    if (fr < 0) return 0;
    d64_bas_footer(&b, (uint16_t)fr);
    return d64_bas_end(&b);
}

VD_COLD size_t d64_sizeof_d64(void)   { return sizeof(d64_t); }
VD_COLD size_t d64_sizeof_mem(void)   { return sizeof(d64_mem_t); }
VD_COLD size_t d64_sizeof_dir(void)   { return sizeof(d64_dir_t); }
VD_COLD size_t d64_sizeof_wfile(void) { return sizeof(d64_wfile_t); }
VD_COLD size_t d64_sizeof_file(void)  { return sizeof(d64_file_t); }
VD_COLD size_t d64_sizeof_entry(void) { return sizeof(d64_entry_t); }
