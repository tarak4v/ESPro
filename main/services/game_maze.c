/**
 * @file game_maze.c
 * @brief Tilt Marble Maze — accelerometer-driven ball maze game.
 *
 * 640×172 elongated maze.  Tilt the device to roll a marble from
 * the left start zone to the right goal zone, avoiding holes.
 * 5 levels of increasing difficulty. Best times stored in SPIFFS.
 *
 * Physics tick runs inside screen_menu_update() (~100 ms cadence).
 */

#include "game_maze.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "maze";

/* ── Display geometry ─────────────────────────────────────── */
#define GW             LCD_H_RES        /* 640 */
#define GH             LCD_V_RES        /* 172 */

/* ── Ball ─────────────────────────────────────────────────── */
#define BALL_R         6
#define MAX_VEL        6.0f
#define FRICTION       0.92f
#define ACCEL_SCALE    3.5f             /* tilt sensitivity */

/* ── Maze elements ────────────────────────────────────────── */
#define MAX_WALLS      30
#define MAX_HOLES      10
#define NUM_LEVELS     5
#define BEST_TIME_FILE "/log/maze_best.dat"

typedef struct { int16_t x, y, w, h; }   wall_t;
typedef struct { int16_t cx, cy, r; }      hole_t;

typedef struct {
    wall_t  walls[MAX_WALLS];
    int     wall_count;
    hole_t  holes[MAX_HOLES];
    int     hole_count;
    int16_t start_x, start_y;
    int16_t goal_x,  goal_y, goal_r;
} level_t;

/* ── Game state ───────────────────────────────────────────── */
typedef enum { ST_MENU, ST_PLAY, ST_WIN, ST_FALL } game_state_t;

static bool          s_active = false;
static lv_obj_t     *s_overlay = NULL;

/* Play state */
static game_state_t  s_state   = ST_MENU;
static int           s_level   = 0;       /* 0-based */
static level_t       s_lvl;
static float         bx, by;             /* ball centre */
static float         vx, vy;             /* velocity     */
static int64_t       s_start_us = 0;
static int32_t       s_elapsed_ms = 0;
static int32_t       s_best_ms[NUM_LEVELS];

/* LVGL objects (play) */
static lv_obj_t     *ball_obj   = NULL;
static lv_obj_t     *timer_lbl  = NULL;
static lv_obj_t     *level_lbl  = NULL;
static lv_obj_t     *msg_lbl    = NULL;
static lv_obj_t     *goal_obj   = NULL;
static lv_obj_t     *wall_objs[MAX_WALLS];
static lv_obj_t     *hole_objs[MAX_HOLES];

/* LVGL objects (menu) */
static lv_obj_t     *menu_cont  = NULL;

/* ================================================================
 *  Best-time persistence (SPIFFS)
 * ================================================================ */
static void load_best_times(void)
{
    for (int i = 0; i < NUM_LEVELS; i++) s_best_ms[i] = 0;
    FILE *f = fopen(BEST_TIME_FILE, "rb");
    if (f) {
        fread(s_best_ms, sizeof(int32_t), NUM_LEVELS, f);
        fclose(f);
    }
}

static void save_best_times(void)
{
    FILE *f = fopen(BEST_TIME_FILE, "wb");
    if (f) {
        fwrite(s_best_ms, sizeof(int32_t), NUM_LEVELS, f);
        fclose(f);
    }
}

/* ================================================================
 *  Level generation (procedural, seeded by level index)
 * ================================================================ */

/* Simple deterministic pseudo-RNG seeded per level */
static uint32_t s_rng;
static void     rng_seed(uint32_t s) { s_rng = s; }
static uint32_t rng_next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static int rng_range(int lo, int hi) { return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1)); }

static void generate_level(int idx, level_t *l)
{
    memset(l, 0, sizeof(*l));

    l->start_x = 20;
    l->start_y = GH / 2;
    l->goal_x  = GW - 25;
    l->goal_y  = GH / 2;
    l->goal_r  = 12;

    /* Border walls (top, bottom) */
    l->walls[0] = (wall_t){ 0,       0,       GW, 4 };            /* top */
    l->walls[1] = (wall_t){ 0,       GH - 4,  GW, 4 };           /* bottom */
    l->wall_count = 2;

    rng_seed(42 + idx * 137);

    /* Number of vertical barrier walls scales with level */
    int n_barriers = 3 + idx;          /* L0=3, L1=4, ... L4=7 */
    if (n_barriers > 12) n_barriers = 12;

    int section_w = (GW - 80) / n_barriers;    /* spacing between barriers */

    for (int i = 0; i < n_barriers && l->wall_count < MAX_WALLS - 1; i++) {
        int wx = 60 + i * section_w + rng_range(-10, 10);
        if (wx < 50) wx = 50;
        if (wx > GW - 50) wx = GW - 50;

        /* Each barrier has a gap — gap position varies */
        int gap_y  = rng_range(20, GH - 40);
        int gap_h  = 40 - idx * 4;       /* gap shrinks with difficulty */
        if (gap_h < 22) gap_h = 22;

        /* Top segment */
        if (gap_y > 6) {
            l->walls[l->wall_count++] = (wall_t){ (int16_t)wx, 4,
                 6, (int16_t)(gap_y - 4) };
        }
        /* Bottom segment */
        int bot_y = gap_y + gap_h;
        if (bot_y < GH - 6) {
            l->walls[l->wall_count++] = (wall_t){ (int16_t)wx, (int16_t)bot_y,
                 6, (int16_t)(GH - 4 - bot_y) };
        }
    }

    /* Holes (traps) — more with higher levels */
    int n_holes = 1 + idx;             /* L0=1 … L4=5 */
    if (n_holes > MAX_HOLES) n_holes = MAX_HOLES;

    for (int i = 0; i < n_holes; i++) {
        int hx = rng_range(80, GW - 80);
        int hy = rng_range(20, GH - 20);
        /* Make sure hole doesn't overlap start/goal zones */
        if (abs(hx - l->start_x) < 40 && abs(hy - l->start_y) < 30) hx += 60;
        if (abs(hx - l->goal_x)  < 40 && abs(hy - l->goal_y)  < 30) hx -= 60;
        l->holes[l->hole_count++] = (hole_t){ (int16_t)hx, (int16_t)hy, 8 };
    }
}

/* ================================================================
 *  IMU read (accelerometer X/Y for tilt)
 * ================================================================ */
static void read_accel(float *ax_g, float *ay_g)
{
    uint8_t buf[6];
    *ax_g = 0.0f;
    *ay_g = 0.0f;
    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) != 0) return;

    int16_t ax_raw = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t ay_raw = (int16_t)((buf[3] << 8) | buf[2]);

    /* ±8 g scale factor = 4096 LSB/g */
    *ax_g = ax_raw / 4096.0f;
    *ay_g = ay_raw / 4096.0f;
}

/* ================================================================
 *  Collision helpers
 * ================================================================ */
static bool ball_wall_collide(float px, float py, const wall_t *w,
                              float *nx, float *ny)
{
    /* Closest point on AABB to ball centre */
    float cx = px, cy = py;
    if (cx < w->x)           cx = w->x;
    if (cx > w->x + w->w)    cx = w->x + w->w;
    if (cy < w->y)           cy = w->y;
    if (cy > w->y + w->h)    cy = w->y + w->h;

    float dx = px - cx,  dy = py - cy;
    float dist2 = dx * dx + dy * dy;
    float r = BALL_R;
    if (dist2 < r * r && dist2 > 0.001f) {
        float d = sqrtf(dist2);
        *nx = dx / d;
        *ny = dy / d;
        return true;
    }
    return false;
}

static bool ball_in_circle(float px, float py, int16_t cx, int16_t cy, int16_t r)
{
    float dx = px - cx, dy = py - cy;
    return (dx * dx + dy * dy) < (float)(r * r);
}

/* ================================================================
 *  UI builders
 * ================================================================ */
static void destroy_play_objs(void)
{
    ball_obj = NULL;  timer_lbl = NULL;  level_lbl = NULL;
    msg_lbl  = NULL;  goal_obj  = NULL;
    memset(wall_objs, 0, sizeof(wall_objs));
    memset(hole_objs, 0, sizeof(hole_objs));
}

static void build_play_ui(void)
{
    /* Clear overlay children */
    lv_obj_clean(s_overlay);
    destroy_play_objs();

    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0D1117), 0);

    /* Draw walls */
    for (int i = 0; i < s_lvl.wall_count; i++) {
        wall_t *w = &s_lvl.walls[i];
        lv_obj_t *o = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, w->x, w->y);
        lv_obj_set_size(o, w->w, w->h);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x3A5A8C), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 2, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        wall_objs[i] = o;
    }

    /* Draw holes (traps) */
    for (int i = 0; i < s_lvl.hole_count; i++) {
        hole_t *h = &s_lvl.holes[i];
        lv_obj_t *o = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, h->r * 2, h->r * 2);
        lv_obj_set_pos(o, h->cx - h->r, h->cy - h->r);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x220000), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0xCC2222), 0);
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        hole_objs[i] = o;
    }

    /* Goal zone */
    goal_obj = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(goal_obj);
    lv_obj_set_size(goal_obj, s_lvl.goal_r * 2, s_lvl.goal_r * 2);
    lv_obj_set_pos(goal_obj, s_lvl.goal_x - s_lvl.goal_r,
                             s_lvl.goal_y - s_lvl.goal_r);
    lv_obj_set_style_bg_color(goal_obj, lv_color_hex(0x00AA44), 0);
    lv_obj_set_style_bg_opa(goal_obj, LV_OPA_70, 0);
    lv_obj_set_style_radius(goal_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(goal_obj, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_border_width(goal_obj, 2, 0);
    lv_obj_clear_flag(goal_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Ball */
    ball_obj = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(ball_obj);
    lv_obj_set_size(ball_obj, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_bg_color(ball_obj, lv_color_hex(0xFFDD44), 0);
    lv_obj_set_style_bg_opa(ball_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ball_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(ball_obj, 8, 0);
    lv_obj_set_style_shadow_color(ball_obj, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_shadow_opa(ball_obj, LV_OPA_60, 0);
    lv_obj_clear_flag(ball_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(ball_obj, (int16_t)(bx - BALL_R), (int16_t)(by - BALL_R));

    /* Timer label (top-centre) */
    timer_lbl = lv_label_create(s_overlay);
    lv_label_set_text(timer_lbl, "0.0s");
    lv_obj_set_style_text_font(timer_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(timer_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(timer_lbl, LV_ALIGN_TOP_MID, 0, 2);

    /* Level label (top-left) */
    level_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(level_lbl, "Lv %d", s_level + 1);
    lv_obj_set_style_text_font(level_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(level_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(level_lbl, LV_ALIGN_TOP_LEFT, 6, 2);

    /* Message label (centre, hidden initially) */
    msg_lbl = lv_label_create(s_overlay);
    lv_label_set_text(msg_lbl, "");
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(msg_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(msg_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ── Start a level ────────────────────────────────────────── */
static void start_level(int idx)
{
    s_level = idx;
    generate_level(idx, &s_lvl);
    bx = s_lvl.start_x;
    by = s_lvl.start_y;
    vx = vy = 0.0f;
    s_state = ST_PLAY;
    s_start_us = esp_timer_get_time();
    s_elapsed_ms = 0;
    build_play_ui();
    ESP_LOGI(TAG, "Level %d started (%d walls, %d holes)",
             idx + 1, s_lvl.wall_count, s_lvl.hole_count);
}

/* ================================================================
 *  Level-select menu overlay
 * ================================================================ */
static void level_btn_cb(lv_event_t *e)
{
    int lvl = (int)(uintptr_t)lv_event_get_user_data(e);
    start_level(lvl);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    game_maze_close();
}

static void build_menu_ui(void)
{
    lv_obj_clean(s_overlay);
    destroy_play_objs();
    menu_cont = NULL;

    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0A0A14), 0);

    /* Back button */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, LV_SYMBOL_PLAY "  Tilt Maze");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(s_overlay);
    lv_label_set_text(sub, "Tilt device to roll ball\nAvoid red holes, reach green goal");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 26);

    /* Level buttons row */
    lv_obj_t *row = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 560, 80);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NUM_LEVELS; i++) {
        lv_obj_t *card = lv_btn_create(row);
        lv_obj_set_size(card, 96, 70);
        uint32_t col = (i == 0) ? 0x1B5E20 :
                       (i == 1) ? 0x33691E :
                       (i == 2) ? 0x827717 :
                       (i == 3) ? 0xBF360C : 0x880E4F;
        lv_obj_set_style_bg_color(card, lv_color_hex(col), 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_add_event_cb(card, level_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        /* Level number */
        lv_obj_t *num = lv_label_create(card);
        lv_label_set_text_fmt(num, "Lv %d", i + 1);
        lv_obj_set_style_text_font(num, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(num, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(num, LV_ALIGN_TOP_MID, 0, -2);

        /* Best time (if set) */
        lv_obj_t *best = lv_label_create(card);
        if (s_best_ms[i] > 0) {
            int sec = s_best_ms[i] / 1000;
            int dec = (s_best_ms[i] % 1000) / 100;
            lv_label_set_text_fmt(best, "Best: %d.%ds", sec, dec);
        } else {
            lv_label_set_text(best, "---");
        }
        lv_obj_set_style_text_font(best, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(best, lv_color_hex(0xCCCCCC), 0);
        lv_obj_align(best, LV_ALIGN_BOTTOM_MID, 0, 2);

        /* Stars (difficulty) */
        lv_obj_t *stars = lv_label_create(card);
        char star_buf[12] = "";
        for (int s = 0; s <= i && s < 5; s++) strcat(star_buf, "*");
        lv_label_set_text(stars, star_buf);
        lv_obj_set_style_text_font(stars, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(stars, lv_color_hex(0xFFCC00), 0);
        lv_obj_align(stars, LV_ALIGN_CENTER, 0, 2);
    }

    menu_cont = row;
    s_state = ST_MENU;
}

/* ================================================================
 *  Win / fall result screen (tap to continue)
 * ================================================================ */
static void result_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_WIN) {
        /* Advance to next level, or back to menu if last */
        if (s_level + 1 < NUM_LEVELS)
            start_level(s_level + 1);
        else
            build_menu_ui();
    } else {
        /* Restart same level on fall */
        start_level(s_level);
    }
}

static void show_result(const char *text, uint32_t color)
{
    if (msg_lbl) {
        lv_label_set_text(msg_lbl, text);
        lv_obj_set_style_text_color(msg_lbl, lv_color_hex(color), 0);
        lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    /* Make overlay clickable for tap-to-continue */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, result_tap_cb, LV_EVENT_CLICKED, NULL);
}

/* ================================================================
 *  Public API
 * ================================================================ */
void game_maze_open(lv_obj_t *parent)
{
    if (s_active) return;

    load_best_times();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, GW, GH);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_active = true;
    build_menu_ui();

    ESP_LOGI(TAG, "Maze game opened");
}

void game_maze_close(void)
{
    if (!s_active) return;
    destroy_play_objs();
    menu_cont = NULL;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_state  = ST_MENU;
    ESP_LOGI(TAG, "Maze game closed");
}

bool game_maze_is_active(void)
{
    return s_active;
}

/* ================================================================
 *  Physics / game tick  (~100 ms from screen_menu_update)
 * ================================================================ */
void game_maze_update(void)
{
    if (!s_active || s_state != ST_PLAY) return;

    /* ── Read accelerometer ───────────────────────────────── */
    float ax_g, ay_g;
    read_accel(&ax_g, &ay_g);

    /*
     * Device orientation (landscape, USB on right):
     *   Same axis mapping as Traffic Rider:
     *   screen-X ← accel-X (ax_g)
     *   screen-Y ← accel-Y (ay_g) inverted
     */
    float tilt_x = ax_g * ACCEL_SCALE;
    float tilt_y = -ay_g * ACCEL_SCALE;

    /* ── Update velocity ──────────────────────────────────── */
    vx = (vx + tilt_x) * FRICTION;
    vy = (vy + tilt_y) * FRICTION;

    /* Clamp */
    if (vx >  MAX_VEL) vx =  MAX_VEL;
    if (vx < -MAX_VEL) vx = -MAX_VEL;
    if (vy >  MAX_VEL) vy =  MAX_VEL;
    if (vy < -MAX_VEL) vy = -MAX_VEL;

    /* ── Tentative new position ───────────────────────────── */
    float nx = bx + vx;
    float ny = by + vy;

    /* ── Wall collisions ──────────────────────────────────── */
    for (int i = 0; i < s_lvl.wall_count; i++) {
        float wnx, wny;
        if (ball_wall_collide(nx, ny, &s_lvl.walls[i], &wnx, &wny)) {
            /* Push ball out of wall */
            float overlap = BALL_R - sqrtf(
                (nx - (nx - wnx * BALL_R)) * (nx - (nx - wnx * BALL_R)) +
                (ny - (ny - wny * BALL_R)) * (ny - (ny - wny * BALL_R)));
            nx += wnx * (BALL_R * 0.5f);
            ny += wny * (BALL_R * 0.5f);
            (void)overlap;

            /* Reflect velocity along collision normal */
            float dot = vx * wnx + vy * wny;
            vx -= 1.8f * dot * wnx;
            vy -= 1.8f * dot * wny;
            vx *= 0.5f;
            vy *= 0.5f;
        }
    }

    /* ── Boundary clamp ───────────────────────────────────── */
    if (nx < BALL_R + 4)      { nx = BALL_R + 4;      vx = -vx * 0.3f; }
    if (nx > GW - BALL_R - 4) { nx = GW - BALL_R - 4; vx = -vx * 0.3f; }
    if (ny < BALL_R + 4)      { ny = BALL_R + 4;      vy = -vy * 0.3f; }
    if (ny > GH - BALL_R - 4) { ny = GH - BALL_R - 4; vy = -vy * 0.3f; }

    bx = nx;
    by = ny;

    /* ── Hole check (fall) ─────────────────────────────────── */
    for (int i = 0; i < s_lvl.hole_count; i++) {
        float speed = sqrtf(vx * vx + vy * vy);
        float effective_r = s_lvl.holes[i].r * 0.6f;
        if (speed < 2.0f && ball_in_circle(bx, by,
                s_lvl.holes[i].cx, s_lvl.holes[i].cy, effective_r)) {
            s_state = ST_FALL;
            show_result("FELL!  Tap to retry", 0xFF4444);
            ESP_LOGI(TAG, "Ball fell in hole at (%d,%d)",
                     s_lvl.holes[i].cx, s_lvl.holes[i].cy);
            return;
        }
    }

    /* ── Goal check ───────────────────────────────────────── */
    if (ball_in_circle(bx, by, s_lvl.goal_x, s_lvl.goal_y, s_lvl.goal_r)) {
        s_state = ST_WIN;
        s_elapsed_ms = (int32_t)((esp_timer_get_time() - s_start_us) / 1000);

        bool new_best = false;
        if (s_best_ms[s_level] == 0 || s_elapsed_ms < s_best_ms[s_level]) {
            s_best_ms[s_level] = s_elapsed_ms;
            save_best_times();
            new_best = true;
        }

        char buf[64];
        int sec = s_elapsed_ms / 1000;
        int dec = (s_elapsed_ms % 1000) / 100;
        if (new_best)
            snprintf(buf, sizeof(buf), "CLEAR!  %d.%ds  NEW BEST!\nTap to continue", sec, dec);
        else
            snprintf(buf, sizeof(buf), "CLEAR!  %d.%ds\nTap to continue", sec, dec);

        show_result(buf, 0x00FF88);
        ESP_LOGI(TAG, "Level %d cleared in %d.%ds", s_level + 1, sec, dec);
        return;
    }

    /* ── Update ball position on screen ───────────────────── */
    if (ball_obj)
        lv_obj_set_pos(ball_obj, (int16_t)(bx - BALL_R), (int16_t)(by - BALL_R));

    /* ── Update timer ─────────────────────────────────────── */
    s_elapsed_ms = (int32_t)((esp_timer_get_time() - s_start_us) / 1000);
    if (timer_lbl) {
        int sec = s_elapsed_ms / 1000;
        int dec = (s_elapsed_ms % 1000) / 100;
        lv_label_set_text_fmt(timer_lbl, "%d.%ds", sec, dec);
    }
}
