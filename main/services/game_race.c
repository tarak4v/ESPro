/**
 * @file game_race.c
 * @brief Traffic Rider - tilt-to-steer bike racing on 640x172.
 *
 * Perspective: first-person highway view.  Road scrolls toward the
 * player (stripes move downward).  Bike is centred horizontally,
 * near the bottom.  Tilt left/right to weave between oncoming
 * traffic.  Speed increases over time.  High score saved to LittleFS.
 *
 * Physics tick runs inside screen_menu_update() (~100 ms cadence).
 */

#include "game_race.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "screen_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "race";

/* -- Display geometry -------------------------------------------------- */
#define GW             LCD_H_RES        /* 640 */
#define GH             LCD_V_RES        /* 172 */

/* -- Road layout: centred vertical highway ----------------------------- */
#define ROAD_LEFT      120              /* road left edge (grass/shoulder) */
#define ROAD_RIGHT     520              /* road right edge */
#define ROAD_W         (ROAD_RIGHT - ROAD_LEFT)   /* 400 px */
#define LANE_COUNT     4
#define LANE_W         (ROAD_W / LANE_COUNT)       /* 100 px per lane */
#define LANE_CX(n)     (ROAD_LEFT + LANE_W / 2 + LANE_W * (n))

/* -- Player bike ------------------------------------------------------- */
#define BIKE_W         14               /* narrow bike body */
#define BIKE_H         32               /* taller than wide */
#define BIKE_Y         (GH - 50)        /* fixed Y near bottom */
#define STEER_SPEED    5.0f             /* tilt sensitivity */

/* -- Traffic (obstacles) ----------------------------------------------- */
#define MAX_OBS        10
#define OBS_W          22               /* car/truck width */
#define OBS_H_MIN      30               /* short car */
#define OBS_H_MAX      44               /* long truck */
#define OBS_START_SPD  3.5f             /* initial scroll speed */
#define OBS_MAX_SPD    12.0f
#define OBS_ACCEL      0.004f           /* speed ramp per tick */

/* -- Road centre stripes ----------------------------------------------- */
#define STRIPE_ROWS    6
#define STRIPE_W       3
#define STRIPE_H       18
#define STRIPE_GAP     (GH / STRIPE_ROWS)

/* -- Shoulder rumble strips -------------------------------------------- */
#define RUMBLE_COUNT   6
#define RUMBLE_W       8
#define RUMBLE_H       6

/* -- High score file --------------------------------------------------- */
#define HISCORE_FILE   "/log/race_best.dat"

/* -- Game states -------------------------------------------------------- */
typedef enum { RS_READY, RS_PLAY, RS_OVER } race_state_t;

typedef struct {
    float x, y;
    int   h;              /* per-obstacle height (car vs truck) */
    bool  active;
    uint32_t color;
} obstacle_t;

/* -- Static state ------------------------------------------------------- */
static bool          s_active = false;
static lv_obj_t     *s_overlay = NULL;
static race_state_t  s_state = RS_READY;

/* Player */
static float         s_bike_x;          /* centre X */

/* Traffic */
static obstacle_t    s_obs[MAX_OBS];
static float         s_speed;
static int           s_next_spawn;

/* Score */
static int           s_score;
static int           s_hiscore;
static int64_t       s_last_tick_us;

/* Stripe scroll */
static float         s_stripe_off;

/* -- LVGL objects ------------------------------------------------------- */
static lv_obj_t     *s_bike_body    = NULL;
static lv_obj_t     *s_bike_wheel_f = NULL;
static lv_obj_t     *s_bike_wheel_r = NULL;
static lv_obj_t     *s_obs_objs[MAX_OBS];
static lv_obj_t     *s_score_lbl  = NULL;
static lv_obj_t     *s_hi_lbl     = NULL;
static lv_obj_t     *s_speed_lbl  = NULL;
static lv_obj_t     *s_msg_lbl    = NULL;
static lv_obj_t     *s_stripe_objs[(LANE_COUNT - 1) * STRIPE_ROWS];
static lv_obj_t     *s_rumble_l[RUMBLE_COUNT];
static lv_obj_t     *s_rumble_r[RUMBLE_COUNT];

/* -- Simple xorshift RNG ---------------------------------------------- */
static uint32_t s_rng = 12345;
static void rng_seed(uint32_t s) { s_rng = s ? s : 1; }
static uint32_t rng_next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static int rng_range(int lo, int hi)
{
    return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}

/* -- High score persistence ------------------------------------------- */
static void load_hiscore(void)
{
    s_hiscore = 0;
    FILE *f = fopen(HISCORE_FILE, "rb");
    if (f) {
        fread(&s_hiscore, sizeof(int), 1, f);
        fclose(f);
    }
}

static void save_hiscore(void)
{
    FILE *f = fopen(HISCORE_FILE, "wb");
    if (f) {
        fwrite(&s_hiscore, sizeof(int), 1, f);
        fclose(f);
    }
}

/* -- IMU read --------------------------------------------------------- */
static void read_accel(float *ax_g, float *ay_g)
{
    uint8_t buf[6];
    *ax_g = 0.0f;
    *ay_g = 0.0f;
    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) != 0) return;
    int16_t ax_raw = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t ay_raw = (int16_t)((buf[3] << 8) | buf[2]);
    *ax_g = ax_raw / 4096.0f;
    *ay_g = ay_raw / 4096.0f;
}

/* -- Traffic colours -------------------------------------------------- */
static const uint32_t obs_colors[] = {
    0xE53935, 0x1E88E5, 0x43A047, 0xFDD835,
    0x8E24AA, 0xF4511E, 0x00ACC1, 0x6D4C41,
};
#define OBS_COLOR_COUNT (sizeof(obs_colors) / sizeof(obs_colors[0]))

/* -- Spawn traffic (top of road, random lane) ------------------------- */
static void spawn_obstacle(void)
{
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].active) {
            int lane = rng_range(0, LANE_COUNT - 1);
            int h = rng_range(OBS_H_MIN, OBS_H_MAX);
            s_obs[i].x = (float)(LANE_CX(lane) - OBS_W / 2);
            s_obs[i].y = (float)(-h - 4);
            s_obs[i].h = h;
            s_obs[i].active = true;
            s_obs[i].color = obs_colors[rng_next() % OBS_COLOR_COUNT];

            if (s_obs_objs[i]) {
                lv_obj_set_size(s_obs_objs[i], OBS_W, h);
                lv_obj_set_style_bg_color(s_obs_objs[i],
                    lv_color_hex(s_obs[i].color), 0);
                lv_obj_clear_flag(s_obs_objs[i], LV_OBJ_FLAG_HIDDEN);
            }
            break;
        }
    }
    int gap = 18 - (int)(s_speed * 1.2f);
    if (gap < 4) gap = 4;
    s_next_spawn = rng_range(gap, gap + 6);
}

/* -- Collision detect (AABB bike vs obstacle) ------------------------- */
static bool check_collision(void)
{
    float bx = s_bike_x  - BIKE_W / 2.0f;
    float by = (float)BIKE_Y - BIKE_H / 2.0f;

    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].active) continue;
        float ox = s_obs[i].x;
        float oy = s_obs[i].y;
        int   oh = s_obs[i].h;
        if (bx + BIKE_W - 3 > ox + 3 &&
            bx + 3 < ox + OBS_W - 3 &&
            by + BIKE_H - 3 > oy + 3 &&
            by + 3 < oy + oh - 3) {
            return true;
        }
    }
    return false;
}

/* -- Helper: create a simple rect obj --------------------------------- */
static lv_obj_t *make_rect(lv_obj_t *parent, int w, int h,
                            uint32_t color, lv_opa_t opa)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, opa, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* -- Destroy pointers ------------------------------------------------- */
static void destroy_play_objs(void)
{
    s_bike_body = NULL;
    s_bike_wheel_f = NULL;
    s_bike_wheel_r = NULL;
    s_score_lbl = NULL;
    s_hi_lbl = NULL;
    s_speed_lbl = NULL;
    s_msg_lbl = NULL;
    memset(s_obs_objs, 0, sizeof(s_obs_objs));
    memset(s_stripe_objs, 0, sizeof(s_stripe_objs));
    memset(s_rumble_l, 0, sizeof(s_rumble_l));
    memset(s_rumble_r, 0, sizeof(s_rumble_r));
}

/* -- Build gameplay UI (vertical road, bike at bottom) ---------------- */
static void build_play_ui(void)
{
    lv_obj_clean(s_overlay);
    destroy_play_objs();

    /* Grass background */
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1B5E20), 0);

    /* Road surface (dark asphalt) */
    lv_obj_t *road = make_rect(s_overlay, ROAD_W, GH, 0x37474F, LV_OPA_COVER);
    lv_obj_set_pos(road, ROAD_LEFT, 0);

    /* Road edge lines (white) */
    lv_obj_t *el = make_rect(s_overlay, 3, GH, 0xFFFFFF, LV_OPA_70);
    lv_obj_set_pos(el, ROAD_LEFT - 2, 0);
    lv_obj_t *er = make_rect(s_overlay, 3, GH, 0xFFFFFF, LV_OPA_70);
    lv_obj_set_pos(er, ROAD_RIGHT, 0);

    /* Rumble strips (red/white alternating on shoulders) */
    for (int i = 0; i < RUMBLE_COUNT; i++) {
        int ry = i * (GH / RUMBLE_COUNT);
        uint32_t c = (i % 2 == 0) ? 0xFF0000 : 0xFFFFFF;
        s_rumble_l[i] = make_rect(s_overlay, RUMBLE_W, RUMBLE_H, c, LV_OPA_80);
        lv_obj_set_pos(s_rumble_l[i], ROAD_LEFT - RUMBLE_W - 3, ry);
        s_rumble_r[i] = make_rect(s_overlay, RUMBLE_W, RUMBLE_H, c, LV_OPA_80);
        lv_obj_set_pos(s_rumble_r[i], ROAD_RIGHT + 4, ry);
    }

    /* Lane divider stripes (dashed vertical lines scrolling down) */
    int si = 0;
    for (int lane = 1; lane < LANE_COUNT; lane++) {
        int lx = ROAD_LEFT + lane * LANE_W - STRIPE_W / 2;
        for (int j = 0; j < STRIPE_ROWS; j++) {
            lv_obj_t *s = make_rect(s_overlay, STRIPE_W, STRIPE_H,
                                     0xBDBDBD, LV_OPA_70);
            lv_obj_set_pos(s, lx, j * STRIPE_GAP);
            s_stripe_objs[si++] = s;
        }
    }

    /* Player bike: body */
    s_bike_body = make_rect(s_overlay, BIKE_W, BIKE_H, 0x00E676, LV_OPA_COVER);
    lv_obj_set_style_radius(s_bike_body, 4, 0);
    lv_obj_set_style_shadow_width(s_bike_body, 12, 0);
    lv_obj_set_style_shadow_color(s_bike_body, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_shadow_opa(s_bike_body, LV_OPA_50, 0);

    /* Front wheel */
    s_bike_wheel_f = make_rect(s_overlay, BIKE_W + 2, 5, 0x222222, LV_OPA_COVER);
    lv_obj_set_style_radius(s_bike_wheel_f, 2, 0);

    /* Rear wheel */
    s_bike_wheel_r = make_rect(s_overlay, BIKE_W + 2, 5, 0x222222, LV_OPA_COVER);
    lv_obj_set_style_radius(s_bike_wheel_r, 2, 0);

    /* Obstacle objects (pre-allocated, hidden) */
    for (int i = 0; i < MAX_OBS; i++) {
        lv_obj_t *o = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, OBS_W, OBS_H_MIN);
        lv_obj_set_style_bg_color(o, lv_color_hex(0xE53935), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 4, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s_obs_objs[i] = o;
    }

    /* HUD: score (left grass) */
    s_score_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_score_lbl, "0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(s_score_lbl, 10, 10);

    /* HUD: best (left grass) */
    s_hi_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_hi_lbl, "Best:%d", s_hiscore);
    lv_obj_set_style_text_font(s_hi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hi_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(s_hi_lbl, 10, 34);

    /* HUD: speed (right grass) */
    s_speed_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_speed_lbl, "60\nkm/h");
    lv_obj_set_style_text_font(s_speed_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_speed_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_speed_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_speed_lbl, GW - 60, 10);

    /* Centre message label (hidden) */
    s_msg_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_msg_lbl, "");
    lv_obj_set_style_text_font(s_msg_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_msg_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* -- Position bike (body + wheels) ------------------------------------ */
static void position_bike(void)
{
    int bx = (int)(s_bike_x - BIKE_W / 2.0f);
    int by = BIKE_Y - BIKE_H / 2;

    if (s_bike_body)
        lv_obj_set_pos(s_bike_body, bx, by);
    if (s_bike_wheel_f)
        lv_obj_set_pos(s_bike_wheel_f, bx - 1, by - 4);
    if (s_bike_wheel_r)
        lv_obj_set_pos(s_bike_wheel_r, bx - 1, by + BIKE_H);
}

/* -- Reset game -------------------------------------------------------- */
static void reset_game(void)
{
    s_bike_x = (float)(ROAD_LEFT + ROAD_W / 2);
    s_speed = OBS_START_SPD;
    s_score = 0;
    s_next_spawn = 8;
    s_stripe_off = 0.0f;
    s_last_tick_us = esp_timer_get_time();

    rng_seed((uint32_t)esp_timer_get_time());

    for (int i = 0; i < MAX_OBS; i++) {
        s_obs[i].active = false;
        if (s_obs_objs[i])
            lv_obj_add_flag(s_obs_objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* -- Forward declarations --------------------------------------------- */
static void start_tap_cb(lv_event_t *e);
static void gameover_tap_cb(lv_event_t *e);
static void back_btn_cb(lv_event_t *e);

/* -- Ready screen (tap to start) -------------------------------------- */
static void show_ready(void)
{
    build_play_ui();
    reset_game();
    position_bike();

    if (s_msg_lbl) {
        lv_label_set_text(s_msg_lbl, "TRAFFIC RIDER\nTap to Start!");
        lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* Back button (left grass area) */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 60, 24);
    lv_obj_set_pos(back, 4, GH - 30);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, start_tap_cb, LV_EVENT_CLICKED, NULL);

    s_state = RS_READY;
}

static void start_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != RS_READY) return;
    if (s_msg_lbl)
        lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_overlay, start_tap_cb);
    s_state = RS_PLAY;
    s_last_tick_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Traffic Rider started");
}

static void gameover_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != RS_OVER) return;
    lv_obj_remove_event_cb(s_overlay, gameover_tap_cb);
    show_ready();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    game_race_close();
}

/* ================================================================
 *  Public API
 * ================================================================ */
void game_race_open(lv_obj_t *parent)
{
    if (s_active) return;
    load_hiscore();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, GW, GH);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_active = true;
    show_ready();
    ESP_LOGI(TAG, "Traffic Rider opened");
}

void game_race_close(void)
{
    if (!s_active) return;
    destroy_play_objs();
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_state = RS_READY;
    ESP_LOGI(TAG, "Traffic Rider closed");
}

bool game_race_is_active(void)
{
    return s_active;
}

/* ================================================================
 *  Game tick (~100 ms from screen_menu_update)
 * ================================================================ */
void game_race_update(void)
{
    if (!s_active || s_state != RS_PLAY) return;

    /* Delta time */
    int64_t now_us = esp_timer_get_time();
    float dt = (float)(now_us - s_last_tick_us) / 100000.0f;
    s_last_tick_us = now_us;
    if (dt > 3.0f) dt = 3.0f;

    /* Read accel -> steer bike left/right */
    float ax_g, ay_g;
    read_accel(&ax_g, &ay_g);

    /* Short-side tilt: screen-X <- ax_g */
    float steer = ax_g * STEER_SPEED * dt * 14.0f;
    s_bike_x += steer;

    /* Clamp to road */
    float half_w = BIKE_W / 2.0f;
    if (s_bike_x < ROAD_LEFT + half_w + 4)
        s_bike_x = ROAD_LEFT + half_w + 4;
    if (s_bike_x > ROAD_RIGHT - half_w - 4)
        s_bike_x = ROAD_RIGHT - half_w - 4;

    position_bike();

    /* Move traffic downward */
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].active) continue;
        s_obs[i].y += s_speed * dt;
        if (s_obs[i].y > GH + 10) {
            s_obs[i].active = false;
            if (s_obs_objs[i])
                lv_obj_add_flag(s_obs_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else if (s_obs_objs[i]) {
            lv_obj_set_pos(s_obs_objs[i],
                           (int16_t)s_obs[i].x, (int16_t)s_obs[i].y);
        }
    }

    /* Spawn new traffic */
    s_next_spawn--;
    if (s_next_spawn <= 0)
        spawn_obstacle();

    /* Collision check */
    if (check_collision()) {
        s_state = RS_OVER;

        if (s_bike_body)
            lv_obj_set_style_bg_color(s_bike_body, lv_color_hex(0xFF0000), 0);

        if (s_score > s_hiscore) {
            s_hiscore = s_score;
            save_hiscore();
        }

        if (s_msg_lbl) {
            if (s_score >= s_hiscore && s_score > 0)
                lv_label_set_text_fmt(s_msg_lbl,
                    "CRASH!  Score: %d\n" LV_SYMBOL_OK " NEW BEST!\nTap to retry",
                    s_score);
            else
                lv_label_set_text_fmt(s_msg_lbl,
                    "CRASH!  Score: %d\nTap to retry", s_score);
            lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_hi_lbl)
            lv_label_set_text_fmt(s_hi_lbl, "Best:%d", s_hiscore);

        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_overlay, gameover_tap_cb, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "Game over! Score: %d, Best: %d", s_score, s_hiscore);
        return;
    }

    /* Increase speed + score */
    s_speed += OBS_ACCEL * dt;
    if (s_speed > OBS_MAX_SPD) s_speed = OBS_MAX_SPD;
    s_score++;

    /* Scroll lane stripes downward */
    s_stripe_off += s_speed * dt;
    if (s_stripe_off >= STRIPE_GAP) s_stripe_off -= STRIPE_GAP;

    int si = 0;
    for (int lane = 1; lane < LANE_COUNT; lane++) {
        int lx = ROAD_LEFT + lane * LANE_W - STRIPE_W / 2;
        for (int j = 0; j < STRIPE_ROWS; j++) {
            if (s_stripe_objs[si]) {
                int sy = (int)(j * STRIPE_GAP + s_stripe_off);
                if (sy > GH) sy -= STRIPE_GAP * STRIPE_ROWS;
                lv_obj_set_pos(s_stripe_objs[si], lx, sy);
            }
            si++;
        }
    }

    /* Scroll rumble strips */
    int rumble_gap = GH / RUMBLE_COUNT;
    for (int i = 0; i < RUMBLE_COUNT; i++) {
        int ry = (int)(i * rumble_gap + s_stripe_off);
        if (ry > GH) ry -= rumble_gap * RUMBLE_COUNT;
        if (s_rumble_l[i])
            lv_obj_set_pos(s_rumble_l[i], ROAD_LEFT - RUMBLE_W - 3, ry);
        if (s_rumble_r[i])
            lv_obj_set_pos(s_rumble_r[i], ROAD_RIGHT + 4, ry);
    }

    /* Update HUD */
    if (s_score_lbl)
        lv_label_set_text_fmt(s_score_lbl, "%d", s_score);

    if (s_speed_lbl) {
        int kmh = 60 + (int)((s_speed - OBS_START_SPD) * 18.0f);
        lv_label_set_text_fmt(s_speed_lbl, "%d\nkm/h", kmh);
    }
}