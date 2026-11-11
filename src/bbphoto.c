/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbphoto: photo mode for Bloodborne 1.09 as a bbhost plugin (port of bbmod for shadPS4).
 *
 * Free camera: Lance McDonald's debug camera patch, written by the plugin: hold Action (Cross) and
 * press L3. Modes Frozen (the game paused; Cross steps it), Static and Live. Entering it opens the
 * photo panel, an options-menu section with six pages (Camera, Picture, Depth of Field, Chromatic
 * Aberration, Fog, Motion Blur): L1/R1 turn the pages, Circle hides the panel and R3 shows it again.
 *
 * The panel is the game's own option section, opened from code the way the System menu opens one, on
 * the plugin's copies of the PC sections of bbhost's generated optionsetting.gfx (movie.c): bbhost
 * already gave the section-name table a seventh slot pointing at its own name buffer, so the plugin
 * writes a sprite name there before each open. The game calls back into the plugin through empty
 * guest functions in the free camera's code cave, hooked with hook2.
 *
 * Built against bbhost's plugin API (include/bbhost_plugin.h, GPL-3.0-or-later), so this plugin is
 * GPL-3.0-or-later as well.
 */
#include "bbhost_plugin.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "cave_bytes.h"
#include "movie.c"

static const BbHostApi* api;
static uint64_t g_slide;   /* runtime = Binary Ninja address + g_slide */

/* no settings: one empty heading, so that the plugin menu does not say "No settings." */
BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {{BB_OPT_HEADING, "", ""}, {0}};

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "bbphoto", "Photo mode", "1.0.1", "Nox-",
    "Free camera (P, or hold Action and press L3) with an in-game panel: camera, picture, depth of field, "
    "chromatic aberration, fog and motion blur."};

/* ---- helpers -------------------------------------------------------------------------------- */

#define L(...) api->log(__VA_ARGS__)

static uint64_t now_us(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (f.QuadPart / 1000000));
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

static uint64_t rt(uint64_t bn) { return bn + g_slide; }
/* a Binary Ninja address in the image, or a runtime one outside it; safe */
static uint64_t rd64(uint64_t at) {
    uint64_t v = 0;
    return at && api->read(at, &v, 8) == 0 ? v : 0;
}
static uint64_t mem64(uint64_t a) { uint64_t v; memcpy(&v, (void*)(uintptr_t)a, 8); return v; }
static int plausible(uint64_t p) { return p > 0x10000 && p < 0x800000000000ULL && (p & 7) == 0; }
static float rdf(uint64_t a) { float f; memcpy(&f, (void*)(uintptr_t)a, 4); return f; }
static void wrf(uint64_t a, float f) { memcpy((void*)(uintptr_t)a, &f, 4); }
static uint8_t clamp8(float v, int mx) { int x = (v == v) ? (int)(v + 0.5f) : 0; return (uint8_t)(x < 0 ? 0 : x > mx ? mx : x); }

/* A game function by Binary Ninja address, from the main thread (a hook or on_frame). */
static uint64_t call6(uint64_t bn, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    BbCallRegs r;
    memset(&r, 0, sizeof r);
    r.arg[0] = a; r.arg[1] = b; r.arg[2] = c; r.arg[3] = d; r.arg[4] = e; r.arg[5] = f;
    return api->call_guest(bn, &r) == 0 ? r.ret : 0;
}
static uint64_t call4(uint64_t bn, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return call6(bn, a, b, c, d, 0, 0); }
/* a function pointer the game holds (a vtable entry) */
static uint64_t call_ptr(uint64_t fn, uint64_t a, uint64_t b) { return call6(fn - g_slide, a, b, 0, 0, 0, 0); }
#define P(x) ((uint64_t)(uintptr_t)(x))

/* ---- rows and their values ------------------------------------------------------------------ */

enum { PG_CAMERA, PG_PICTURE, PG_DOF, PG_CA, PG_FOG, PG_MB, PAGE_COUNT };
static const char16_t* const k_page_titles[PAGE_COUNT] = {u"Photo Mode - Camera", u"Photo Mode - Picture",
    u"Photo Mode - Depth of Field", u"Photo Mode - Chromatic Aberration", u"Photo Mode - Fog", u"Photo Mode - Motion Blur"};
/* bbhost's sections (engine/menu_assets.cpp): their widget slots and title ids. A page needs at least
 * as many slots as rows (one more shows the game's Defaults row), and two pages next to each other
 * may not use the same sprite (opening one sprite right after itself crashed on shadPS4). */
static const char* const k_pc_sprite[PAGE_COUNT] = {"PCSetting", "PCGraphics", "PCControls", "PCEffects", "PCSetting", "PCControls"};
/* the plugin's own clones of three of them, smaller and in the bottom right corner (movie.c) */
static const char* const k_photo_sprite[PAGE_COUNT] = {"PhotoA", "PhotoB", "PhotoC", "PhotoA", "PhotoB", "PhotoC"};
static const char* const* k_page_sprite = k_pc_sprite;
static const uint32_t k_title_ids[] = {116020, 117020, 118020, 120020};

#define FOV_MIN 20
#define FOV_MAX 110
#define ROLL_CENTER 90
static volatile uint8_t g_page_row;
/* The pad (scePadReadState through the cave's wrapper): L1/R1 turn the pages and are kept from the
 * panel (it jumps sliders with them); OPTIONS is kept from the game in the free camera. Without the
 * wrapper the first row picks the page. */
static int g_pad_ok;
static volatile uint32_t g_buttons;   /* as the pad gave them, before our masks */

/* The keyboard and mouse (bbhost API 13): kb_frame reads them each frame, on_pad adds what they press
 * to the pad's data - buttons, the right stick from the mouse, L2/R2 - so the free camera and the panel
 * take them as the controller's. */
enum { K_PHOTO, K_UP, K_DOWN, K_FAST, K_SLOW, K_STEP, K_PANEL, K_PREV, K_NEXT, K_COUNT };
#define KEY_MOUSE 1000   /* + a BbInput.buttons bit; the rest SDL scancodes */
static const int g_key[K_COUNT] = {19, 44, 224, KEY_MOUSE + 0, KEY_MOUSE + 1, 17, 43, 20, 8};
static const char16_t* const g_key_name[K_COUNT] = {u"P", u"Space", u"Ctrl", u"Left Click", u"Right Click", u"N", u"Tab", u"Q", u"E"};
static int g_kb_ok;
#define MOUSE_SENS 1.0f
static volatile uint32_t g_kb_btn;           /* buttons the keys press now */
static volatile uint32_t g_kb_strip;         /* buttons bbhost's own bindings of our page keys press in the panel */
static float g_look_x, g_look_y;             /* the mouse's motion for the camera, in counts (main thread) */
static volatile int g_kb_held;               /* a key or mouse button is held */
static volatile int g_kb_last;               /* the keyboard was used last: its key guide */
static volatile int g_guide_kb = -1;         /* the key guide the panel shows (1 the keyboard's) */
static volatile uint8_t g_fov_deg = 43, g_roll_val = ROLL_CENTER, g_hud_on = 0, g_vig_pct, g_vig_entry;
static volatile uint8_t g_sat = 100, g_vib = 100, g_exp = 100, g_gamma = 100, g_con = 100;
static volatile uint8_t g_dof_enable, g_dof_focus = 30, g_dof_flvl, g_dof_blvl = 1, g_dof_on;
static volatile uint8_t g_ca_enable, g_ca_v[4] = {100, 100, 100, 100}, g_ca_on;
static uint8_t g_ca_entry[4];
static volatile uint8_t g_fog_dist = 100, g_fog_alpha = 100, g_fog_con = 100, g_fog_doff = 100, g_fog_hoff = 100;
static volatile uint8_t g_mb_enable, g_mb_time, g_mb_len, g_mb_scale, g_mb_samples = 4;
static uint8_t g_mb_entry[4], g_dof_entry[4];

/* message ids of the plugin's own text (the message hook below answers them) */
enum {
    MSG_MODE = 990041, MSG_PAGE = 990040, MSG_FOV = 990000, MSG_HUD, MSG_ROLL, MSG_SAT, MSG_VIB, MSG_CON, MSG_EXP, MSG_VIG = 990008,
    MSG_GAMMA = 990010, MSG_DOF, MSG_FOCUS, MSG_FLVL = 990020, MSG_BLVL, MSG_CA_L0, MSG_CA_L1, MSG_CA_U0, MSG_CA_U1, MSG_CA_EN,
    MSG_FOG_DIST, MSG_FOG_ALPHA, MSG_FOG_CON, MSG_FOG_DOFF, MSG_FOG_HOFF,
    MSG_MB_EN, MSG_MB_TIME, MSG_MB_LEN, MSG_MB_SCALE, MSG_MB_SAMPLES
};
static const struct { uint32_t id; const char16_t* text; } k_texts[] = {
    {MSG_MODE, u"Mode"}, {MSG_PAGE, u"Page"}, {MSG_FOV, u"FOV"}, {MSG_HUD, u"HUD"}, {MSG_ROLL, u"Roll"}, {MSG_SAT, u"Saturation"},
    {MSG_VIB, u"Vibrance"}, {MSG_CON, u"Contrast"}, {MSG_EXP, u"Exposure"},
    {MSG_VIG, u"Vignette"}, {MSG_GAMMA, u"Gamma"}, {MSG_DOF, u"Depth of Field"}, {MSG_FOCUS, u"Focus Depth"},
    {MSG_FLVL, u"Front Blur"}, {MSG_BLVL, u"Back Blur"}, {MSG_CA_L0, u"Lateral Dispersion 1"},
    {MSG_CA_L1, u"Lateral Dispersion 2"}, {MSG_CA_U0, u"Uniform Dispersion 1"}, {MSG_CA_U1, u"Uniform Dispersion 2"},
    {MSG_CA_EN, u"Chromatic Aberration"}, {MSG_FOG_DIST, u"Fog Distance"}, {MSG_FOG_ALPHA, u"Fog Intensity"},
    {MSG_FOG_CON, u"Fog Contrast"}, {MSG_FOG_DOFF, u"Fog Distance Offset"}, {MSG_FOG_HOFF, u"Height Fog Offset"},
    {MSG_MB_EN, u"Motion Blur"}, {MSG_MB_TIME, u"Blur Time"}, {MSG_MB_LEN, u"Max Blur Length"},
    {MSG_MB_SCALE, u"Blur Scale"}, {MSG_MB_SAMPLES, u"Samples"},
};

/* ---- building rows -------------------------------------------------------------------------- */

#define MSG_REPOSITORY 0x01ee8cc0ULL   /* fills a caption: (out, group, id) */
#define SCRATCH_INIT 0x0208af20ULL
#define ADD_SLIDER 0x01f2ac00ULL
#define ADD_CHOICE 0x01f2a100ULL
#define BUILD_ONOFF 0x01f2b3b0ULL
#define OPEN_SECTION 0x01f20900ULL
#define SECTION_TABLE 0x057398b0ULL    /* slot [6]: bbhost's name buffer */

/* A caption keeps a wstring copy of its text beside the pointer; one of eight characters or more is on
 * the game's heap: capacity at +0x18, buffer at +0x00, allocator at +0x20 (vtable +0x70 frees). */
static void free_wstring(uint8_t* ws) {
    const uint64_t cap = mem64(P(ws + 0x18)), ptr = mem64(P(ws)), alloc = mem64(P(ws + 0x20));
    if (cap < 8 || !ptr || !plausible(alloc)) return;
    call_ptr(mem64(mem64(alloc) + 0x70), alloc, ptr);
}
static void caption(uint8_t* buf, uint32_t id) {
    memset(buf, 0, 0x80);
    call4(MSG_REPOSITORY, P(buf), 0xc8, id, 0);
    call4(MSG_REPOSITORY, P(buf + 0x40), 0xc9, id, 0);
}
static void caption_free(uint8_t* buf) {
    free_wstring(buf + 0x08);
    free_wstring(buf + 0x48);
}

/* FUN_01f2ac00 builds its slider with a fixed range {0, 10, 1} written as mov imm32 in its code: they
 * are found once and changed only while our row is built. */
static uint64_t g_sl_min, g_sl_max, g_sl_step;
static int g_sl_searched;
static void find_slider_range(void) {
    uint8_t f[0x740];
    g_sl_searched = 1;
    if (api->read(ADD_SLIDER, f, sizeof f)) return;
    for (int i = 24; i < 0x700; i++) {
        uint32_t v;
        memcpy(&v, f + i + 6, 4);
        if (!(f[i] == 0xc7 && f[i + 1] == 0x85 && v == 10)) continue;
        uint64_t mn = 0, st = 0;
        for (int j = -24; j <= 24; j++) {
            if (!j) continue;
            const uint8_t* q = f + i + j;
            memcpy(&v, q + 6, 4);
            if (q[0] != 0xc7 || q[1] != 0x85) continue;
            if (!mn && v == 0) mn = ADD_SLIDER + i + j + 6;
            if (!st && v == 1) st = ADD_SLIDER + i + j + 6;
        }
        if (mn && st) { g_sl_max = ADD_SLIDER + i + 6; g_sl_min = mn; g_sl_step = st; return; }
    }
    L("bbphoto: slider range not found, sliders stay 0-10");
}
static void put_imm(uint64_t at, uint32_t v) {
    uint32_t cur;
    if (api->read(at, &cur, 4) == 0 && cur != v) api->patch(at, &cur, &v, 4);
}
static void add_slider(uint64_t dialog, uint32_t msg, volatile uint8_t* val, uint8_t def, int mn, int mx, int step) {
    uint8_t c[0x80] __attribute__((aligned(16)));
    if (!g_sl_searched) find_slider_range();
    caption(c, msg);
    if (g_sl_max) { put_imm(g_sl_min, (uint32_t)mn); put_imm(g_sl_max, (uint32_t)mx); put_imm(g_sl_step, (uint32_t)step); }
    call4(ADD_SLIDER, dialog, P(c), P(val), P(&def));
    if (g_sl_max) { put_imm(g_sl_min, 0); put_imm(g_sl_max, 10); put_imm(g_sl_step, 1); }
    caption_free(c);
}
/* A pick-list row (what Language's Text uses): entries of 0x48 bytes - int32 value, the drawn UTF-16
 * pointer at +8, a wstring copy at +0x10 (FUN_02d67040 builds it), a flag at +0x40 - appended with
 * FUN_01f1c0c0 to a zeroed container, then FUN_01f29370(dialog, captions, int32*, list, &default). */
#define WSTR_INIT 0x02d67040ULL
#define LIST_APPEND 0x01f1c0c0ULL
#define ADD_LIST 0x01f29370ULL
static void add_list(uint64_t dialog, uint32_t msg, volatile int32_t* val, const char16_t* const* names, int count, int32_t def) {
    static uint8_t list[0x1000] __attribute__((aligned(16)));
    uint8_t c[0x80] __attribute__((aligned(16)));
    memset(list, 0, sizeof list);
    caption(c, msg);
    for (int i = 0; i < count; i++) {
        uint8_t e[0x48] __attribute__((aligned(16)));
        memset(e, 0, sizeof e);
        const int32_t v = i;
        const uint64_t t = P(names[i]);
        memcpy(e, &v, 4);
        memcpy(e + 8, &t, 8);
        call4(WSTR_INIT, P(e + 0x10), t, 0, 0);
        e[0x40] = 1;
        call4(LIST_APPEND, P(list), P(e), 0, 0);
        free_wstring(e + 0x18);
    }
    call6(ADD_LIST, dialog, P(c), P(val), P(list), P(&def), 0);
    for (int i = 0; i < count; i++) free_wstring(list + i * 0x48 + 0x18);
    caption_free(c);
}
/* the free camera's modes: the game paused, the camera still while the hunter moves, or the camera
 * flying while the game runs */
enum { MODE_FROZEN, MODE_STATIC, MODE_LIVE, MODE_COUNT };
static const char16_t* const k_mode_names[MODE_COUNT] = {u"Frozen", u"Static", u"Live"};
static volatile int32_t g_cam_mode;       /* the Mode row (it changes while its list is browsed) */
static volatile int32_t g_mode_applied;   /* what the camera does: the row once confirmed with Cross */
static volatile uint8_t g_live;   /* read by the cave's pause check (mode_stub.s) */
static int g_fc_entered;          /* the free camera just started (frame advance waits for Cross let go) */

static void add_onoff(uint64_t dialog, uint32_t msg, volatile uint8_t* val, uint8_t def) {
    uint8_t c[0x80] __attribute__((aligned(16))), list[0x100] __attribute__((aligned(16)));
    memset(list, 0, sizeof list);
    caption(c, msg);
    call4(BUILD_ONOFF, P(list), 0, 0, 0);
    call6(ADD_CHOICE, dialog, P(c), P(val), P(list), P(&def), 0);
    caption_free(c);
}

static volatile int g_page;
static volatile uint64_t g_page_opened_at;

static void build_rows(uint64_t dialog) {
    uint8_t scratch[0x100] __attribute__((aligned(16)));
    memset(scratch, 0, sizeof scratch);
    call4(SCRATCH_INIT, P(scratch), 0, 0, 0);
    g_page_row = (uint8_t)g_page;
    if (!g_pad_ok) add_slider(dialog, MSG_PAGE, &g_page_row, (uint8_t)g_page, 0, PAGE_COUNT - 1, 1);
    switch (g_page) {
    case PG_CAMERA:
        add_list(dialog, MSG_MODE, &g_cam_mode, k_mode_names, MODE_COUNT, MODE_FROZEN);
        add_slider(dialog, MSG_FOV, &g_fov_deg, 43, FOV_MIN, FOV_MAX, 2);
        add_slider(dialog, MSG_ROLL, &g_roll_val, ROLL_CENTER, 0, 180, 2);
        add_onoff(dialog, MSG_HUD, &g_hud_on, 0);
        add_slider(dialog, MSG_VIG, &g_vig_pct, g_vig_entry, 0, 100, 2);
        break;
    case PG_PICTURE:
        add_slider(dialog, MSG_SAT, &g_sat, 100, 0, 200, 5);
        add_slider(dialog, MSG_VIB, &g_vib, 100, 20, 250, 5);
        add_slider(dialog, MSG_EXP, &g_exp, 100, 10, 250, 5);
        add_slider(dialog, MSG_GAMMA, &g_gamma, 100, 50, 150, 2);
        add_slider(dialog, MSG_CON, &g_con, 100, 80, 120, 1);
        break;
    case PG_DOF:
        add_onoff(dialog, MSG_DOF, &g_dof_enable, g_dof_enable);
        add_slider(dialog, MSG_FOCUS, &g_dof_focus, 30, 5, 200, 5);
        add_slider(dialog, MSG_FLVL, &g_dof_flvl, 0, 0, 7, 1);
        add_slider(dialog, MSG_BLVL, &g_dof_blvl, 1, 0, 7, 1);
        break;
    case PG_CA:
        add_onoff(dialog, MSG_CA_EN, &g_ca_enable, g_ca_enable);
        for (int i = 0; i < 4; i++) add_slider(dialog, MSG_CA_L0 + i, &g_ca_v[i], 100, 0, 200, 2);
        break;
    case PG_FOG:
        add_slider(dialog, MSG_FOG_DIST, &g_fog_dist, 100, 10, 200, 2);
        add_slider(dialog, MSG_FOG_ALPHA, &g_fog_alpha, 100, 0, 200, 2);
        add_slider(dialog, MSG_FOG_CON, &g_fog_con, 100, 0, 200, 2);
        add_slider(dialog, MSG_FOG_DOFF, &g_fog_doff, 100, 0, 200, 2);
        add_slider(dialog, MSG_FOG_HOFF, &g_fog_hoff, 100, 0, 200, 2);
        break;
    case PG_MB:
        add_onoff(dialog, MSG_MB_EN, &g_mb_enable, g_mb_enable);
        add_slider(dialog, MSG_MB_TIME, &g_mb_time, g_mb_time, 0, 100, 2);
        add_slider(dialog, MSG_MB_LEN, &g_mb_len, g_mb_len, 0, 100, 2);
        add_slider(dialog, MSG_MB_SCALE, &g_mb_scale, g_mb_scale, 0, 100, 2);
        add_slider(dialog, MSG_MB_SAMPLES, &g_mb_samples, g_mb_samples, 1, 16, 1);
        break;
    }
}

/* ---- the panel: opener and handler (called by the game through the cave's empty functions) --- */

#define CAVE_OPENER 0x01de3570ULL
#define CAVE_HANDLER 0x01de3580ULL

static int on_handler(BbHookCtx* ctx, void* user) {   /* handler(dialog, params) */
    (void)user;
    build_rows(ctx->arg[0]);
    g_page_opened_at = now_us();
    return 1;
}
static int on_opener(BbHookCtx* ctx, void* user) {    /* opener(root, params) -> dialog */
    (void)user;
    char* name = (char*)(uintptr_t)rd64(SECTION_TABLE + 8 * 6);
    if (!name) { ctx->ret = 0; return 1; }
    strcpy(name, k_page_sprite[g_page]);
    ctx->ret = call6(OPEN_SECTION, ctx->arg[0], ctx->arg[1], P(name), rt(CAVE_HANDLER), 0, 0);
    return 1;
}

/* Opening a menu from code, as the menu flow opens System/Network: a screen made by FUN_01fb4c70 from
 * an opener functor, pushed with FUN_01fb27a0 and a callback functor. Done at the start of the flow's
 * own update (FUN_01faf8c0, the flow in rdi), on its thread. [flow+0xa88] is the open menu and
 * [flow+0xb12] = 1 asks it to close. */
#define FLOW_UPDATE 0x01faf8c0ULL
#define MAKE_SCREEN 0x01fb4c70ULL
#define PUSH_SCREEN 0x01fb27a0ULL
#define OPENER_VTABLE 0x05743880ULL
#define CALLBACK_VTABLE 0x05740dd0ULL
#define PAGE_REOPEN_US 150000
#define REOPEN_US 150000
#define OPEN_SETTLE_US 350000
#define PAGE_COOLDOWN_US 350000

static volatile int g_fc;             /* free camera on */
static volatile int g_open_req, g_close_req, g_reopen, g_menu_open;
#define GUIDE_SWITCH_US 300000
static uint64_t g_our_menu, g_closing_menu, g_menu_closed_at, g_menu_opened_at;
static int g_menu_prev_open;

static void release(uint64_t p) {
    if (!p) return;
    if (__atomic_fetch_sub((int*)(uintptr_t)(p + 8), 1, __ATOMIC_SEQ_CST) == 1) call_ptr(mem64(mem64(p)), p, 0);
}
static void functor_destroy(uint8_t* obj) {
    const uint64_t impl = mem64(P(obj + 0x20));
    if (impl) call_ptr(mem64(mem64(impl) + 0x20), impl, impl != P(obj));
}
static void open_panel(uint64_t flow) {
    uint8_t fn[0x40] __attribute__((aligned(16))), cb[0x40] __attribute__((aligned(16)));
    uint64_t v, screen = 0, out = 0;
    memset(fn, 0, sizeof fn);
    v = rt(OPENER_VTABLE); memcpy(fn, &v, 8);
    v = rt(CAVE_OPENER);   memcpy(fn + 8, &v, 8);
    v = P(fn);             memcpy(fn + 0x20, &v, 8);
    call4(MAKE_SCREEN, P(&screen), flow + 0x48, P(fn), 0);
    functor_destroy(fn);
    if (!screen) { L("bbphoto: the panel's screen was not made"); return; }
    memset(cb, 0, sizeof cb);
    v = rt(CALLBACK_VTABLE); memcpy(cb, &v, 8);
    v = P(cb);               memcpy(cb + 0x20, &v, 8);
    call4(PUSH_SCREEN, P(&out), flow, P(&screen), P(cb));
    release(out);
    functor_destroy(cb);
    release(screen);
    g_our_menu = mem64(flow + 0xa88);
}

static int on_flow_update(BbHookCtx* ctx, void* user) {
    (void)user;
    const uint64_t flow = ctx->arg[0];
    if (!plausible(flow)) return 0;
    const uint64_t cur = mem64(flow + 0xa88), now = now_us();
    g_menu_open = cur != 0;
    if (g_menu_prev_open && !g_menu_open) g_menu_closed_at = now;
    g_menu_prev_open = g_menu_open;
    /* Mode takes effect when its choice is confirmed: Cross pressed and let go (browsing the list writes
     * the row; Circle puts the old value back). With the panel closed the row is what counts. */
    {
        static uint32_t prev;
        static int confirm;
        const uint32_t bt = g_buttons;
        if (!(g_menu_open && cur == g_our_menu)) { g_mode_applied = g_cam_mode; confirm = 0; }
        else if ((bt & ~prev) & 0x4000) confirm = 1;
        else if (confirm && !(bt & 0x4000)) { g_mode_applied = g_cam_mode; confirm = 0; }
        prev = bt;
    }
    /* L1/R1 (a press during the cooldown waits for it) or the Page row: reopen on the other page */
    {
        static uint32_t prev;
        static int pending;   /* +1 R1, -1 L1 */
        const uint32_t bt = g_buttons, down = bt & ~prev;
        prev = bt;
        const int ours = g_fc && g_menu_open && cur == g_our_menu && !g_reopen;
        if (ours && (down & 0x0800)) pending = 1;
        else if (ours && (down & 0x0400)) pending = -1;
        if (!g_fc || (!g_menu_open && !g_reopen)) pending = 0;
        if (ours && now - g_page_opened_at >= PAGE_COOLDOWN_US) {
            int to = g_page;
            if (pending) to = (g_page + (pending > 0 ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
            else if (!g_pad_ok && g_page_row != g_page && g_page_row < PAGE_COUNT) to = g_page_row;
            pending = 0;
            /* the other device used for a moment: the same page again, with its key guide and tabs */
            static uint64_t other_since;
            const int other = g_guide_kb >= 0 && g_guide_kb != g_kb_last;
            if (!other) other_since = 0;
            else if (!other_since) other_since = now;
            if (to != g_page || (other && now - other_since >= GUIDE_SWITCH_US)) {
                g_page = to;
                g_close_req = 1;
                g_reopen = 1;
                other_since = 0;
            }
        }
    }
    if (g_close_req && g_our_menu && now - g_menu_opened_at < OPEN_SETTLE_US) return 0;   /* still opening */
    if (g_close_req) {
        g_close_req = 0;
        if (cur && cur == g_our_menu) { g_closing_menu = cur; *(uint8_t*)(uintptr_t)(flow + 0xb12) = 1; }
        g_our_menu = 0;
    }
    if (g_reopen) {
        static uint64_t closed_at;
        if (cur) { closed_at = 0; return 0; }
        if (!closed_at) { closed_at = now; return 0; }
        if (now - closed_at < PAGE_REOPEN_US) return 0;
        closed_at = 0;
        g_reopen = 0;
        if (g_fc) { open_panel(flow); g_menu_opened_at = now; }
        return 0;
    }
    if (!g_open_req) return 0;
    if (!g_fc) { g_open_req = 0; return 0; }
    if (cur) {
        if (cur != g_closing_menu && cur != g_our_menu) g_open_req = 0;   /* someone else's menu */
        return 0;
    }
    if (now - g_menu_closed_at < REOPEN_US) return 0;
    g_open_req = 0;
    open_panel(flow);
    g_menu_opened_at = now;
    return 0;
}

/* ---- text ----------------------------------------------------------------------------------- */
/* MsgRepositoryImp::LookupEntry (0x0136f340): (repo, ?, group, id) -> UTF-16. Our ids answer with our
 * text; while the panel is up, the titles of the borrowed sections and the key guide are ours. */
#define MSG_LOOKUP 0x0136f340ULL
static const uint8_t k_lookup_prologue[15] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6, 0x31, 0xc0, 0x48, 0x85, 0xf6};

/* The tabs over the panel (StaticText_990098, HTML): the page in white, the others grey. */
static const char16_t* const k_tab_names[PAGE_COUNT] = {u"Camera", u"Picture", u"DOF", u"CA", u"Fog", u"Blur"};
static const char16_t* page_tabs(void) {
    static char16_t buf[768];
    int k = 0;
#define PUT(s) do { const char16_t* q_ = (s); while (*q_ && k < 766) buf[k++] = *q_++; } while (0)
    if (g_pad_ok && g_kb_last) { PUT(u"<font color='#8a8a7a'>"); PUT(g_key_name[K_PREV]); PUT(u"</font>    "); }
    else if (g_pad_ok) PUT(g_movie_icons ? u"<img src='KG_L1' vspace='-8'>    " : u"<font color='#8a8a7a'>L1</font>    ");
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i) PUT(u"   ");
        if (i == g_page) { PUT(u"<b><font color='#ffffff'>"); PUT(k_tab_names[i]); PUT(u"</font></b>"); }
        else { PUT(u"<font color='#6e6e64'>"); PUT(k_tab_names[i]); PUT(u"</font>"); }
    }
    if (g_pad_ok && g_kb_last) { PUT(u"    <font color='#8a8a7a'>"); PUT(g_key_name[K_NEXT]); PUT(u"</font>"); }
    else if (g_pad_ok) PUT(g_movie_icons ? u"    <img src='KG_R1' vspace='-8'>" : u"    <font color='#8a8a7a'>R1</font>");
#undef PUT
    buf[k] = 0;
    return buf;
}
/* the key guide on the bar along the bottom (StaticText_990097). The game's names are Japanese style:
 * KG_OK draws Circle and KG_Cancel Cross on this disc. */
#define ICON(n) u"<img src='" n u"' vspace='-8'>"
static const char16_t k_guide_icons[] =
    ICON("KG_L2") ICON("KG_R2") u": Down / Up     " ICON("KG_L1") ICON("KG_R1") u": Speed / Page     "
    ICON("KG_Cancel") u": Next frame     " ICON("KG_Cancel") u"+" ICON("KG_L3") u": Exit     "
    ICON("KG_OK") u": Hide panel     " ICON("KG_R3") u": Show panel";
static const char16_t k_guide[] =
    u"L2 / R2: Down / Up     L1 / R1: Speed (panel hidden), Page (panel open)     Cross: Next frame     "
    u"Action + L3: Exit     Circle: Hide panel     R3: Show panel";

/* the key guide for the keyboard, from the keys set */
static const char16_t* kb_guide(void) {
    static char16_t buf[512];
    int k = 0;
#define PUT(s) do { const char16_t* q_ = (s); while (*q_ && k < 510) buf[k++] = *q_++; } while (0)
    PUT(u"WASD: Move     Mouse: Look     "); PUT(g_key_name[K_UP]); PUT(u" / "); PUT(g_key_name[K_DOWN]); PUT(u": Up / Down     ");
    PUT(g_key_name[K_FAST]); PUT(u" / "); PUT(g_key_name[K_SLOW]); PUT(u": Faster / Slower     "); PUT(g_key_name[K_STEP]);
    PUT(u": Next frame     "); PUT(g_key_name[K_PANEL]); PUT(u": Panel     "); PUT(g_key_name[K_PREV]); PUT(u" / "); PUT(g_key_name[K_NEXT]); PUT(u": Page     "); PUT(g_key_name[K_PHOTO]);
    PUT(u": Exit");
#undef PUT
    buf[k] = 0;
    return buf;
}

static int on_lookup(BbHookCtx* ctx, void* user) {
    (void)user;
    const uint32_t group = (uint32_t)ctx->arg[2], id = (uint32_t)ctx->arg[3];
    const char16_t* t = 0;
    if (group == 0xc8 && id == 990098) t = page_tabs();
    else if (group == 0xc8 && id == 990097) {
        g_guide_kb = g_kb_ok && g_kb_last;
        t = g_guide_kb ? kb_guide() : g_movie_icons ? k_guide_icons : k_guide;
    }
    else if (id >= 990000 && id < 990100) {
        if (group == 0xc9) t = u"";
        else for (size_t i = 0; i < sizeof k_texts / sizeof k_texts[0]; i++)
            if (k_texts[i].id == id) t = k_texts[i].text;
    } else if (g_fc && group == 0xc8) {
        for (size_t i = 0; i < sizeof k_title_ids / sizeof k_title_ids[0]; i++)
            if (k_title_ids[i] == id) t = k_page_titles[g_page];
    } else if (g_fc && group == 0xca && id == 110000) {
        t = g_pad_ok ? u"Circle: Hide   R3: Show   L1/R1: Page" : u"Circle: Hide   R3: Show   Page row: change page";
    }
    if (!t) return 0;
    ctx->ret = P(t);
    return 1;
}

/* ---- free camera ---------------------------------------------------------------------------- */

#define CAVE_ENTRY 0x01972998ULL
#define CAVE_CODE 0x01de3328ULL
#define CAVE_FRAME 0x01de3560ULL
#define HUD_ROOT 0x0593b130ULL   /* [[root] + 0x48] + 9: 1 = HUD visible */
#define PAD_HELD 0x01391c60ULL   /* (pad, key id) & 1 */
#define KEY_R3 0x54

static int g_fc_prev, g_hud_saved = -1, g_r3_prev;
static float g_roll_base[16], g_roll_written[16], g_roll_applied;
static int g_roll_have, g_roll_layout = -1;
static uint64_t g_roll_cam;

static uint8_t* hud_byte(void) {
    const uint64_t root = rd64(HUD_ROOT);
    const uint64_t opt = plausible(root) ? rd64(root + 0x48) : 0;
    return plausible(opt) ? (uint8_t*)(uintptr_t)(opt + 9) : 0;
}

static void axis_get(const float* m, int layout, int i, float* v) {
    for (int k = 0; k < 3; k++) v[k] = layout == 0 ? m[i * 4 + k] : m[k * 4 + i];
}
static void axis_set(float* m, int layout, int i, const float* v) {
    for (int k = 0; k < 3; k++) {
        if (layout == 0) m[i * 4 + k] = v[k];
        else m[k * 4 + i] = v[k];
    }
}
/* v about the unit axis k by a radians */
static void rotate3(float* v, const float* k, float a) {
    const float c = cosf(a), s = sinf(a), d = (k[0] * v[0] + k[1] * v[1] + k[2] * v[2]) * (1 - c);
    const float x[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + k[i] * d;
}
static void norm3(float* v) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) for (int i = 0; i < 3; i++) v[i] /= l;
}
/* The mouse turns the camera itself, an angle per count as Dark Souls III's: about the world's up (y)
 * and the camera's right, the pitch kept short of straight up or down, slower when zoomed in. */
static void look_apply(float* m, int layout) {
    if (g_look_x == 0.0f && g_look_y == 0.0f) return;
    const float k = 0.0035f * MOUSE_SENS * (float)g_fov_deg / 60.0f;
    float yaw = g_look_x * k, pitch = g_look_y * k;
    g_look_x = g_look_y = 0;
    float r[3], u[3], f[3];
    static const float up[3] = {0, 1, 0};
    axis_get(m, layout, 0, r);
    axis_get(m, layout, 1, u);
    axis_get(m, layout, 2, f);
    /* whatever the matrix's handedness: right on the mouse turns forward toward right, down toward -up */
    if (f[2] * r[0] - f[0] * r[2] < 0) yaw = -yaw;   /* (up x f) . r, up = y */
    {
        const float x[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        if (x[0] * u[0] + x[1] * u[1] + x[2] * u[2] > 0) pitch = -pitch;   /* about right, forward goes to r x f */
    }
    rotate3(r, up, yaw);
    rotate3(u, up, yaw);
    rotate3(f, up, yaw);
    norm3(r);
    float u2[3] = {u[0], u[1], u[2]}, f2[3] = {f[0], f[1], f[2]};
    rotate3(u2, r, pitch);
    rotate3(f2, r, pitch);
    if (f2[1] < 0.985f && f2[1] > -0.985f && u2[1] > 0.0f) { memcpy(u, u2, sizeof u); memcpy(f, f2, sizeof f); }
    norm3(f);
    /* back to square: up from forward and right */
    u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];
    norm3(u);
    float t[3]; axis_get(m, layout, 1, t);
    if (t[0] * u[0] + t[1] * u[1] + t[2] * u[2] < 0) for (int i = 0; i < 3; i++) u[i] = -u[i];   /* the game's handedness */
    axis_set(m, layout, 0, r);
    axis_set(m, layout, 1, u);
    axis_set(m, layout, 2, f);
}
static void roll_apply(float* m, int layout, float deg) {   /* right and up about forward */
    if (deg == 0.0f) return;
    float r[3], u[3], r2[3], u2[3];
    const float c = cosf(deg * 0.017453292f), s = sinf(deg * 0.017453292f);
    axis_get(m, layout, 0, r);
    axis_get(m, layout, 1, u);
    for (int k = 0; k < 3; k++) { r2[k] = c * r[k] + s * u[k]; u2[k] = -s * r[k] + c * u[k]; }
    axis_set(m, layout, 0, r2);
    axis_set(m, layout, 1, u2);
}
/* The free camera starts from the matrix we left: take our roll back out when it moved, or the turn
 * adds up frame after frame. */
static void roll_frame(uint64_t cam) {
    float* m = (float*)(uintptr_t)(cam + 0x10);
    if (g_roll_layout < 0) g_roll_layout = (fabsf(m[3]) < 1e-4f && fabsf(m[7]) < 1e-4f && fabsf(m[11]) < 1e-4f) ? 0 : 1;
    const float deg = (float)((int)g_roll_val - ROLL_CENTER);
    if (cam != g_roll_cam) { g_roll_cam = cam; g_roll_have = 0; }
    if (!(g_roll_have && memcmp(m, g_roll_written, sizeof g_roll_written) == 0)) {
        memcpy(g_roll_base, m, sizeof g_roll_base);
        if (g_roll_have) roll_apply(g_roll_base, g_roll_layout, -g_roll_applied);
    }
    look_apply(g_roll_base, g_roll_layout);
    g_roll_have = 1;
    float out[16];
    memcpy(out, g_roll_base, sizeof out);
    roll_apply(out, g_roll_layout, deg);
    g_roll_applied = deg;
    memcpy(m, out, sizeof out);
    memcpy(g_roll_written, out, sizeof out);
}

/* the area's values when the free camera starts (the global post-process block) */
#define POST_BLOCK 0x059406e0ULL
static uint8_t* g_post;

/* Each row keeps what the player set: a row still at the area's value (as it was at the last entry)
 * takes the new area's, a changed one stays. The entries are the new area's, so the changed rows keep
 * being written. */
static int g_have_area;
static uint8_t g_ca_en_entry, g_mb_en_entry;
static void keep_or_area(volatile uint8_t* row, volatile uint8_t* entry, uint8_t area) {
    if (!g_have_area || *row == *entry) *row = area;
    *entry = area;
}
static void load_area(void) {
    const uint8_t* b = g_post;
    float v[4], f;
    int32_t i1, i2;
    memcpy(v, b + 0xb4, sizeof v);
    g_ca_on = 0;
    for (int i = 0; i < 4; i++) {
        int x = (v[i] == v[i]) ? (int)(v[i] * 100.0f + (v[i] < 0 ? -0.5f : 0.5f)) + 100 : 100;
        x &= ~1;
        keep_or_area(&g_ca_v[i], &g_ca_entry[i], (uint8_t)(x < 0 ? 0 : x > 200 ? 200 : x));
        if (g_ca_v[i] != g_ca_entry[i]) g_ca_on = 1;
    }
    keep_or_area(&g_ca_enable, &g_ca_en_entry, b[0xac] ? 1 : 0);
    memcpy(&f, b + 0xd0, 4);
    keep_or_area(&g_vig_pct, &g_vig_entry, (uint8_t)(clamp8(f > 0 ? f * 10.0f : 0, 100) & ~1));
    memcpy(&f, b + 0x1b0, 4);
    int d = (f > 0 && f == f) ? (int)(sqrtf(f) * 10.0f + 0.5f) : 30;
    d = (d + 2) / 5 * 5;
    memcpy(&i1, b + 0x1d0, 4);
    memcpy(&i2, b + 0x1d4, 4);
    keep_or_area(&g_dof_enable, &g_dof_entry[0], b[0x1ac] ? 1 : 0);
    keep_or_area(&g_dof_focus, &g_dof_entry[1], (uint8_t)(d < 5 ? 5 : d > 200 ? 200 : d));
    keep_or_area(&g_dof_flvl, &g_dof_entry[2], (uint8_t)(i1 < 0 ? 0 : i1 > 7 ? 7 : i1));
    keep_or_area(&g_dof_blvl, &g_dof_entry[3], (uint8_t)(i2 < 0 ? 0 : i2 > 7 ? 7 : i2));
    g_dof_on = g_dof_enable != g_dof_entry[0] || g_dof_focus != g_dof_entry[1] || g_dof_flvl != g_dof_entry[2] ||
               g_dof_blvl != g_dof_entry[3];
    keep_or_area(&g_mb_enable, &g_mb_en_entry, b[0x1dc] ? 1 : 0);
    memcpy(&f, b + 0x1e4, 4); keep_or_area(&g_mb_time, &g_mb_entry[0], clamp8(f * 100.0f, 100) & ~1);
    memcpy(&f, b + 0x1e8, 4); keep_or_area(&g_mb_len, &g_mb_entry[1], clamp8(f * 100.0f, 100) & ~1);
    memcpy(&f, b + 0x350, 4); keep_or_area(&g_mb_scale, &g_mb_entry[2], clamp8(f * 10.0f, 100) & ~1);
    memcpy(&i1, b + 0x1ec, 4); keep_or_area(&g_mb_samples, &g_mb_entry[3], (uint8_t)(i1 < 1 ? 1 : i1 > 16 ? 16 : i1));
    g_have_area = 1;
}

static void fog_restore(void);
static void vig_tick(void);
static void vig_restore(void);

#define STEP_FRAMES 2

/* The roll's invincibility (its animation's JumpTable 8 "Invincible", frames 0-11) is a bit the game
 * sets on the hunter's action flags each frame it lasts (ActionFlag module +0x1bc, 0x02; found by
 * freezing in and after rolls). Frozen keeps it set, so the paused hunter cannot be hit by another
 * player's game: Frozen clears it each frame. */
#define AF_FLAGS 0x1bc
#define AF_INVINCIBLE 0x02
static void iframes_off(void) {
    BbChr c[64];
    size_t n = 0;
    if (api->chr_list(c, 64, &n)) return;
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; i++) {
        if (!c[i].is_player) continue;
        const uint64_t ins = (uint64_t)(uintptr_t)c[i].ins;
        const uint64_t box = plausible(ins) ? mem64(ins + 0x3b0) : 0;
        const uint64_t af = plausible(box) ? mem64(box + 0x08) : 0;
        if (plausible(af)) *(volatile uint8_t*)(uintptr_t)(af + AF_FLAGS) &= (uint8_t)~AF_INVINCIBLE;
        return;
    }
}

static int on_freecam_frame(BbHookCtx* ctx, void* user) {
    (void)user;
    uint8_t* r12 = (uint8_t*)(uintptr_t)ctx->arg[0];
    uint8_t* r13 = (uint8_t*)(uintptr_t)ctx->arg[1];
    const uint64_t pad = ctx->arg[2];
    if (!r12) return 0;
    const int on = r12[0xC4] == 1;
    if (on && !g_fc_prev && g_menu_open) {       /* only with no menu of the game's open */
        r12[0xC4] = 0;
        if (r13) r13[0x150] = 0;
        return 0;
    }
    uint8_t* hud = hud_byte();
    if (on && !g_fc_prev) {
        g_hud_saved = hud ? *hud : -1;
        /* FOV, roll and HUD keep what they were set to; the first time, the camera's FOV */
        const uint64_t cam = mem64(P(r12 + 0xD0));
        if (!g_have_area && plausible(cam)) g_fov_deg = clamp8(rdf(cam + 0x50) * 57.29578f, FOV_MAX);
        if (g_fov_deg < FOV_MIN) g_fov_deg = FOV_MIN;
        g_roll_have = 0;
        g_roll_cam = 0;
        g_cam_mode = g_mode_applied = MODE_FROZEN;   /* the photo mode always starts with the game paused */
        g_fc_entered = 1;
        load_area();
        g_page = PG_CAMERA;
        g_fc = 1;
        g_open_req = 1;
    } else if (!on && g_fc_prev) {
        if (hud && g_hud_saved >= 0) *hud = (uint8_t)g_hud_saved;
        g_fc = 0;
        fog_restore();
        vig_restore();
        g_open_req = 0;
        g_reopen = 0;
        if (g_menu_open) g_close_req = 1;
    }
    g_fc_prev = on;
    if (!on) { g_live = 0; return 0; }
    /* the mode: Static is the camera's own flag (+0xC8; the game runs and the hunter moves), Live the
     * cave's pause check skipped; both clear the pause request the entry made */
    {
        const int32_t mode = g_mode_applied;
        /* frame advance (Frozen): each press of Cross with the panel hidden lets the game run
         * STEP_FRAMES frames, once Cross has been let go since the free camera started (it is held to enter it) */
        static int armed, prev_cross, steps;
        const int cross = (g_buttons & 0x4000) != 0;
        if (g_fc_entered) { armed = 0; g_fc_entered = 0; steps = 0; }
        if (mode == MODE_FROZEN && armed && cross && !prev_cross && !g_menu_open && !g_reopen) steps = STEP_FRAMES;
        const int step = steps > 0 && mode == MODE_FROZEN;
        if (steps > 0) steps--;
        if (!cross) armed = 1;
        prev_cross = cross;
        r12[0xC8] = mode == MODE_STATIC;
        g_live = mode == MODE_LIVE || step;
        if (mode == MODE_FROZEN && !step) iframes_off();
        if ((mode != MODE_FROZEN || step) && r13) r13[0x150] = 0;
    }
    if (hud) *hud = g_hud_on ? 1 : 0;
    /* with the panel up the game turns the pad's key assign off (+0x20): keep it on for the camera */
    if (pad && g_menu_open && ((uint8_t*)(uintptr_t)pad)[0x20] == 0) ((uint8_t*)(uintptr_t)pad)[0x20] = 1;
    const int r3 = pad && (call6(PAD_HELD, pad, KEY_R3, 0, 0, 0, 0) & 1);
    if (r3 && !g_r3_prev && !g_menu_open) g_open_req = 1;
    g_r3_prev = r3;
    const uint64_t cam = mem64(P(r12 + 0xD0));
    if (!plausible(cam)) return 0;
    wrf(cam + 0x50, g_fov_deg * 0.017453292f);
    roll_frame(cam);
    vig_tick();
    return 0;
}

#define CAVE_LIVE_FLAG 0x01de35f1ULL   /* mode_stub.s: movabs rax, <the flag> */
#define PAD_GOT 0x057e4ea0ULL      /* the eboot's import slot for scePadReadState */
#define PAD_WRAPPER 0x01de3590ULL
#define PAD_ORIG 0x01de35b8ULL
#define PAD_HOOK 0x01de35c0ULL
static int on_pad(BbHookCtx* ctx, void* user) {   /* (data, the read's result) after the read */
    (void)user;
    uint32_t* data = (uint32_t*)(uintptr_t)ctx->arg[0];
    if (!data || (int32_t)ctx->arg[1] != 0) return 0;
    uint32_t b = *data;
    uint8_t* d8 = (uint8_t*)data;   /* ScePadData: +4 left stick, +6 right stick, +8 L2, +9 R2 */
    if (g_kb_ok) {
        const int sticks = d8[4] < 88 || d8[4] > 168 || d8[5] < 88 || d8[5] > 168 || d8[6] < 88 || d8[6] > 168 ||
                           d8[7] < 88 || d8[7] > 168;
        if ((b || sticks) && !g_kb_held && !g_kb_btn) g_kb_last = 0;   /* the controller, with no key held */
        b &= ~g_kb_strip;
        const uint32_t k = g_kb_btn;
        if (k) {
            b |= k;
            if (k & 0x0100) d8[8] = 255;
            if (k & 0x0200) d8[9] = 255;
        }
        *data = b;
    }
    g_buttons = b;
    uint32_t mask = 0;
    if (g_fc) mask |= 0x0008;                                        /* OPTIONS */
    if (g_fc && (g_menu_open || g_reopen)) mask |= 0x0400 | 0x0800;  /* L1, R1 */
    if (b & mask) *data = b & ~mask;
    return 0;
}
static int install_pad(void) {
    const uint64_t orig = rd64(PAD_GOT), lo = rt(0x400000), hi = rt(0x06000000);
    uint64_t zero = 0, wrapper = rt(PAD_WRAPPER);
    if (!orig || (orig >= lo && orig < hi)) { L("bbphoto: scePadReadState's import slot not found (%llx)", (unsigned long long)orig); return 0; }
    if (api->patch(PAD_ORIG, &zero, &orig, 8) || api->hook2(PAD_HOOK, 0, 14, on_pad, 0) || api->write(PAD_GOT, &wrapper, 8)) {
        L("bbphoto: cannot place the pad wrapper");
        return 0;
    }
    return 1;
}

static int install_freecam(void) {
    uint8_t cur[sizeof k_cave_blk1];
    if (api->read(CAVE_CODE, cur, sizeof k_cave_blk1) || api->patch(CAVE_CODE, cur, k_cave_blk1, sizeof k_cave_blk1)) {
        L("bbphoto: cannot write the free camera's code");
        return 0;
    }
    {   /* the live flag's address into the cave's pause check */
        static const uint8_t mark[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        const uint64_t flag = P(&g_live);
        if (api->patch(CAVE_LIVE_FLAG, mark, &flag, 8)) { L("bbphoto: cannot write the live mode's flag"); return 0; }
    }
    if (api->hook2(CAVE_FRAME, 0, 14, on_freecam_frame, 0) || api->hook2(CAVE_OPENER, 0, 14, on_opener, 0) ||
        api->hook2(CAVE_HANDLER, 0, 14, on_handler, 0)) {
        L("bbphoto: cannot hook the cave's functions");
        return 0;
    }
    if (api->read(CAVE_ENTRY, cur, sizeof k_cave_blk0) || api->patch(CAVE_ENTRY, cur, k_cave_blk0, sizeof k_cave_blk0)) {
        L("bbphoto: cannot write the free camera's entry");
        return 0;
    }
    return 1;
}

/* ---- post-processing (YEBIS) ---------------------------------------------------------------- */
/* sub_25d4860 records the YEBIS pass and starts by copying the global post-process block: at its entry
 * every value is this view's, and the next view starts again from the area's. bbhost hooks the same
 * entry; ours goes in front of it (the 14-byte jump it leaves there runs as it is). */
#define YEBIS_RECORD 0x025d4860ULL
static const uint8_t k_yebis_prologue[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                             0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};

static void scale(int off, int pct) {
    if (pct != 100) wrf(P(g_post + off), rdf(P(g_post + off)) * pct / 100.0f);
}
static int on_yebis(BbHookCtx* ctx, void* user) {
    (void)user;
    const uint8_t* yebis = (const uint8_t*)(uintptr_t)ctx->arg[0];
    if (!g_fc || !g_post || !yebis || yebis[0x2710]) return 0;   /* 0x2710: the block was not refreshed */
    uint8_t* b = g_post;
    scale(0xc8, g_sat);
    scale(0xcc, g_vib);
    scale(0x248, g_con);
    scale(0x98, g_exp);
    scale(0x9c, g_gamma);
    if (!g_ca_on) for (int i = 0; i < 4; i++) if (g_ca_v[i] != g_ca_entry[i]) g_ca_on = 1;
    b[0xac] = g_ca_enable ? 1 : 0;
    if (g_ca_on) for (int i = 0; i < 4; i++) wrf(P(b + 0xb4 + 4 * i), (g_ca_v[i] - 100) / 100.0f);
    b[0x1dc] = g_mb_enable ? 1 : 0;
    if (g_mb_time != g_mb_entry[0]) wrf(P(b + 0x1e4), g_mb_time / 100.0f);
    if (g_mb_len != g_mb_entry[1]) wrf(P(b + 0x1e8), g_mb_len / 100.0f);
    if (g_mb_scale != g_mb_entry[2]) wrf(P(b + 0x350), g_mb_scale / 10.0f);
    if (g_mb_samples != g_mb_entry[3]) { const int32_t n = g_mb_samples; memcpy(b + 0x1ec, &n, 4); }
    b[0x2d0] = 0;                                             /* CustomDof off */
    if (!g_dof_on && (g_dof_enable != g_dof_entry[0] || g_dof_focus != g_dof_entry[1] || g_dof_flvl != g_dof_entry[2] ||
                      g_dof_blvl != g_dof_entry[3])) g_dof_on = 1;
    b[0x1ac] = g_dof_enable ? 1 : 0;
    if (g_dof_on && g_dof_enable) {
        float F = g_dof_focus / 10.0f;
        const int32_t fl = g_dof_flvl, bl = g_dof_blvl;
        F *= F;
        b[0x18c] = 0;                                         /* AutoFocus off */
        wrf(P(b + 0x1b0), F);
        wrf(P(b + 0x1b4), 0.01f);                             /* aperture: the levels set the blur */
        memcpy(b + 0x1d0, &fl, 4);
        memcpy(b + 0x1d4, &bl, 4);
    }
    return 0;
}

/* The vignette. The block's power alone does nothing (an area without vignette keeps none), so like
 * bbhost's own drawparam poke the plugin writes it into the Tone Map entries the view blends: at the
 * render view's entry (sub_269e990: the graphics manager in rdi, the renderer in r9), the Tone Map
 * bank at manager +0x5978 looked up (sub_26044a0) with the renderer's two ids (+0x2a80, +0x2ae8);
 * entry +0x70 is "Yebis-Vignette Power". The entries are the area's own, so they are put back when
 * the free camera ends or the row returns to its value. */
#define RENDER_VIEW 0x0269e990ULL
#define TONEMAP_LOOKUP 0x026044a0ULL
static const uint8_t k_render_view_prologue[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                   0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x0e, 0x00, 0x00};
static struct { uint64_t at; float orig; } g_vig_saved[16];
static int g_vig_n;
/* The render view runs on the render thread, where call_guest is not allowed: the hook only notes the
 * renderers and the manager, and the free camera's frame (main thread) does the lookups and writes.
 * A frame draws several views (since bbhost 0.2.2x also ones of its own after the scene's), so every
 * renderer with a Tone Map entry of its own is kept, not only the last one seen. */
#define VIEW_MAX 8
static volatile uint64_t g_view_r[VIEW_MAX], g_view_mgr;
/* The Tone Map entries found, and the power for them: written again at every view's start, on the
 * render thread, since the game (with bbhost 0.2.32) puts the area's value back in them each frame
 * before the view reads them. */
static volatile uint64_t g_vig_at[16];
static volatile int g_vig_at_n;
static volatile float g_vig_value = -1.0f;   /* < 0: none */
static int on_render_view(BbHookCtx* ctx, void* user) {
    (void)user;
    /* rdi the graphics manager, r9 the renderer (bbhost's own hook reads them so) */
    const uint64_t mgr = ctx->arg[0], r = ctx->arg[5];
    if (!plausible(mgr) || !plausible(r)) return 0;
    const uint32_t* key = (const uint32_t*)(uintptr_t)(r + 0x2a80);
    if (key[0] == 0xffffffffu && key[1] == 0xffffffffu) return 0;   /* no area entry (the title's, menus') */
    const float v = g_vig_value;
    if (v >= 0.0f)
        for (int i = 0; i < g_vig_at_n; i++) *(volatile float*)(uintptr_t)g_vig_at[i] = v;
    g_view_mgr = mgr;
    for (int i = 0; i < VIEW_MAX; i++) {
        if (g_view_r[i] == r) return 0;
        if (!g_view_r[i]) { g_view_r[i] = r; return 0; }
    }
    return 0;
}
static void vig_restore(void) {
    g_vig_value = -1.0f;
    g_vig_at_n = 0;
    for (int i = 0; i < g_vig_n; i++) wrf(g_vig_saved[i].at, g_vig_saved[i].orig);
    g_vig_n = 0;
}
static void vig_tick(void) {
    if (!(g_fc && g_vig_pct != g_vig_entry)) { vig_restore(); return; }
    const uint64_t mgr = g_view_mgr;
    if (!plausible(mgr)) return;
    const uint64_t bank = mem64(mgr + 0x5978);
    if (!plausible(bank)) return;
    const uint32_t ids[2] = {0x2a80, 0x2ae8};
    for (int v = 0; v < VIEW_MAX; v++) {
    const uint64_t r = g_view_r[v];
    if (!plausible(r)) continue;
    for (int k = 0; k < 2; k++) {
        const uint32_t* key = (const uint32_t*)(uintptr_t)(r + ids[k]);
        if (key[0] == 0xffffffffu || key[1] == 0xffffffffu) continue;
        BbCallRegs cr;
        memset(&cr, 0, sizeof cr);
        cr.arg[0] = bank;
        cr.arg[1] = r + ids[k];
        const uint64_t e = api->call_guest(TONEMAP_LOOKUP, &cr) == 0 ? cr.ret : 0;
        if (!plausible(e)) continue;
        const uint64_t at = e + 0x70;
        int known = 0;
        for (int i = 0; i < g_vig_n; i++) known |= g_vig_saved[i].at == at;
        if (!known) {
            if (g_vig_n >= 16) continue;
            g_vig_saved[g_vig_n].at = at;
            g_vig_saved[g_vig_n].orig = rdf(at);
            g_vig_n++;
        }
        wrf(at, g_vig_pct / 10.0f);
    }
    }
    /* for the render view's hook: every entry known, then the value */
    for (int i = 0; i < g_vig_n; i++) g_vig_at[i] = g_vig_saved[i].at;
    g_vig_at_n = g_vig_n;
    g_vig_value = g_vig_pct / 10.0f;
}

/* A hook in front of one bbhost may already have placed: its 14-byte absolute jump, or the original. */
static int hook_shared(uint64_t bn, const uint8_t* orig, size_t n_orig, BbHookFn2 fn, const char* what) {
    uint8_t have[32];
    size_t n = 0;
    if (api->read(bn, have, n_orig)) return 0;
    if (have[0] == 0xff && have[1] == 0x25 && !memcmp(have + 2, "\0\0\0\0", 4)) n = 14;
    else if (!memcmp(have, orig, n_orig)) n = n_orig;
    if (!n || api->hook2(bn, have, n, fn, 0)) {
        L("bbphoto: cannot hook %s (%02x %02x %02x)", what, have[0], have[1], have[2]);
        return 0;
    }
    return 1;
}

/* ---- fog ------------------------------------------------------------------------------------ */
/* The fog drawn comes from a table the renderer rebuilds from the area's GParam fog records when one
 * changes, so the records are what we edit (as the debug menu's FogParamEditor does). The fog manager
 * is [graphics manager + 0x5888]; its +0x10 lists banks (node: [0] next, [3] -> container), container+8
 * lists records (node: [0] next, [2] id, [3] -> record). A record: +0x20 Dist, +0x2c Alpha,
 * +0x30 Contrast, +0x54 DistOffset, +0x58 HeightOffset. Every record of the map gets the rows. */
#define GRAPHICS_MANAGER 0x05940dd8ULL
#define FOG_MAX 64
static uint64_t g_fog_rec[FOG_MAX];
static float g_fog_orig[FOG_MAX][5];
static int g_fog_n = -1;   /* -1: no snapshot */
static const int k_fog_off[5] = {0x20, 0x2c, 0x30, 0x54, 0x58};

static void fog_snapshot(void) {
    g_fog_n = 0;
    const uint64_t gm = rd64(GRAPHICS_MANAGER);
    const uint64_t fm = plausible(gm) ? rd64(gm + 0x5888) : 0;
    const uint64_t head = plausible(fm) ? rd64(fm + 0x10) : 0;
    if (!plausible(head)) return;
    int guard = 0;
    for (uint64_t b = rd64(head); b != head && plausible(b) && guard < 4096; b = rd64(b), guard++) {
        const uint64_t c = rd64(b + 0x18), h2 = plausible(c) ? rd64(c + 8) : 0;
        if (!plausible(h2)) continue;
        for (uint64_t n = rd64(h2); n != h2 && plausible(n) && guard < 4096 && g_fog_n < FOG_MAX; n = rd64(n), guard++) {
            if ((int32_t)rd64(n + 0x10) == -1) continue;
            const uint64_t q = rd64(n + 0x18);
            uint8_t rec[0x60];
            if (!plausible(q) || api->read(q, rec, sizeof rec)) continue;
            g_fog_rec[g_fog_n] = q;
            for (int k = 0; k < 5; k++) memcpy(&g_fog_orig[g_fog_n][k], rec + k_fog_off[k], 4);
            g_fog_n++;
        }
    }
}
static void fog_put(uint64_t a, float v) {   /* only on a change, as the editor does */
    if (rdf(a) != v) wrf(a, v);
}
static void fog_restore(void) {
    for (int i = 0; i < g_fog_n; i++)
        for (int k = 0; k < 5; k++) fog_put(g_fog_rec[i] + k_fog_off[k], g_fog_orig[i][k]);
    g_fog_n = -1;
}
static void fog_frame(void) {
    const int changed = g_fog_dist != 100 || g_fog_alpha != 100 || g_fog_con != 100 || g_fog_doff != 100 || g_fog_hoff != 100;
    if (!(g_fc && changed) || api->loading_screen()) {
        if (g_fog_n >= 0 && !api->loading_screen()) fog_restore();
        return;
    }
    if (g_fog_n < 0) fog_snapshot();
    for (int i = 0; i < g_fog_n; i++) {
        const uint64_t r = g_fog_rec[i];
        const float* o = g_fog_orig[i];
        if (!(o[2] > 0.0001f)) continue;   /* no fog */
        fog_put(r + 0x20, o[0] * g_fog_dist / 100.0f);
        fog_put(r + 0x2c, o[1] * g_fog_alpha / 100.0f);
        fog_put(r + 0x30, o[2] * g_fog_con / 100.0f);
        fog_put(r + 0x54, o[3] + (g_fog_doff - 100) * 20.0f);
        fog_put(r + 0x58, o[4] + (g_fog_hoff - 100) * 10.0f);
    }
}
static void on_world_load(uint32_t block, void* user) {   /* a load frees the records */
    (void)block;
    (void)user;
    g_fog_n = -1;
    g_vig_value = -1.0f;                            /* and the renderers, with their Tone Map entries */
    g_vig_at_n = 0;
    g_vig_n = 0;
    for (int i = 0; i < VIEW_MAX; i++) g_view_r[i] = 0;
}
/* ---- keyboard and mouse -------------------------------------------------------------------- */

static int kb_is_held(const BbInput* in, int c) {
    if (c >= KEY_MOUSE) return (in->buttons >> (c - KEY_MOUSE)) & 1;
    return c > 0 && c < 512 && in->key[c];
}
static void kb_frame(void) {
    if (!g_kb_ok) return;
    BbInput in;
    if (api->input(&in)) return;
    const uint64_t now = now_us();
    static int prev[K_COUNT], taken;
    static uint64_t photo_at, photo_until, panel_until;
    static uint32_t panel_btn;
    int h[K_COUNT], any = in.buttons != 0;
    for (int i = 0; i < K_COUNT; i++) h[i] = kb_is_held(&in, g_key[i]);
    for (int i = 4; i < 512 && !any; i++) any = in.key[i];
    g_kb_held = any;
    if (any || in.dx != 0.0f || in.dy != 0.0f || in.wheel != 0.0f) g_kb_last = 1;
    const int ours = g_fc && g_menu_open;   /* the panel (or another menu) is up */
    /* Action (Cross) held with L3, as on the controller; the panel closed first (Circle), so that Cross
     * does not pick its row */
    if (h[K_PHOTO] && !prev[K_PHOTO] && (in.playing || ours)) {
        if (ours) { panel_btn = 0x2000; panel_until = now + 100000; }
        photo_at = now + (ours ? 250000 : 0);
        photo_until = photo_at + 120000;
    }
    if (g_fc && h[K_PANEL] && !prev[K_PANEL]) { panel_btn = g_menu_open ? 0x2000 : 0x0004; panel_until = now + 100000; }  /* Circle / R3 */
    uint32_t b = 0;
    if (now >= photo_at && now < photo_until) b |= 0x4000 | 0x0002;
    if (now < panel_until) b |= panel_btn;
    if (g_fc && !g_menu_open) {
        if (h[K_UP]) b |= 0x0200;     /* R2 */
        if (h[K_DOWN]) b |= 0x0100;   /* L2 */
        if (h[K_FAST]) b |= 0x0800;   /* R1 */
        if (h[K_SLOW]) b |= 0x0400;   /* L1 */
        if (h[K_STEP]) b |= 0x4000;   /* Cross */
    }
    float mx = 0, my = 0;
    if (g_fc && in.playing) { mx = in.dx; my = in.dy; }
    if (g_fc) { g_look_x += mx; g_look_y += my; }
    else g_look_x = g_look_y = 0;
    /* pages; Q and E are also Lock On (R3) and Interact (Cross), which the panel would take, and Cross
     * between two pages would step the frozen game */
    uint32_t strip = 0;
    if (ours) {
        if (h[K_PREV]) b |= 0x0400;
        if (h[K_NEXT]) b |= 0x0800;
    }
    if (g_fc && (h[K_PREV] || h[K_NEXT]) && !in.key[40]) strip = 0x4000 | 0x0004;
    g_kb_strip = strip;
    g_kb_btn = b;
    memcpy(prev, h, sizeof prev);
    /* in the free camera our keys, the mouse and its wheel are the camera's alone, and Shift and Alt
     * (the game's Strong Attack and walk modifiers) do nothing */
    if (g_fc != taken) {
        taken = g_fc;
        if (taken) {
            uint16_t sc[K_COUNT + 4] = {225, 226, 229, 230};   /* Left/Right Shift, Left/Right Alt */
            size_t n = 4;
            uint32_t mb = 0;
            for (int i = K_UP; i < K_COUNT; i++) {
                if (g_key[i] >= KEY_MOUSE) mb |= 1u << (g_key[i] - KEY_MOUSE);
                else if (g_key[i] > 0) sc[n++] = (uint16_t)g_key[i];
            }
            api->input_take(sc, n, mb, 1, 1);
        } else {
            api->input_take(0, 0, 0, 0, 0);
        }
    }
}

static void on_frame(void* user) {
    (void)user;
    fog_frame();
    kb_frame();

}

/* Our clones in the options movie, through the plugin's overlay (before the game reads it). */
static int install_movie(void) {
    static const char path[] = "/dvdroot_ps4/menu/optionsetting.gfx";
    const void* d = 0;
    size_t n = 0, out_n = 0;
    const float scale = 0.7f;   /* the panel's size, in the bottom right corner */
    if (api->game_file(path, &d, &n)) { L("bbphoto: no %s to add the panel to", path); return 0; }
    uint8_t* m = movie_with_photo((const uint8_t*)d, n, scale, 1, &out_n);
    if (!m) { L("bbphoto: the photo sections were not added to the options movie: %s", g_movie_why); return 0; }
    const int ok = api->overlay_file("bbphoto", path, m, out_n) == 0;
    free(m);
    if (!ok) L("bbphoto: the options movie could not be written");
    if (!g_frame_made) L("bbphoto: the panel's frame was not shortened (another frame than measured)");
    return ok;
}

/* ---- entry ---------------------------------------------------------------------------------- */

static const uint8_t k_flow_prologue[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* a) {
    if (a->version < 12) return 1;
    api = a;
    if (!api->eboot_is_109()) {
        L("bbphoto: not the 1.09 eboot");
        return 1;
    }
    g_slide = api->guest_addr(0x400000) - 0x400000;
    g_post = (uint8_t*)(uintptr_t)rt(POST_BLOCK);
    const uint64_t name = rd64(SECTION_TABLE + 8 * 6);
    uint8_t cmp = 0;
    api->read(0x01f20e21, &cmp, 1);
    if (!name || cmp != 7) L("bbphoto: bbhost's seventh option section is not there (slot %llx, cmp %d): no panel",
                             (unsigned long long)name, cmp);
    if (name && cmp == 7 && install_movie()) k_page_sprite = k_photo_sprite;
    const int fc = install_freecam();
    g_pad_ok = fc && install_pad();
    int panel = 0;
    if (name && cmp == 7) {
        uint8_t flow[20];
        panel = api->read(FLOW_UPDATE, flow, sizeof flow) == 0 && !memcmp(flow, k_flow_prologue, sizeof k_flow_prologue) &&
                api->hook2(FLOW_UPDATE, flow, sizeof flow, on_flow_update, 0) == 0;
        if (!panel) L("bbphoto: cannot hook the menu flow's update: no panel");
        panel = panel && hook_shared(MSG_LOOKUP, k_lookup_prologue, sizeof k_lookup_prologue, on_lookup, "the message lookup");
    }
    const int yb = hook_shared(YEBIS_RECORD, k_yebis_prologue, sizeof k_yebis_prologue, on_yebis, "the YEBIS record");
    const int vig = hook_shared(RENDER_VIEW, k_render_view_prologue, sizeof k_render_view_prologue, on_render_view, "the render view");
    g_kb_ok = g_pad_ok && api->version >= 13 && api->size >= offsetof(BbHostApi, key_bound) + sizeof(void*);
    if (g_kb_ok && api->key_bound((uint16_t)g_key[K_PHOTO])) L("bbphoto: the photo mode's key is also one of the game's");
    api->on_frame(on_frame, 0);
    api->on_world_load(on_world_load, 0);
    L("bbphoto: ready (free camera %s, panel %s, picture %s, vignette %s, L1/R1 %s, keyboard %s)", fc ? "on" : "FAILED",
      panel ? "on" : "FAILED", yb ? "on" : "FAILED", vig ? "on" : "FAILED", g_pad_ok ? "on" : "FAILED",
      g_kb_ok ? "on" : "off (bbhost too old)");
    return 0;
}
