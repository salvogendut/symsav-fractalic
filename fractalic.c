// fractalic.c — Fractal screensaver for SymbOS
// Renders fractals directly to CPC Mode-1 VRAM via Bank_Copy,
// or MSX Screen-7 VRAM via VDP port writes.
// Fractal types: Sierpinski triangle (chaos game), Koch snowflake,
// Dragon curve, Barnsley fern (IFS).
// SymbOS C port by Salvatore Bognanni

#include <symbos.h>
#include <symbos/msgid.h>
#include <symbos/keys.h>
#include <stdlib.h>
#include <string.h>

extern void vdp_fill(unsigned int vram_addr, unsigned char fill_byte, unsigned short len);

#define MSC_SAV_INIT   1
#define MSC_SAV_START  2
#define MSC_SAV_CONFIG 3
#define MSR_SAV_CONFIG 4

#define SCREEN_W  320
#define SCREEN_H  200

// MSX Screen 7: 512x212, 2 pixels/byte (4-bit colour), linear VRAM
#define MSX_W  512
#define MSX_H  212

// ---------------------------------------------------------------------------
// CPC Mode-1 VRAM
// Byte layout for 4 pixels p0..p3 (ink 0-3):
//   bit7=p0_lo  bit6=p1_lo  bit5=p2_lo  bit4=p3_lo
//   bit3=p0_hi  bit2=p1_hi  bit1=p2_hi  bit0=p3_hi
// SymbOS default palette: ink0=white(0x00) ink1=black(0xF0) ink2=dim(0x0F) ink3=bright(0xFF)
// ---------------------------------------------------------------------------

static const unsigned char ink_byte[4] = { 0x00, 0xF0, 0x0F, 0xFF };

// MSX: both nibbles = same colour index (writes 2 adjacent pixels at once)
// ink0=white(0x88) ink1=black(0x11) ink2=dim/green(0x99) ink3=bright/lgreen(0xAA)
static const unsigned char msx_ink_byte[4] = { 0x88, 0x11, 0x99, 0xAA };

// Fractal type IDs stored in cfgdat[4]
#define FRAC_SIERPINSKI  1
#define FRAC_KOCH        2
#define FRAC_DRAGON      3
#define FRAC_FERN        4

// ---------------------------------------------------------------------------
// Data-segment buffers
// ---------------------------------------------------------------------------

_data unsigned char zero_plane[2000];
_data unsigned char fill_buf[80];
_data unsigned char pixbuf;

// Config: [0..3]="FRAC" [4]=type(1-4) [5]=depth(1-3) [6]=speed(1-3)
_data char cfgdat[64];
_data char init_tmp[64];

// Koch snowflake segment storage: up to 192 segments at depth 3
// Each segment: x0,y0,x1,y1 as int (16-bit), 8 bytes per segment
// 192 * 8 = 1536 bytes
#define KOCH_MAX_SEGS  200
_data int koch_x0[KOCH_MAX_SEGS];
_data int koch_y0[KOCH_MAX_SEGS];
_data int koch_x1[KOCH_MAX_SEGS];
_data int koch_y1[KOCH_MAX_SEGS];

// Dragon curve turn bits: 1 bit per turn, packed into bytes
// 12 iterations = 4095 turns = 512 bytes
#define DRAGON_MAX_BYTES  512
_data unsigned char dragon_turns[DRAGON_MAX_BYTES];
_data int dragon_n_turns;   // total number of turns

// ---------------------------------------------------------------------------
// Animation state
// ---------------------------------------------------------------------------

_transfer char          is_msx;
_transfer unsigned short screen_w;
_transfer unsigned short screen_h;

_transfer unsigned char frac_type;       // currently active fractal type
_transfer unsigned char frac_cfg_type;   // user-configured type (0 = cycle all)
_transfer unsigned char frac_depth;
_transfer unsigned char frac_speed;      // 1=slow 2=normal 3=fast
_transfer int           anim_timer;
_transfer int           anim_pause;
_transfer unsigned char anim_stage;      // 0=drawing, 1=pause, 2=restart

// Incremental draw state (Koch and Dragon draw step by step)
_transfer int           draw_idx;        // current segment/step index
_transfer int           draw_total;      // total segments/steps

// ---------------------------------------------------------------------------
// VRAM helpers
// ---------------------------------------------------------------------------

static void vram_clear(void)
{
    unsigned char k;
    if (is_msx) {
        vdp_fill(0u, 0x11u, 54272u);   // 212 rows x 256 bytes = background
        return;
    }
    for (k = 0; k < 8; k++) {
        Bank_Copy(0,
            (char *)(0xC000u + (unsigned short)k * 0x0800u),
            _symbank, (char *)zero_plane, 2000u);
    }
}

static void vram_pixel(int x, int y, unsigned char ink)
{
    unsigned short addr;
    unsigned char pos, lo_mask, hi_mask;

    if (x < 0 || x >= (int)screen_w || y < 0 || y >= (int)screen_h) return;

    if (is_msx) {
        vdp_fill((unsigned int)(unsigned short)y * 256u
                 + (unsigned int)((unsigned short)x >> 1),
                 msx_ink_byte[ink & 3], 1u);
        return;
    }

    addr = 0xC000u
         + (unsigned short)(y >> 3) * 80u
         + (unsigned short)(y &  7) * 0x0800u
         + (unsigned short)(x >> 2);

    pos     = (unsigned char)(x & 3);
    lo_mask = (unsigned char)(0x80u >> pos);
    hi_mask = (unsigned char)(0x08u >> pos);

    Bank_Copy(_symbank, (char *)&pixbuf, 0, (char *)addr, 1u);
    pixbuf &= (unsigned char)(~(lo_mask | hi_mask));
    if (ink & 1) pixbuf |= lo_mask;
    if (ink & 2) pixbuf |= hi_mask;
    Bank_Copy(0, (char *)addr, _symbank, (char *)&pixbuf, 1u);
}

static void vram_line(int x0, int y0, int x1, int y1, unsigned char ink)
{
    int dx, dy, sx, sy, err, e2;

    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;

    for (;;) {
        vram_pixel(x0, y0, ink);
        if (x0 == x1 && y0 == y1) break;
        e2 = err + err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Fill a 4-pixel-aligned rectangle (x and w must be multiples of 4)
static void vram_fill_rect(int x, int y, int w, int h, unsigned char ink)
{
    unsigned short addr;
    unsigned short urow;
    int row, bx, bw;

    if (is_msx) {
        for (urow = (unsigned short)y; urow < (unsigned short)(y + h); urow++)
            vdp_fill((unsigned int)urow * 256u + (unsigned int)((unsigned short)(x >> 1)),
                     msx_ink_byte[ink & 3], (unsigned short)(w >> 1));
        return;
    }

    bx = x >> 2;
    bw = w >> 2;
    if (bw <= 0 || h <= 0) return;

    memset(fill_buf, ink_byte[ink & 3], (unsigned short)bw);

    for (row = y; row < y + h; row++) {
        addr = 0xC000u
             + (unsigned short)(row >> 3) * 80u
             + (unsigned short)(row &  7) * 0x0800u
             + (unsigned short)bx;
        Bank_Copy(0, (char *)addr, _symbank, (char *)fill_buf, (unsigned short)bw);
    }
}

// ---------------------------------------------------------------------------
// Sierpinski Triangle — chaos game
// ---------------------------------------------------------------------------
// Vertices computed at init time from screen dimensions.

static const unsigned char sier_ink[3] = { 3, 2, 0 };  // bright, dim, white

_transfer int sier_ax, sier_ay, sier_bx, sier_by, sier_cx, sier_cy;
_transfer int sier_px, sier_py;
_transfer int sier_step;  // steps done so far
_transfer int sier_total; // total steps

static void sierpinski_init(void)
{
    sier_ax = (int)screen_w / 2;
    sier_ay = 10;
    sier_bx = (int)screen_w / 16;
    sier_by = (int)screen_h * 94 / 100;
    sier_cx = (int)screen_w * 15 / 16;
    sier_cy = sier_by;

    sier_px = sier_ax;
    sier_py = sier_ay;
    sier_step = 0;

    switch (frac_depth) {
        case 1:  sier_total = 8000;  break;
        case 3:  sier_total = 30000; break;
        default: sier_total = 18000; break;
    }
}

// Run N chaos-game steps
static void sierpinski_steps(int n)
{
    int v, nx, ny, i;
    for (i = 0; i < n && sier_step < sier_total; i++, sier_step++) {
        v = rand() % 3;
        switch (v) {
            case 0: nx = (sier_px + sier_ax) >> 1; ny = (sier_py + sier_ay) >> 1; break;
            case 1: nx = (sier_px + sier_bx) >> 1; ny = (sier_py + sier_by) >> 1; break;
            default:nx = (sier_px + sier_cx) >> 1; ny = (sier_py + sier_cy) >> 1; break;
        }
        sier_px = nx;
        sier_py = ny;
        vram_pixel(nx, ny, sier_ink[v]);
    }
}

// ---------------------------------------------------------------------------
// Koch Snowflake — iterative segment expansion
// ---------------------------------------------------------------------------
// sqrt(3)/2 ≈ 7/8 = 0.875 (close enough for pixel art)
// For Koch: given P3=(ax,ay) P5=(bx,by), apex P4 = midpoint + perp * sqrt(3)/2
// perp of (bx-ax, by-ay) rotated 90° CCW = (-(by-ay), bx-ax)
// apex.x = (ax+bx)/2 - (by-ay)*7/8  (interior of snowflake, so subtract)
// apex.y = (ay+by)/2 + (bx-ax)*7/8
//
// Triangle size W is derived from screen height so the snowflake fits:
// total height = W*7/6, so W = 6*(screen_h - 13)/7.

_data int tmp_x0[KOCH_MAX_SEGS];
_data int tmp_y0[KOCH_MAX_SEGS];
_data int tmp_x1[KOCH_MAX_SEGS];
_data int tmp_y1[KOCH_MAX_SEGS];

static void koch_init(void)
{
    int i, j, n, ns, p1x, p1y, p2x, p2y, mx, my, px, py, dx, dy, apx, apy;
    int depth, W;

    // W derived from screen height: total footprint = W*7/6, leaving 13px margin.
    W   = 6 * ((int)screen_h - 13) / 7;
    p1x = (int)screen_w / 2;
    p1y = 7;
    p2x = p1x - W / 2;
    p2y = p1y + W * 7 / 8;
    mx  = p1x + W / 2;
    my  = p2y;

    koch_x0[0] = p1x; koch_y0[0] = p1y; koch_x1[0] = p2x; koch_y1[0] = p2y;
    koch_x0[1] = p2x; koch_y0[1] = p2y; koch_x1[1] = mx;  koch_y1[1] = my;
    koch_x0[2] = mx;  koch_y0[2] = my;  koch_x1[2] = p1x; koch_y1[2] = p1y;
    n = 3;

    depth = (int)frac_depth;  // 1, 2, or 3

    for (i = 0; i < depth; i++) {
        ns = 0;
        for (j = 0; j < n && ns + 4 <= KOCH_MAX_SEGS; j++) {
            p1x = koch_x0[j]; p1y = koch_y0[j];
            p2x = koch_x1[j]; p2y = koch_y1[j];

            // one-third and two-thirds points along the segment
            mx = p1x + (p2x - p1x) / 3;
            my = p1y + (p2y - p1y) / 3;
            px = p1x + (p2x - p1x) * 2 / 3;
            py = p1y + (p2y - p1y) * 2 / 3;

            // apex of equilateral triangle on mx..px
            // rotate (px-mx, py-my) by 90° CCW and scale by sqrt(3)/2 ≈ 7/8
            dx  = px - mx;
            dy  = py - my;
            apx = (mx + px) / 2 - dy * 7 / 8;
            apy = (my + py) / 2 + dx * 7 / 8;

            tmp_x0[ns] = p1x; tmp_y0[ns] = p1y; tmp_x1[ns] = mx;  tmp_y1[ns] = my;  ns++;
            tmp_x0[ns] = mx;  tmp_y0[ns] = my;  tmp_x1[ns] = apx; tmp_y1[ns] = apy; ns++;
            tmp_x0[ns] = apx; tmp_y0[ns] = apy; tmp_x1[ns] = px;  tmp_y1[ns] = py;  ns++;
            tmp_x0[ns] = px;  tmp_y0[ns] = py;  tmp_x1[ns] = p2x; tmp_y1[ns] = p2y; ns++;
        }
        for (j = 0; j < ns; j++) {
            koch_x0[j] = tmp_x0[j]; koch_y0[j] = tmp_y0[j];
            koch_x1[j] = tmp_x1[j]; koch_y1[j] = tmp_y1[j];
        }
        n = ns;
    }

    draw_idx   = 0;
    draw_total = n;
}

static void koch_steps(int count)
{
    int i;
    unsigned char col;
    for (i = 0; i < count && draw_idx < draw_total; i++, draw_idx++) {
        col = (unsigned char)(1 + (draw_idx % 3));  // cycle inks 1,2,3... avoid 1(bg)
        if (col == 1) col = 3;
        vram_line(koch_x0[draw_idx], koch_y0[draw_idx],
                  koch_x1[draw_idx], koch_y1[draw_idx], col);
    }
}

// ---------------------------------------------------------------------------
// Dragon Curve
// ---------------------------------------------------------------------------
// Turn direction stored as 1 bit per turn: 0=left, 1=right
// Iterative generation: start with bit 1, each pass double the sequence
// with the rule: new = old + [1] + mirror(old) with inverted last bit

static void dragon_set_turn(int idx, unsigned char v)
{
    unsigned char byte_i, bit_i;
    byte_i = (unsigned char)((unsigned short)idx >> 3);
    bit_i  = (unsigned char)(idx & 7);
    if (v)
        dragon_turns[byte_i] |=  (unsigned char)(0x80u >> bit_i);
    else
        dragon_turns[byte_i] &= (unsigned char)(~(0x80u >> bit_i));
}

static unsigned char dragon_get_turn(int idx)
{
    unsigned char byte_i, bit_i;
    byte_i = (unsigned char)((unsigned short)idx >> 3);
    bit_i  = (unsigned char)(idx & 7);
    return (dragon_turns[byte_i] >> (7 - bit_i)) & 1u;
}

static void dragon_init(void)
{
    int i, n, iters, old_n, j;

    memset(dragon_turns, 0, sizeof(dragon_turns));
    dragon_set_turn(0, 1);
    n = 1;

    iters = (int)frac_depth + 8;  // depth 1→9 iters, depth 3→11 iters
    if (iters > 11) iters = 11;   // 2^11-1 = 2047 turns, fits in 256 bytes

    for (i = 0; i < iters; i++) {
        old_n = n;
        // Middle turn is always 1
        dragon_set_turn(n, 1);
        // Mirror of original in reverse with toggled parity
        for (j = 0; j < old_n; j++) {
            dragon_set_turn(n + 1 + j, dragon_get_turn(old_n - 1 - j) ^ 1u);
        }
        n = n + 1 + old_n;
        if (n >= DRAGON_MAX_BYTES * 8) { n = DRAGON_MAX_BYTES * 8 - 1; break; }
    }

    dragon_n_turns = n;
    draw_idx   = 0;
    draw_total = n;
}

// Direction encoding: 0=right 1=down 2=left 3=up
static const signed char dir_dx[4] = { 1, 0, -1,  0 };
static const signed char dir_dy[4] = { 0, 1,  0, -1 };

_transfer int dragon_cx, dragon_cy, dragon_dir;
_transfer int dragon_step_size;

static void dragon_reset_pos(void)
{
    dragon_step_size = 2;
    dragon_cx = (int)screen_w / 2;
    dragon_cy = (int)screen_h / 2;
    dragon_dir = 0;
    draw_idx = 0;
}

static void dragon_steps(int count)
{
    int i, nx, ny, t;
    unsigned char ink;
    for (i = 0; i < count && draw_idx <= draw_total; i++, draw_idx++) {
        nx = dragon_cx + (int)dir_dx[dragon_dir] * dragon_step_size;
        ny = dragon_cy + (int)dir_dy[dragon_dir] * dragon_step_size;
        // cycle inks 0, 2, 3 (skip ink 1 = black background)
        switch ((draw_idx >> 4) % 3) {
            case 0:  ink = 0; break;
            case 1:  ink = 2; break;
            default: ink = 3; break;
        }
        vram_line(dragon_cx, dragon_cy, nx, ny, ink);
        dragon_cx = nx;
        dragon_cy = ny;
        if (draw_idx < draw_total) {
            t = dragon_get_turn(draw_idx);
            // 0=left turn, 1=right turn
            if (t)
                dragon_dir = (dragon_dir + 1) & 3;
            else
                dragon_dir = (dragon_dir + 3) & 3;
        }
    }
}

// ---------------------------------------------------------------------------
// Barnsley Fern — IFS chaos game
// Fixed-point scale: 1.0 = 32
// Scale 32 keeps all 16-bit products within range (max ~8720 for T2).
// With scale 256 the 0.85*y term reaches 218*2560=558080 — overflow.
// ---------------------------------------------------------------------------
// Coefficients * 32 (rounded):
// T1 (p=1%):  x'=0,         y'=0.16y       → 0.16*32=5
// T2 (p=85%): x'=0.85x+0.04y y'=-0.04x+0.85y+1.6 → 27, 1, 51
// T3 (p=7%):  x'=0.2x-0.26y  y'=0.23x+0.22y+1.6  → 6,8,7,7,51
// T4 (p=7%):  x'=-0.15x+0.28y y'=0.26x+0.24y+0.44 → 5,9,8,8,14
//
// Screen mapping at scale 32:
//   CPC:  sx = 160 + fern_fx*2,   sy = 199 - fern_fy*5/8
//   MSX:  sx = 256 + fern_fx*3,   sy = 211 - fern_fy*2/3

_transfer int fern_fx, fern_fy;  // current point, scale 32

static void fern_init(void)
{
    fern_fx = 0;
    fern_fy = 0;
    draw_idx  = 0;
    switch (frac_depth) {
        case 1:  draw_total = 8000;  break;
        case 3:  draw_total = 30000; break;
        default: draw_total = 16000; break;
    }
}

static void fern_steps(int n)
{
    int i, r, nx, ny, sx, sy;
    unsigned char ink;

    for (i = 0; i < n && draw_idx < draw_total; i++, draw_idx++) {
        r = rand() % 100;

        if (r == 0) {
            // T1: x'=0, y'=0.16y  (5/32 = 0.156 ≈ 0.16)
            nx = 0;
            ny = fern_fy * 5 / 32;
        } else if (r < 86) {
            // T2: x'=0.85x+0.04y, y'=-0.04x+0.85y+1.6
            // max product: 27*320 = 8640 — fits in 16-bit
            nx = (27 * fern_fx + fern_fy) / 32;
            ny = (-fern_fx + 27 * fern_fy) / 32 + 51;
        } else if (r < 93) {
            // T3: x'=0.2x-0.26y, y'=0.23x+0.22y+1.6
            nx = (6 * fern_fx - 8 * fern_fy) / 32;
            ny = (7 * fern_fx + 7 * fern_fy) / 32 + 51;
        } else {
            // T4: x'=-0.15x+0.28y, y'=0.26x+0.24y+0.44
            nx = (-5 * fern_fx + 9 * fern_fy) / 32;
            ny = (8 * fern_fx + 8 * fern_fy) / 32 + 14;
        }

        fern_fx = nx;
        fern_fy = ny;

        // Map to screen; vram_pixel clips out-of-bounds coordinates
        if (is_msx) {
            sx = (int)screen_w / 2 + fern_fx * 3;
            sy = ((int)screen_h - 1) - fern_fy * 2 / 3;
        } else {
            sx = 160 + fern_fx * 2;
            sy = 199 - fern_fy * 5 / 8;   // fern_fy*5 max=1600, no overflow
        }

        // Color by real y: stem white, mid-fronds dim, tips bright
        ink = (fern_fy > 200) ? 3 : (fern_fy > 64) ? 2 : 0;
        vram_pixel(sx, sy, ink);
    }
}

// ---------------------------------------------------------------------------
// Animation tick dispatch
// ---------------------------------------------------------------------------

static void fractal_init(void)
{
    switch (frac_type) {
        case FRAC_SIERPINSKI: sierpinski_init(); break;
        case FRAC_KOCH:       koch_init();       break;
        case FRAC_DRAGON:     dragon_init(); dragon_reset_pos(); break;
        case FRAC_FERN:       fern_init();       break;
    }
}

static unsigned char fractal_done(void)
{
    switch (frac_type) {
        case FRAC_SIERPINSKI: return (sier_step >= sier_total);
        case FRAC_KOCH:       return (draw_idx  >= draw_total);
        case FRAC_DRAGON:     return (draw_idx  >  draw_total);
        case FRAC_FERN:       return (draw_idx  >= draw_total);
    }
    return 1;
}

static void fractal_step(void)
{
    // steps-per-tick by speed: slow=1x, normal=3x, fast=8x
    int spd;
    spd = (frac_speed == 1) ? 1 : (frac_speed == 3) ? 8 : 3;

    switch (frac_type) {
        case FRAC_SIERPINSKI: sierpinski_steps(spd * 200); break;
        case FRAC_KOCH:       koch_steps(spd);             break;
        case FRAC_DRAGON:     dragon_steps(spd * 20);      break;
        case FRAC_FERN:       fern_steps(spd * 200);       break;
    }
}

// ---------------------------------------------------------------------------
// anim_tick: state machine
//   stage 0 = drawing incrementally
//   stage 1 = pause (5 seconds = ~300 ticks at ~60/s via Idle)
//   stage 2 = clear and restart
// ---------------------------------------------------------------------------

static void anim_tick(void)
{
    if (anim_stage == 0) {
        fractal_step();
        if (fractal_done()) {
            anim_stage = 1;
            anim_timer = anim_pause;  // 5-second hold
        }
    } else if (anim_stage == 1) {
        if (--anim_timer <= 0)
            anim_stage = 2;
    } else {
        // Next type: random if user chose Random, else stick to the configured type
        if (frac_cfg_type == 0)
            frac_type = (unsigned char)(1 + rand() % 4);
        else
            frac_type = frac_cfg_type;
        vram_clear();
        fractal_init();
        anim_stage = 0;
    }
}

// ---------------------------------------------------------------------------
// Key scan
// ---------------------------------------------------------------------------

static unsigned char any_key_down(void)
{
    unsigned char sc;
    for (sc = 0; sc < 80; sc++)
        if (Key_Down(sc)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Desktop stop / resume
// ---------------------------------------------------------------------------

static void desktop_stop(unsigned char wid)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKSTP;
    _symmsg[2] = 0xFF;
    _symmsg[3] = wid;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Msg_Wait(_sympid, 2, _symmsg, MSR_DSK_DSKSRV);
}

static void desktop_cont(void)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKCNT;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Idle();
}

// ---------------------------------------------------------------------------
// Config dialog
// ---------------------------------------------------------------------------

_transfer char        tmp_type  = 1;
_transfer char        tmp_depth = 2;
_transfer char        tmp_speed = 2;
_transfer char        cfg_prz   = 0;
_transfer signed char cfgwin_id = -1;

// rg_type needs 6 slots: 5 radio buttons + 1 terminator
_transfer char rg_type [6] = { -1, -1, -1, -1, -1, -1 };
_transfer char rg_depth[4] = { -1, -1, -1, -1 };
_transfer char rg_speed[4] = { -1, -1, -1, -1 };

_transfer Ctrl_TFrame cfg_tf    = { "Settings",    (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_t = { "Fractal:",    (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_d = { "Depth:",      (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_s = { "Speed:",      (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };

_transfer Ctrl_Radio cfg_rad_t1 = { &tmp_type, "Sierp.",  (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_type };
_transfer Ctrl_Radio cfg_rad_t2 = { &tmp_type, "Koch",    (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_type };
_transfer Ctrl_Radio cfg_rad_t3 = { &tmp_type, "Dragon",  (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_type };
_transfer Ctrl_Radio cfg_rad_t4 = { &tmp_type, "Fern",    (COLOR_BLACK<<2)|COLOR_ORANGE, 4, rg_type };
_transfer Ctrl_Radio cfg_rad_t0 = { &tmp_type, "Random",  (COLOR_BLACK<<2)|COLOR_ORANGE, 0, rg_type };

_transfer Ctrl_Radio cfg_rad_d1 = { &tmp_depth, "Low",    (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_depth };
_transfer Ctrl_Radio cfg_rad_d2 = { &tmp_depth, "Med",    (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_depth };
_transfer Ctrl_Radio cfg_rad_d3 = { &tmp_depth, "High",   (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_depth };

_transfer Ctrl_Radio cfg_rad_s1 = { &tmp_speed, "Slow",   (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_speed };
_transfer Ctrl_Radio cfg_rad_s2 = { &tmp_speed, "Normal", (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_speed };
_transfer Ctrl_Radio cfg_rad_s3 = { &tmp_speed, "Fast",   (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_speed };

// Dialog: 280 x 86 px (widened to fit 5 type options on one row)
// Fractal row: Sierp.(54) Koch(100) Dragon(136) Fern(182) Random(218)
_transfer Ctrl ccc0  = { 0,  C_AREA,   -1, COLOR_ORANGE,                      0,  0, 280, 86, 0 };
_transfer Ctrl ccc1  = { 0,  C_TFRAME, -1, (unsigned short)&cfg_tf,           2,  1, 276, 62, 0 };
_transfer Ctrl ccc2  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_t,        8,  9,  42,  8, 0 };
_transfer Ctrl ccc3  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_t1,      54,  9,  42,  8, 0 };
_transfer Ctrl ccc4  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_t2,      98,  9,  34,  8, 0 };
_transfer Ctrl ccc5  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_t3,     134,  9,  44,  8, 0 };
_transfer Ctrl ccc6  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_t4,     180,  9,  34,  8, 0 };
_transfer Ctrl ccc7  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_t0,     216,  9,  52,  8, 0 };
_transfer Ctrl ccc8  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_d,        8, 22,  42,  8, 0 };
_transfer Ctrl ccc9  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d1,      54, 22,  28,  8, 0 };
_transfer Ctrl ccc10 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d2,      84, 22,  30,  8, 0 };
_transfer Ctrl ccc11 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d3,     116, 22,  36,  8, 0 };
_transfer Ctrl ccc12 = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_s,        8, 35,  42,  8, 0 };
_transfer Ctrl ccc13 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s1,      54, 35,  30,  8, 0 };
_transfer Ctrl ccc14 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s2,      86, 35,  44,  8, 0 };
_transfer Ctrl ccc15 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s3,     132, 35,  30,  8, 0 };
_transfer Ctrl ccc16 = { 10, C_BUTTON, -1, (unsigned short)"OK",             88, 70,  32, 12, 0 };
_transfer Ctrl ccc17 = { 11, C_BUTTON, -1, (unsigned short)"Cancel",        128, 70,  52, 12, 0 };

_transfer Ctrl_Group cfgcg;
_transfer Window     cfgwin;
_transfer char       cfg_title[10] = { 'F','r','a','c','t','a','l','i','c',0 };

_transfer Ctrl       anim_ctrl[1];
_transfer Ctrl_Group anim_cg;
_transfer Window     anim_win;
_transfer char       empty_str[1];

static void cfg_open(void)
{
    if (cfgwin_id >= 0) return;

    tmp_type  = cfgdat[4];
    tmp_depth = cfgdat[5];
    tmp_speed = cfgdat[6];

    rg_type[0]  = rg_type[1]  = rg_type[2]  = rg_type[3]  = rg_type[4] = rg_type[5] = -1;
    rg_depth[0] = rg_depth[1] = rg_depth[2] = rg_depth[3] = -1;
    rg_speed[0] = rg_speed[1] = rg_speed[2] = rg_speed[3] = -1;

    memset(&cfgcg, 0, sizeof(cfgcg));
    cfgcg.controls = 18;
    cfgcg.pid      = _sympid;
    cfgcg.first    = &ccc0;

    memset(&cfgwin, 0, sizeof(cfgwin));
    cfgwin.state    = WIN_NORMAL;
    cfgwin.flags    = WIN_TITLE | WIN_CENTERED | WIN_NOTTASKBAR;
    cfgwin.pid      = _sympid;
    cfgwin.w        = 280;
    cfgwin.h        = 86;
    cfgwin.wfull    = 280;
    cfgwin.hfull    = 86;
    cfgwin.wmin     = 280;
    cfgwin.hmin     = 86;
    cfgwin.wmax     = 280;
    cfgwin.hmax     = 86;
    cfgwin.title    = cfg_title;
    cfgwin.controls = &cfgcg;

    cfgwin_id = Win_Open(_symbank, &cfgwin);
}

static void cfg_close(void)
{
    if (cfgwin_id < 0) return;
    Win_Close((unsigned char)cfgwin_id);
    cfgwin_id = -1;
}

static void cfg_ok(void)
{
    cfgdat[4] = tmp_type;
    cfgdat[5] = tmp_depth;
    cfgdat[6] = tmp_speed;
    cfg_close();
    if (cfg_prz) {
        _symmsg[0] = MSR_SAV_CONFIG;
        _symmsg[1] = _symbank;
        _symmsg[2] = (char)((unsigned short)cfgdat & 0xFF);
        _symmsg[3] = (char)((unsigned short)cfgdat >> 8);
        while (!Msg_Send(_sympid, cfg_prz, _symmsg));
        cfg_prz = 0;
    }
}

static void cfg_cancel(void)
{
    cfg_close();
    cfg_prz = 0;
}

// ---------------------------------------------------------------------------
// Animation entry point
// ---------------------------------------------------------------------------

void start_animation(void)
{
    signed char    wid;
    unsigned char  speed, b;
    unsigned short mx0, my0;
    unsigned short resp;

    // Detect platform and set screen dimensions
    is_msx = ((Sys_Type() & TYPE_MSX) != 0) ? 1 : 0;
    if (is_msx) {
        screen_w = MSX_W;
        screen_h = MSX_H;
    } else {
        screen_w = SCREEN_W;
        screen_h = SCREEN_H;
    }

    frac_cfg_type = (unsigned char)cfgdat[4];
    frac_depth    = (unsigned char)cfgdat[5];
    speed         = (unsigned char)cfgdat[6];

    if (frac_cfg_type > 4) frac_cfg_type = 1;  // 0 = Random is valid
    if (frac_depth < 1 || frac_depth > 3) frac_depth = 2;
    if (speed < 1 || speed > 3) speed = 2;
    frac_speed = speed;

    // First run: if Random, pick a type now; otherwise use configured type
    if (frac_cfg_type == 0)
        frac_type = (unsigned char)(1 + rand() % 4);
    else
        frac_type = frac_cfg_type;

    // 5-second pause = ~300 ticks (each Idle ≈ 1/60 s)
    anim_pause = 300;

    srand((unsigned int)Sys_Counter());

    memset(zero_plane, 0xF0, sizeof(zero_plane));

    empty_str[0] = 0;

    anim_ctrl[0].value  = 0;
    anim_ctrl[0].type   = C_AREA;
    anim_ctrl[0].bank   = -1;
    anim_ctrl[0].param  = AREA_16COLOR | COLOR_BLACK;
    anim_ctrl[0].x      = 0;
    anim_ctrl[0].y      = 0;
    anim_ctrl[0].w      = screen_w;
    anim_ctrl[0].h      = screen_h;
    anim_ctrl[0].unused = 0;

    memset(&anim_cg, 0, sizeof(anim_cg));
    anim_cg.controls = 1;
    anim_cg.pid      = _sympid;
    anim_cg.first    = &anim_ctrl[0];

    memset(&anim_win, 0, sizeof(anim_win));
    anim_win.state    = WIN_NORMAL;
    anim_win.flags    = WIN_NOTTASKBAR | WIN_NOTMOVEABLE;
    anim_win.pid      = _sympid;
    anim_win.w        = screen_w;
    anim_win.h        = screen_h;
    anim_win.wfull    = screen_w;
    anim_win.hfull    = screen_h;
    anim_win.wmin     = 32;
    anim_win.hmin     = 24;
    anim_win.wmax     = screen_w;
    anim_win.hmax     = screen_h;
    anim_win.title    = empty_str;
    anim_win.status   = empty_str;
    anim_win.controls = &anim_cg;

    wid = Win_Open(_symbank, &anim_win);
    if (wid < 0) return;

    desktop_stop((unsigned char)wid);
    vram_clear();

    Idle();
    // Restore bottom char row (taskbar area) — CPC only
    if (!is_msx) {
        for (b = 0; b < 8; b++)
            Bank_Copy(0, (char *)(0xC000u + (unsigned short)b * 0x0800u + 1920u),
                      _symbank, (char *)zero_plane, 80u);
    }

    fractal_init();
    anim_stage = 0;

    mx0 = Mouse_X();
    my0 = Mouse_Y();

    while (1) {
        if (Mouse_X()    != mx0 ||
            Mouse_Y()    != my0 ||
            Mouse_Buttons()     ||
            any_key_down()) {

            desktop_cont();
            Idle();
            Win_Close((unsigned char)wid);
            Screen_Redraw();
            return;
        }

        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 1) {
            if (_symmsg[0] == 0) {
                desktop_cont();
                Win_Close((unsigned char)wid);
                exit(0);
            }
        }

        anim_tick();

        Idle();
        if (!is_msx) {
            for (b = 0; b < 8; b++)
                Bank_Copy(0, (char *)(0xC000u + (unsigned short)b * 0x0800u + 1920u),
                          _symbank, (char *)zero_plane, 80u);
        }
    }
}

// ---------------------------------------------------------------------------
// Main — screensaver protocol
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    unsigned short resp;
    unsigned char  got_msg, sender, b;

    cfgdat[0] = 'F'; cfgdat[1] = 'R'; cfgdat[2] = 'A'; cfgdat[3] = 'C';
    cfgdat[4] = 1;   // type: Sierpinski
    cfgdat[5] = 2;   // depth: medium
    cfgdat[6] = 2;   // speed: normal

    got_msg = 0;
    sender  = 0;

    for (b = 0; b < 10; b++) {
        Idle();
        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 0x01) {
            got_msg = 1;
            sender  = (unsigned char)(resp >> 8);
            break;
        }
    }

    if (!got_msg) {
        start_animation();
        exit(0);
    }

    while (1) {
        switch (_symmsg[0]) {

        case 0:
            exit(0);

        case MSC_SAV_INIT:
            Bank_Copy(
                _symbank, init_tmp,
                (unsigned char)_symmsg[1],
                (char *)((unsigned short)((unsigned char)_symmsg[3] << 8)
                         | (unsigned char)_symmsg[2]),
                64u);
            if (init_tmp[0] == 'F' && init_tmp[1] == 'R' &&
                init_tmp[2] == 'A' && init_tmp[3] == 'C') {
                memcpy(cfgdat, init_tmp, 64);
            }
            break;

        case MSC_SAV_START:
            start_animation();
            break;

        case MSC_SAV_CONFIG:
            cfg_prz = sender;
            cfg_open();
            break;

        default:
            if ((unsigned char)_symmsg[0] == MSR_DSK_WCLICK &&
                cfgwin_id >= 0 &&
                (unsigned char)_symmsg[1] == (unsigned char)cfgwin_id) {

                if ((unsigned char)_symmsg[2] == DSK_ACT_CLOSE) {
                    cfg_cancel();
                } else if ((unsigned char)_symmsg[2] == DSK_ACT_CONTENT) {
                    if ((unsigned char)_symmsg[8] == 10)
                        cfg_ok();
                    else if ((unsigned char)_symmsg[8] == 11)
                        cfg_cancel();
                }
            }
            break;
        }

        do {
            resp = Msg_Sleep(_sympid, -1, _symmsg);
        } while (!(resp & 0x01));

        sender = (unsigned char)(resp >> 8);
    }
}
