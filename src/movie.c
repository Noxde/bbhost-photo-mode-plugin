/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbphoto: the photo panel's sections, added to the options movie at start.
 *
 * bbhost generates optionsetting.gfx with its PC sections (clones of ControllSetting placed on the
 * main timeline, each linked to the donor's class). The plugin reads that movie (game_file: the
 * player's mods and bbhost's generated files, not the plugins' overlays), clones three of those
 * sections again under its own instance names - PhotoA, PhotoB, PhotoC - and hands the result to the
 * game through its own overlay. Nothing of bbhost's sections changes. Included by bbphoto.c.
 * The clones are made smaller toward the stage's bottom right corner by changing the matrix of every
 * placement inside them, in every frame: the game sets the section's own placement itself, so a
 * matrix there was not kept (0.3). */

typedef struct { uint8_t* p; size_t n, cap; } Buf;

static int buf_put(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1 << 20;
        while (c < b->n + n) c *= 2;
        uint8_t* q = (uint8_t*)realloc(b->p, c);
        if (!q) return 0;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return 1;
}
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct { int code; size_t head, s, e; } Tag;   /* body from s to e */

/* the tags from i to end, up to max; their count (-1: malformed) */
static int tags_in(const uint8_t* b, size_t size, size_t i, size_t end, Tag* out, int max) {
    int n = 0;
    while (i + 2 <= end && n < max) {
        const uint16_t th = le16(b + i);
        Tag t;
        t.head = i;
        t.code = th >> 6;
        size_t len = th & 0x3f;
        i += 2;
        if (len == 0x3f) {
            if (i + 4 > end) return -1;
            len = le32(b + i);
            i += 4;
        }
        if (i + len > size) return -1;
        t.s = i;
        t.e = i + len;
        out[n++] = t;
        if (t.code == 0) break;
        i += len;
    }
    return n;
}
static int put_tag(Buf* o, int code, const uint8_t* body, size_t n) {
    uint8_t h[6];
    const uint16_t th = (uint16_t)(code << 6 | 0x3f);
    h[0] = (uint8_t)th; h[1] = (uint8_t)(th >> 8);
    h[2] = (uint8_t)n; h[3] = (uint8_t)(n >> 8); h[4] = (uint8_t)(n >> 16); h[5] = (uint8_t)(n >> 24);
    return buf_put(o, h, 6) && buf_put(o, body, n);
}

/* bits, most significant first */
typedef struct { const uint8_t* p; size_t bit; } BitR;
static uint32_t ub(BitR* r, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; k++, r->bit++) v = v << 1 | ((r->p[r->bit >> 3] >> (7 - (r->bit & 7))) & 1);
    return v;
}
static int32_t sb(BitR* r, int n) {
    if (!n) return 0;
    uint32_t v = ub(r, n);
    if (v & (1u << (n - 1))) v |= ~0u << n;
    return (int32_t)v;
}
typedef struct { uint8_t* p; size_t bit; } BitW;
static void wb(BitW* w, uint32_t v, int n) {
    for (int k = n - 1; k >= 0; k--, w->bit++) {
        if (!(w->bit & 7)) w->p[w->bit >> 3] = 0;
        if ((v >> k) & 1) w->p[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
    }
}
static int bits_for(int32_t v) {   /* signed */
    int n = 1;
    while (n < 31 && (v < -(1 << (n - 1)) || v > (1 << (n - 1)) - 1)) n++;
    return n;
}

typedef struct { int has_scale, has_rot; int32_t sx, sy, r0, r1, tx, ty; } Matrix;   /* 16.16, twips */
static size_t matrix_read(const uint8_t* p, Matrix* m) {
    BitR r = {p, 0};
    memset(m, 0, sizeof *m);
    m->sx = m->sy = 1 << 16;
    if ((m->has_scale = (int)ub(&r, 1))) { const int n = (int)ub(&r, 5); m->sx = sb(&r, n); m->sy = sb(&r, n); }
    if ((m->has_rot = (int)ub(&r, 1))) { const int n = (int)ub(&r, 5); m->r0 = sb(&r, n); m->r1 = sb(&r, n); }
    const int n = (int)ub(&r, 5);
    m->tx = sb(&r, n);
    m->ty = sb(&r, n);
    return (r.bit + 7) / 8;
}
static size_t matrix_write(uint8_t* p, const Matrix* m) {
    BitW w = {p, 0};
    wb(&w, 1, 1);
    int n = bits_for(m->sx) > bits_for(m->sy) ? bits_for(m->sx) : bits_for(m->sy);
    wb(&w, (uint32_t)n, 5); wb(&w, (uint32_t)m->sx, n); wb(&w, (uint32_t)m->sy, n);
    wb(&w, (uint32_t)m->has_rot, 1);
    if (m->has_rot) {
        n = bits_for(m->r0) > bits_for(m->r1) ? bits_for(m->r0) : bits_for(m->r1);
        wb(&w, (uint32_t)n, 5); wb(&w, (uint32_t)m->r0, n); wb(&w, (uint32_t)m->r1, n);
    }
    n = bits_for(m->tx) > bits_for(m->ty) ? bits_for(m->tx) : bits_for(m->ty);
    wb(&w, (uint32_t)n, 5); wb(&w, (uint32_t)m->tx, n); wb(&w, (uint32_t)m->ty, n);
    if (w.bit & 7) wb(&w, 0, 8 - (int)(w.bit & 7));
    return w.bit / 8;
}

/* A PlaceObject2 body's parts: flags, depth, character, the matrix, and the rest (colour transform,
 * ratio, name, ...) kept as it is up to the name, which is replaced. */
typedef struct {
    int code;                        /* 26 PlaceObject2, 70 PlaceObject3 */
    uint8_t flags, flags2;
    size_t cls_s, cls_e;             /* PlaceObject3's class name, raw */
    uint16_t depth, cid;
    Matrix m;
    size_t pre_name_s, pre_name_e;   /* the colour transform and ratio, raw */
    const char* name;
    size_t post_name_s, post_name_e; /* clip depth and actions, raw */
} Place2;

static size_t cxform_len(const uint8_t* p) {
    BitR r = {p, 0};
    const int add = (int)ub(&r, 1), mul = (int)ub(&r, 1), n = (int)ub(&r, 4);
    r.bit += (size_t)((add ? 4 : 0) + (mul ? 4 : 0)) * n;
    return (r.bit + 7) / 8;
}
static int place2_parse(const uint8_t* b, int code, size_t s, size_t e, Place2* o) {
    size_t i = s;
    memset(o, 0, sizeof *o);
    if (e - s < 4) return 0;
    o->code = code;
    o->flags = b[i++];
    if (code == 70) o->flags2 = b[i++];
    o->depth = le16(b + i); i += 2;
    o->cls_s = o->cls_e = i;
    if (code == 70 && ((o->flags2 & 0x08) || ((o->flags2 & 0x10) && (o->flags & 0x02)))) {
        while (i < e && b[i]) i++;
        i++;
        o->cls_e = i;
    }
    if (o->flags & 0x02) { o->cid = le16(b + i); i += 2; }
    if (o->flags & 0x04) i += matrix_read(b + i, &o->m);
    else { o->m.sx = o->m.sy = 1 << 16; }
    o->pre_name_s = i;
    if (o->flags & 0x08) i += cxform_len(b + i);
    if (o->flags & 0x10) i += 2;
    o->pre_name_e = i;
    if (o->flags & 0x20) {
        o->name = (const char*)b + i;
        while (i < e && b[i]) i++;
        i++;
    }
    o->post_name_s = i;
    o->post_name_e = e;
    return i <= e;
}

static int is_define(int code) {
    switch (code) {
    case 2: case 6: case 7: case 10: case 11: case 13: case 14: case 20: case 21: case 22: case 32: case 33: case 34:
    case 35: case 36: case 37: case 39: case 46: case 48: case 60: case 62: case 75: case 83: case 84: case 87: case 90:
    case 1000: case 1001: case 1003: case 1004: case 1007: case 1008: case 1009:
        return 1;
    }
    return 0;
}

/* scale and move one placement's matrix: child' = child * s + off (twips) */
static void matrix_xform(Matrix* m, float s, int32_t ox, int32_t oy) {
    m->has_scale = 1;
    m->sx = (int32_t)(m->sx * s); m->sy = (int32_t)(m->sy * s);
    m->r0 = (int32_t)(m->r0 * s); m->r1 = (int32_t)(m->r1 * s);
    m->tx = (int32_t)(m->tx * s) + ox; m->ty = (int32_t)(m->ty * s) + oy;
}
/* The section's full-screen dimmer (ControllSetting's depth-1 child: sprite 19, one shape scaled over
 * the stage), which the panel leaves out. */
#define DIMMER_CID 19
static uint16_t g_frame_cid = 135;   /* the section's frame: its depth-3 child (see frame_short) */
#define FRAME_CID g_frame_cid
/* The section's own key guide and line help (ControllSetting's "StatusBar", sprite 37): the bar along
 * the bottom shows the keys, so the panel's is moved out of the screen (not removed: its script may
 * use it). */
#define STATUSBAR_CID 37
#define OFFSCREEN (4000 * 20)
/* The first row (Camera's Mode) above the others: its pick list opens down over the rows under it, and
 * each row's list is inside the row (Widgets.DropdownList), so the rows placed above it hid it. */
#define RAISE_ROW "Item_0_0"
/* a placement or removal tag copied with another depth (the depth right after the flags) */
static void put_tag_depth(Buf* o, const uint8_t* b, const Tag* t, uint16_t depth) {
    const size_t at = t->code == 28 ? 0 : t->code == 70 ? 2 : 1;
    uint8_t* body = (uint8_t*)malloc(t->e - t->s + 1);
    if (!body) return;
    memcpy(body, b + t->s, t->e - t->s);
    body[at] = (uint8_t)depth;
    body[at + 1] = (uint8_t)(depth >> 8);
    put_tag(o, t->code, body, t->e - t->s);
    free(body);
}
/* A DefineSprite body (id, frame count, tags) with every PlaceObject2/3 that sets a matrix, or places a
 * character, given the transform; the dimmer's placements (and later changes at its depth) dropped. */
static int sprite_xform(const uint8_t* b, size_t s, size_t e, float sc, int32_t ox, int32_t oy, const Buf* extra,
                        uint16_t frame_to, Buf* out) {
    static Tag t[4096];
    const int n = tags_in(b, e, s + 4, e, t, 4096);
    int dim_depth = -1, extra_done = 0, bar_depth = -1, raise_from = -1, top = 0;
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        Place2 p;
        if ((t[i].code != 26 && t[i].code != 70) || !place2_parse(b, t[i].code, t[i].s, t[i].e, &p)) continue;
        if (p.depth > top) top = p.depth;
        if (p.name && !strcmp(p.name, RAISE_ROW)) raise_from = p.depth;
    }
    const uint16_t raise_to = (uint16_t)(top + 1);
    if (raise_to >= 3000) raise_from = -1;   /* our tabs and key guide start there */
    buf_put(out, b + s, 4);
    for (int i = 0; i < n; i++) {
        const int code = t[i].code;
        if ((code == 1 || code == 0) && !extra_done) {   /* our placements, in the first frame */
            if (extra && extra->n) buf_put(out, extra->p, extra->n);
            extra_done = 1;
        }
        if (code == 28 && t[i].e - t[i].s >= 2 && le16(b + t[i].s) == dim_depth) continue;   /* RemoveObject2 */
        if (code == 28 && t[i].e - t[i].s >= 2 && le16(b + t[i].s) == raise_from) { put_tag_depth(out, b, &t[i], raise_to); continue; }
        if (code != 26 && code != 70) { buf_put(out, b + t[i].head, t[i].e - t[i].head); continue; }
        size_t j = t[i].s;
        const uint8_t f1 = b[j++];
        const uint8_t f2 = code == 70 ? b[j++] : 0;
        const uint16_t depth = le16(b + j);
        if (depth == dim_depth && !(f1 & 0x02)) continue;
        if (!(f1 & 0x04) && !(f1 & 0x02)) {
            if (depth == raise_from) put_tag_depth(out, b, &t[i], raise_to);
            else buf_put(out, b + t[i].head, t[i].e - t[i].head);
            continue;
        }
        j += 2;                                                        /* depth */
        if (code == 70 && ((f2 & 0x08) || ((f2 & 0x10) && (f1 & 0x02)))) { while (j < t[i].e && b[j]) j++; j++; }
        if (f1 & 0x02) {                                               /* character */
            const uint16_t cid = le16(b + j);
            if (cid == DIMMER_CID) { dim_depth = depth; continue; }
            if (depth == dim_depth) dim_depth = -1;
            if (cid == STATUSBAR_CID) bar_depth = depth;
            else if (depth == bar_depth) bar_depth = -1;
            j += 2;
        }
        Matrix m;
        size_t ml = 0;
        if (f1 & 0x04) ml = matrix_read(b + j, &m);
        else { memset(&m, 0, sizeof m); m.sx = m.sy = 1 << 16; }
        matrix_xform(&m, sc, ox, oy + (depth == bar_depth ? OFFSCREEN : 0));
        uint8_t mb[64];
        const size_t nl = matrix_write(mb, &m);
        Buf body = {0, 0, 0};
        uint8_t f = (uint8_t)(f1 | 0x04);
        buf_put(&body, &f, 1);
        {
            const size_t at = body.n;
            buf_put(&body, b + t[i].s + 1, j - t[i].s - 1);            /* flags2, depth, name, character */
            if (depth == raise_from) {
                const size_t d = at + (code == 70 ? 1 : 0);
                body.p[d] = (uint8_t)raise_to;
                body.p[d + 1] = (uint8_t)(raise_to >> 8);
            }
            if ((f1 & 0x02) && frame_to && le16(b + j - 2) == FRAME_CID) {   /* our shorter frame instead */
                body.p[at + (j - 2 - t[i].s - 1)] = (uint8_t)frame_to;
                body.p[at + (j - 1 - t[i].s - 1)] = (uint8_t)(frame_to >> 8);
            }
        }
        buf_put(&body, mb, nl);
        buf_put(&body, b + j + ml, t[i].e - j - ml);                   /* the rest as it was */
        put_tag(out, code, body.p, body.n);
        free(body.p);
    }
    return 1;
}

/* Where the panel's frame ends in the unscaled section (twips; measured on bbhost's PC sections, the
 * ornament's right end and the key guide line's bottom), and how far from the corner it goes. */
#define PANEL_R (1417 * 20)
#define PANEL_B (873 * 20)
#define PANEL_MARGIN_X (40 * 20)
#define PANEL_MARGIN_Y ((64 + 16) * 20)   /* above the key guide bar */
#define TABS_Y (122 * 20)
/* the black behind the panel, tabs to key guide line (unscaled section, twips) */
#define PANEL_BACK_X0 (480 * 20)
#define PANEL_BACK_X1 (1440 * 20)
#define PANEL_BACK_Y0 (108 * 20)
#define PANEL_BACK_Y1 (890 * 20)

/* A shorter frame. The section's frame (sprite 135, placed in the section at y 980) has the rows'
 * area down to a line at y 6836, then the Defaults label, the line help and the key guide, and the
 * bottom ornament at 11799..12759. The panel has no key guide and its Defaults row stays among the
 * rows, so the plugin's copy of the frame drops the lower line (depth 33), lifts the bottom ornament
 * (depth 49) by FRAME_CUT and shortens the background (depth 1) by as much. */

#define FRAME_CUT 4300
static int g_frame_made;   /* the shorter frame is in the movie */
static int frame_short(const uint8_t* b, const Tag* tag, uint16_t new_id, Buf* o) {
    /* By where each child sits rather than which it is (0.5.4 wanted exact children and made nothing):
     * at y 8000..11500 the lower line, dropped; below 11500 the bottom ornament, lifted by FRAME_CUT;
     * the background at the lowest depth, shortened by as much. Every frame of the sprite. */
    static Tag t[256];
    const int n = tags_in(b, tag->e, tag->s + 4, tag->e, t, 256);
    if (n <= 0) return 0;
    Buf body = {0, 0, 0};
    uint8_t head[4];
    memcpy(head, b + tag->s, 4);
    head[0] = (uint8_t)new_id; head[1] = (uint8_t)(new_id >> 8);
    buf_put(&body, head, 4);
    int seen = 0, min_depth = 0x10000, dropped[64], nd = 0;
    for (int i = 0; i < n; i++) {
        Place2 p;
        if ((t[i].code == 26 || t[i].code == 70) && place2_parse(b, t[i].code, t[i].s, t[i].e, &p) && (p.flags & 0x02) && p.depth < min_depth)
            min_depth = p.depth;
    }
    for (int i = 0; i < n; i++) {
        Place2 p;
        if (!((t[i].code == 26 || t[i].code == 70) && place2_parse(b, t[i].code, t[i].s, t[i].e, &p))) {
            buf_put(&body, b + t[i].head, t[i].e - t[i].head);
            continue;
        }
        int drop = 0;
        for (int k = 0; k < nd; k++) drop |= dropped[k] == p.depth;
        if (drop && !(p.flags & 0x02)) continue;                      /* a later change of a dropped one */
        if (!(p.flags & 0x04)) { buf_put(&body, b + t[i].head, t[i].e - t[i].head); continue; }
        Matrix m = p.m;
        if (p.depth == min_depth && (p.flags & 0x02)) {                /* the background */
            const float sy = m.sy / 65536.0f, h = 15019.0f * (sy < 0 ? -sy : sy), k = (h - FRAME_CUT) / h;
            const float top = (float)m.ty - 375.0f * sy;
            m.has_scale = 1;
            m.sy = (int32_t)(m.sy * k);
            m.ty = (int32_t)(top + 375.0f * sy * k);
            seen |= 4;
        } else if (m.ty > 8000 && m.ty < 11500) {                       /* the lower line */
            if (nd < 64) dropped[nd++] = p.depth;
            seen |= 1;
            continue;
        } else if (m.ty >= 11500) { m.ty -= FRAME_CUT; seen |= 2; }    /* the bottom ornament */
        else { buf_put(&body, b + t[i].head, t[i].e - t[i].head); continue; }
        uint8_t mb[64];
        size_t ms = t[i].s + 1 + (t[i].code == 70) + 2;                 /* flags, depth */
        ms = p.cls_e > ms ? p.cls_e : ms;
        if (p.flags & 0x02) ms += 2;                                    /* character */
        Buf q = {0, 0, 0};
        buf_put(&q, b + t[i].s, ms - t[i].s);
        buf_put(&q, mb, matrix_write(mb, &m));
        buf_put(&q, b + p.pre_name_s, t[i].e - p.pre_name_s);
        put_tag(&body, t[i].code, q.p, q.n);
        free(q.p);
    }
    if (!(seen & 2)) { free(body.p); return 0; }       /* nothing below to lift: not that frame */
    g_frame_made = 1;
    put_tag(o, 39, body.p, body.n);
    free(body.p);
    return 1;
}

#define MAX_TAGS 20000
typedef struct { const char* donor; const char* name; } Clone;
static const Clone k_clones[3] = {{"PCSetting", "PhotoA"}, {"PCGraphics", "PhotoB"}, {"PCControls", "PhotoC"}};

/* The movie with the clones, or 0. scale: the panel's size; it keeps the stage's bottom right corner. */
/* The slider's number (DefineEditText 63 in the slider widget, sprite 97) is 37 px wide: the game's
 * sliders go to 10, ours to 250. Its bounds are widened around the same centre for three digits. */
#define SLIDER_TEXT_ID 63
static int rect_write(uint8_t* p, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    int n = 1;
    const int32_t v[4] = {x0, x1, y0, y1};
    for (int k = 0; k < 4; k++) if (bits_for(v[k]) > n) n = bits_for(v[k]);
    BitW w = {p, 0};
    wb(&w, (uint32_t)n, 5);
    for (int k = 0; k < 4; k++) wb(&w, (uint32_t)v[k], n);
    if (w.bit & 7) wb(&w, 0, 8 - (int)(w.bit & 7));
    return (int)(w.bit / 8);
}
static int widen_slider_text(const uint8_t* b, const Tag* t, Buf* o) {
    if (t->code != 37 || t->e - t->s < 4 || le16(b + t->s) != SLIDER_TEXT_ID) return 0;
    BitR r = {b + t->s + 2, 0};
    const int n = (int)ub(&r, 5);
    const int32_t x0 = sb(&r, n), x1 = sb(&r, n), y0 = sb(&r, n), y1 = sb(&r, n);
    const size_t old = (r.bit + 7) / 8;
    if (x1 - x0 > 1000) return 0;                       /* already wide (a mods file) */
    const int32_t c = (x0 + x1) / 2, half = 600;        /* 60 px */
    uint8_t rect[24];
    const int len = rect_write(rect, c - half, c + half, y0, y1);
    Buf body = {0, 0, 0};
    buf_put(&body, b + t->s, 2);
    buf_put(&body, rect, (size_t)len);
    buf_put(&body, b + t->s + 2 + old, t->e - t->s - 2 - old);
    put_tag(o, 37, body.p, body.n);
    free(body.p);
    return 1;
}

/* ---- what the plugin adds to each clone: the page tabs over the panel and the camera's key guide on
 * a black bar along the bottom of the screen. Both are text fields named StaticText_<id>, which the game
 * fills from its messages like the section titles (the plugin's message hook answers 990098 and
 * 990097). Built here, so no file but the plugin is needed. */
#define TABS_TEXT_ID_MSG "StaticText_990098"
#define GUIDE_TEXT_ID_MSG "StaticText_990097"
#define BAR_H (64 * 20)
static void put_u16(Buf* o, uint32_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; buf_put(o, x, 2); }
/* an HTML text field in the menu's font, bounds x0..x1 by 0..h (twips), centred */
static void edit_text(Buf* o, uint16_t id, int32_t x0, int32_t x1, int32_t h, uint16_t font_h, const char* init) {
    Buf t = {0, 0, 0};
    uint8_t rect[24];
    put_u16(&t, id);
    buf_put(&t, rect, (size_t)rect_write(rect, x0, x1, -40, h));
    const uint8_t f[2] = {0xEC, 0xB3};   /* text, word wrap, multiline, read only, colour | font class, layout, no select, HTML, outlines */
    buf_put(&t, f, 2);
    buf_put(&t, "MenuFont_01", 12);
    put_u16(&t, font_h);
    const uint8_t rgba[4] = {200, 200, 180, 255};
    buf_put(&t, rgba, 4);
    const uint8_t layout[9] = {2, 0, 0, 0, 0, 0, 0, 0, 0};   /* centre, margins, indent, leading */
    buf_put(&t, layout, 9);
    buf_put(&t, "", 1);                                     /* variable name */
    buf_put(&t, init, strlen(init) + 1);
    put_tag(o, 37, t.p, t.n);
    free(t.p);
}
/* a filled rectangle 0..w by 0..h (DefineShape3, RGBA) */
static void rect_shape(Buf* o, uint16_t id, int32_t w, int32_t h, const uint8_t rgba[4]) {
    Buf t = {0, 0, 0};
    uint8_t r[64];
    put_u16(&t, id);
    buf_put(&t, r, (size_t)rect_write(r, 0, w, 0, h));
    const uint8_t styles[7] = {1, 0x00, rgba[0], rgba[1], rgba[2], rgba[3], 0};   /* one solid fill, no lines */
    buf_put(&t, styles, 7);
    const uint8_t nbits = 0x10;                                                     /* fill bits 1, line bits 0 */
    buf_put(&t, &nbits, 1);
    memset(r, 0, sizeof r);
    BitW w2 = {r, 0};
    wb(&w2, 0, 1); wb(&w2, 0, 1); wb(&w2, 0, 1); wb(&w2, 0, 1); wb(&w2, 1, 1); wb(&w2, 1, 1);   /* fill0 and move */
    wb(&w2, 1, 5); wb(&w2, 0, 1); wb(&w2, 0, 1);                                   /* move to 0,0 */
    wb(&w2, 1, 1);                                                                  /* fill style 0 = 1 */
    const int32_t d[4][2] = {{w, 0}, {0, h}, {-w, 0}, {0, -h}};
    for (int k = 0; k < 4; k++) {
        const int n = 17;
        wb(&w2, 1, 1); wb(&w2, 1, 1); wb(&w2, (uint32_t)(n - 2), 4); wb(&w2, 1, 1);  /* edge, straight, bits, general */
        wb(&w2, (uint32_t)d[k][0], n); wb(&w2, (uint32_t)d[k][1], n);
    }
    wb(&w2, 0, 6);                                                                  /* end */
    if (w2.bit & 7) wb(&w2, 0, 8 - (int)(w2.bit & 7));
    buf_put(&t, r, w2.bit / 8);
    put_tag(o, 32, t.p, t.n);
    free(t.p);
}
static void place_child(Buf* o, uint16_t depth, uint16_t cid, const char* name, int32_t tx, int32_t ty, float sc) {
    Buf t = {0, 0, 0};
    uint8_t m[64];
    Matrix mx;
    memset(&mx, 0, sizeof mx);
    mx.sx = mx.sy = (int32_t)(65536.0f * sc);
    mx.tx = tx;
    mx.ty = ty;
    const uint8_t f = (uint8_t)(0x02 | 0x04 | (name ? 0x20 : 0));
    buf_put(&t, &f, 1);
    put_u16(&t, depth);
    put_u16(&t, cid);
    buf_put(&t, m, matrix_write(m, &mx));
    if (name) buf_put(&t, name, strlen(name) + 1);
    put_tag(o, 26, t.p, t.n);
    free(t.p);
}

static const char* g_movie_why = "";
static int g_movie_icons;   /* every button icon is in the movie */
/* Button icons for the HTML text (<img src='KG_L1'>): the movie exports only the ones the options use
 * (KG_OK, KG_Cancel, the d-pad, ...), each a DefineExternalImage2 naming the game's texture
 * "<name>.tga". The others are made from KG_OK's tag with the name changed. */
static const char* const k_icons[] = {"KG_L1", "KG_R1", "KG_L2", "KG_R2", "KG_L3", "KG_R3", "KG_LS", "KG_RS"};
#define ICON_COUNT (int)(sizeof k_icons / sizeof k_icons[0])
static const uint8_t* find_lstr(const uint8_t* p, size_t n, const char* str) {   /* a UI8-length string */
    const size_t l = strlen(str);
    for (size_t i = 0; i + 1 + l <= n; i++)
        if (p[i] == l && !memcmp(p + i + 1, str, l)) return p + i;
    return 0;
}
static int icon_tag(Buf* o, const uint8_t* body, size_t n, uint16_t donor_id, uint16_t id, const char* name) {
    const uint8_t* en = find_lstr(body, n, "KG_OK");
    const uint8_t* fn = en ? find_lstr(en + 6, n - (size_t)(en + 6 - body), "KG_OK.tga") : 0;
    if (!en || !fn) return 0;
    Buf t = {0, 0, 0};
    uint8_t pre[64];
    const size_t pl = (size_t)(en - body);
    if (pl > sizeof pre) return 0;
    memcpy(pre, body, pl);
    for (size_t j = 0; j + 1 < pl; j++)   /* the id, as UI16 or UI32 */
        if (pre[j] == (uint8_t)donor_id && pre[j + 1] == (uint8_t)(donor_id >> 8)) { pre[j] = (uint8_t)id; pre[j + 1] = (uint8_t)(id >> 8); j++; }
    buf_put(&t, pre, pl);
    uint8_t l = (uint8_t)strlen(name);
    buf_put(&t, &l, 1);
    buf_put(&t, name, l);
    char file[32];
    strcpy(file, name);
    strcat(file, ".tga");
    l = (uint8_t)strlen(file);
    buf_put(&t, &l, 1);
    buf_put(&t, file, l);
    const uint8_t* rest = fn + 1 + 9;
    buf_put(&t, rest, n - (size_t)(rest - body));
    put_tag(o, 1009, t.p, t.n);
    free(t.p);
    return 1;
}

static uint8_t* movie_with_photo(const uint8_t* b, size_t size, float scale, int icons, size_t* out_size) {
    if (size < 32 || memcmp(b, "GFX", 3) != 0) { g_movie_why = "not an uncompressed GFX"; return 0; }
    const int nbits = b[8] >> 3;
    BitR r = {b + 8, 5};
    const int32_t xmin = sb(&r, nbits), xmax = sb(&r, nbits), ymin = sb(&r, nbits), ymax = sb(&r, nbits);
    const size_t body = 8 + (5 + 4 * (size_t)nbits + 7) / 8 + 4;
    static Tag t[MAX_TAGS];
    const int n = tags_in(b, size, body, size, t, MAX_TAGS);
    if (n <= 0) { g_movie_why = "malformed tags"; return 0; }

    int max_id = 0, max_depth = 0, sym = -1, icon_donor = -1, icon_have[ICON_COUNT] = {0}, frame_tag = -1;
    int place_tag[3] = {-1, -1, -1}, sprite_tag[3] = {-1, -1, -1};
    Place2 place[3];
    for (int i = 0; i < n; i++) {
        if (is_define(t[i].code) && t[i].e - t[i].s >= 2 && le16(b + t[i].s) > max_id) max_id = le16(b + t[i].s);
        if (t[i].code == 26 || t[i].code == 70) {   /* bbhost places its sections hidden, with PlaceObject3 */
            Place2 p;
            if (!place2_parse(b, t[i].code, t[i].s, t[i].e, &p)) continue;
            if (p.depth > max_depth) max_depth = p.depth;
            for (int k = 0; k < 3; k++)
                if (p.name && !strcmp(p.name, k_clones[k].name)) { g_movie_why = "already has the photo sections"; return 0; }
            for (int k = 0; k < 3; k++)
                if (p.name && !strcmp(p.name, k_clones[k].donor) && (p.flags & 0x02)) { place_tag[k] = i; place[k] = p; }
        }
        if (t[i].code == 76 && sym < 0) sym = i;

        if (t[i].code == 1009) {
            const uint8_t* body = b + t[i].s;
            const size_t len = t[i].e - t[i].s;
            if (find_lstr(body, len, "KG_OK")) icon_donor = i;
            for (int k = 0; k < ICON_COUNT; k++) if (find_lstr(body, len, k_icons[k])) icon_have[k] = 1;
        }
    }
    for (int k = 0; k < 3; k++) {
        if (place_tag[k] < 0) { g_movie_why = k_clones[k].donor; return 0; }
        for (int i = 0; i < n; i++)
            if (t[i].code == 39 && le16(b + t[i].s) == place[k].cid) sprite_tag[k] = i;
        if (sprite_tag[k] < 0) { g_movie_why = "a section's sprite is missing"; return 0; }
    }
    /* the frame: the first section's character at depth 3, and its DefineSprite */
    {
        static Tag u[4096];
        const Tag* st = &t[sprite_tag[0]];
        const int m = tags_in(b, st->e, st->s + 4, st->e, u, 4096);
        for (int i = 0; i < m && frame_tag < 0; i++) {
            Place2 p;
            if ((u[i].code == 26 || u[i].code == 70) && place2_parse(b, u[i].code, u[i].s, u[i].e, &p) && (p.flags & 0x02) && p.depth == 3) {
                g_frame_cid = p.cid;
                for (int j = 0; j < n; j++)
                    if (t[j].code == 39 && le16(b + t[j].s) == p.cid) frame_tag = j;
            }
        }
    }

    /* the donors' classes, for the clones */
    const char* cls[3] = {0, 0, 0};
    if (sym >= 0) {
        size_t i = t[sym].s + 2;
        const uint16_t count = le16(b + t[sym].s);
        for (int c = 0; c < count && i + 2 < t[sym].e; c++) {
            const uint16_t id = le16(b + i);
            const char* nm = (const char*)b + i + 2;
            for (int k = 0; k < 3; k++) if (id == place[k].cid) cls[k] = nm;
            i += 2 + strlen(nm) + 1;
        }
    }

    Buf o = {0, 0, 0};
    uint8_t tmp[512];
    if (!buf_put(&o, b, body)) return 0;
    /* the added characters: tabs text, guide text, the bar */
    const uint16_t tabs_id = (uint16_t)(max_id + 4), guide_id = (uint16_t)(max_id + 5), bar_id = (uint16_t)(max_id + 6);
    const uint16_t back_id = (uint16_t)(max_id + 7);
    const int32_t tabs_w = 900 * 20, guide_w = 1800 * 20;
    int extras_done = 0, icon_made[ICON_COUNT] = {0};
    const uint16_t frame_id = (uint16_t)(max_id + 20);
    uint16_t frame_to = 0;   /* our shorter frame, once made */
    /* the panel's bottom (unscaled section, twips): shorter with our frame */
    int32_t panel_b = PANEL_B, back_y1 = PANEL_BACK_Y1;
    for (int i = 0; i < n; i++) {
        if (i == sym) {
            Buf s = {0, 0, 0};
            uint16_t count = le16(b + t[i].s);
            int added = 0;
            for (int k = 0; k < 3; k++) added += cls[k] != 0;
            for (int k = 0; k < ICON_COUNT; k++) added += icon_made[k];
            count = (uint16_t)(count + added);
            tmp[0] = (uint8_t)count; tmp[1] = (uint8_t)(count >> 8);
            buf_put(&s, tmp, 2);
            buf_put(&s, b + t[i].s + 2, t[i].e - t[i].s - 2);
            for (int k = 0; k < 3; k++) {
                if (!cls[k]) continue;
                const uint16_t id = (uint16_t)(max_id + 1 + k);
                tmp[0] = (uint8_t)id; tmp[1] = (uint8_t)(id >> 8);
                buf_put(&s, tmp, 2);
                buf_put(&s, cls[k], strlen(cls[k]) + 1);
            }
            for (int k = 0; k < ICON_COUNT; k++) {
                if (!icon_made[k]) continue;
                const uint16_t id = (uint16_t)(max_id + 8 + k);
                tmp[0] = (uint8_t)id; tmp[1] = (uint8_t)(id >> 8);
                buf_put(&s, tmp, 2);
                buf_put(&s, k_icons[k], strlen(k_icons[k]) + 1);
            }
            put_tag(&o, 76, s.p, s.n);
            free(s.p);
            continue;
        }
        if (!widen_slider_text(b, &t[i], &o)) buf_put(&o, b + t[i].head, t[i].e - t[i].head);
        for (int k = 0; k < 3; k++) {
            const uint16_t id = (uint16_t)(max_id + 1 + k);
            if (i == sprite_tag[k]) {   /* the clone's DefineSprite right after the donor's */
                Buf s = {0, 0, 0};
                const Place2* p = &place[k];
                if (!extras_done) {
                    extras_done = 1;
                    edit_text(&o, tabs_id, -tabs_w / 2, tabs_w / 2, 700, 480,
                              "<p align=\"center\"><font size=\"24\" color=\"#c8c8b4\"> </font></p>");
                    edit_text(&o, guide_id, -guide_w / 2, guide_w / 2, 700, 440,
                              "<p align=\"center\"><font size=\"22\" color=\"#c8c8b4\"> </font></p>");
                    const uint8_t black[4] = {0, 0, 0, 200};
                    rect_shape(&o, bar_id, xmax, BAR_H, black);
                    const uint8_t back[4] = {0, 0, 0, 190};
                    if (frame_tag >= 0 && frame_short(b, &t[frame_tag], frame_id, &o)) {
                        frame_to = frame_id;
                        panel_b -= FRAME_CUT;
                        back_y1 -= FRAME_CUT;
                    }
                    rect_shape(&o, back_id, PANEL_BACK_X1 - PANEL_BACK_X0, back_y1 - PANEL_BACK_Y0, back);
                    if (icons && icon_donor >= 0)
                        for (int q = 0; q < ICON_COUNT; q++)
                            if (!icon_have[q])
                                icon_made[q] = icon_tag(&o, b + t[icon_donor].s, t[icon_donor].e - t[icon_donor].s,
                                                        le16(b + t[icon_donor].s), (uint16_t)(max_id + 8 + q), k_icons[q]);
                }
                /* stage = placement + child: child' = child * s + (corner - placement) * (1 - s) */
                /* stage' = stage * s + T, with the panel's right and bottom edges (PANEL_R, PANEL_B in the
                 * unscaled section) PANEL_MARGIN from the stage's corner */
                const float Tx = (float)(xmax - PANEL_MARGIN_X) - PANEL_R * scale, Ty = (float)(ymax - PANEL_MARGIN_Y) - panel_b * scale;
                const int32_t ox = (int32_t)(Tx - p->m.tx * (1.0f - scale)), oy = (int32_t)(Ty - p->m.ty * (1.0f - scale));
                /* stage positions, as children of the section (placed at p->m.tx, ty) */
                Buf extra = {0, 0, 0};
                const float tabs_x = 960 * 20 * scale + Tx, tabs_y = TABS_Y * scale + Ty;
                /* the black behind the panel: depth 2, free in ControllSetting (1 the dimmer, 3 the frame) */
                place_child(&extra, 2, back_id, 0, (int32_t)(PANEL_BACK_X0 * scale + Tx) - p->m.tx,
                            (int32_t)(PANEL_BACK_Y0 * scale + Ty) - p->m.ty, scale);
                place_child(&extra, 3000, tabs_id, TABS_TEXT_ID_MSG, (int32_t)tabs_x - p->m.tx, (int32_t)tabs_y - p->m.ty, scale);
                place_child(&extra, 3001, bar_id, 0, -p->m.tx, ymax - BAR_H - p->m.ty, 1.0f);
                place_child(&extra, 3002, guide_id, GUIDE_TEXT_ID_MSG, xmax / 2 - p->m.tx, ymax - BAR_H + 300 - p->m.ty, 1.0f);
                const int ok = sprite_xform(b, t[i].s, t[i].e, scale, ox, oy, &extra, frame_to, &s);
                free(extra.p);
                if (!ok) { free(s.p); free(o.p); return 0; }
                s.p[0] = (uint8_t)id; s.p[1] = (uint8_t)(id >> 8);
                put_tag(&o, 39, s.p, s.n);
                free(s.p);
            }
            if (i == place_tag[k]) {    /* and its placement after the donor's */
                const Place2* p = &place[k];
                const Matrix m = p->m;
                (void)xmin; (void)ymin;
                Buf s = {0, 0, 0};
                const uint16_t depth = (uint16_t)(max_depth + 1 + k);
                tmp[0] = (uint8_t)(p->flags | 0x04 | 0x20);
                buf_put(&s, tmp, 1);
                if (p->code == 70) buf_put(&s, &p->flags2, 1);
                tmp[0] = (uint8_t)depth; tmp[1] = (uint8_t)(depth >> 8);
                buf_put(&s, tmp, 2);
                buf_put(&s, b + p->cls_s, p->cls_e - p->cls_s);
                tmp[0] = (uint8_t)id; tmp[1] = (uint8_t)(id >> 8);
                buf_put(&s, tmp, 2);
                const size_t ml = matrix_write(tmp, &m);
                buf_put(&s, tmp, ml);
                buf_put(&s, b + p->pre_name_s, p->pre_name_e - p->pre_name_s);
                buf_put(&s, k_clones[k].name, strlen(k_clones[k].name) + 1);
                buf_put(&s, b + p->post_name_s, p->post_name_e - p->post_name_s);
                put_tag(&o, p->code, s.p, s.n);
                free(s.p);
            }
        }
    }
    if (!o.p) return 0;
    g_movie_icons = icons && icon_donor >= 0;
    for (int k = 0; k < ICON_COUNT; k++) if (!icon_have[k] && !icon_made[k]) g_movie_icons = 0;
    const uint32_t len = (uint32_t)o.n;
    o.p[4] = (uint8_t)len; o.p[5] = (uint8_t)(len >> 8); o.p[6] = (uint8_t)(len >> 16); o.p[7] = (uint8_t)(len >> 24);
    *out_size = o.n;
    return o.p;
}
