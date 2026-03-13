/**
 * @file game_dino.c
 * @brief Chrome Dino — tap-to-jump side-scroller on 640x172.
 *
 * Dino runs left-to-right.  Cacti and birds scroll from the right.
 * Tap anywhere to jump.  Speed increases every 15 seconds.
 * High score saved to LittleFS.
 *
 * Physics tick runs inside screen_menu_update() (~100 ms cadence).
 */

#include "game_dino.h"
#include "hw_config.h"
#include "screen_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "dino";

/* -- Display ----------------------------------------------------------- */
#define GW              LCD_H_RES           /* 640 */
#define GH              LCD_V_RES           /* 172 */

/* -- Ground ------------------------------------------------------------- */
#define GROUND_Y        138                 /* top of ground line */
#define GROUND_H        2                   /* ground line thickness */

/* -- Dino --------------------------------------------------------------- */
#define DINO_W          18
#define DINO_H          28
#define DINO_X          70                  /* fixed horizontal position */
#define DINO_GROUND_Y   (GROUND_Y - DINO_H)
#define JUMP_VEL        (-14.0f)            /* initial jump velocity (upward) */
#define GRAVITY         0.75f               /* gravity per tick */
#define DUCK_H          16                  /* dino height when ducking */

/* -- Obstacles ---------------------------------------------------------- */
#define MAX_OBS         6
#define CACTUS_W_SM     10
#define CACTUS_H_SM     24
#define CACTUS_W_LG     18
#define CACTUS_H_LG     34
#define BIRD_W          22
#define BIRD_H          14
#define BIRD_FLY_Y      (GROUND_Y - 50)    /* bird flies above ground */

/* -- Speed -------------------------------------------------------------- */
#define START_SPEED     4.0f
#define MAX_SPEED       14.0f
#define ACCEL           0.003f              /* continuous ramp per tick */
#define LEVEL_UP_US     (15 * 1000000LL)    /* speed bump every 15s */
#define LEVEL_UP_BUMP   0.5f

/* -- Clouds (decorative) ----------------------------------------------- */
#define CLOUD_COUNT     3
#define CLOUD_W         30
#define CLOUD_H         10

/* -- High score file --------------------------------------------------- */
#define HISCORE_FILE    "/log/dino_best.dat"

/* -- Game states -------------------------------------------------------- */
typedef enum { DS_READY, DS_PLAY, DS_OVER } dino_state_t;

typedef enum { OT_NONE, OT_CACTUS_SM, OT_CACTUS_LG, OT_BIRD } obs_type_t;

typedef struct {
    float      x, y;
    int        w, h;
    obs_type_t type;
    bool       active;
} obstacle_t;

/* -- Static state ------------------------------------------------------- */
static bool          s_active = false;
static lv_obj_t     *s_overlay = NULL;
static dino_state_t  s_state = DS_READY;

/* Dino physics */
static float         s_dino_y;              /* current top-Y position */
static float         s_dino_vy;             /* vertical velocity */
static bool          s_on_ground;
static bool          s_ducking;

/* Obstacles */
static obstacle_t    s_obs[MAX_OBS];
static float         s_speed;
static int           s_spawn_cd;            /* spawn cooldown ticks */

/* Score / timing */
static int           s_score;
static int           s_hiscore;
static int64_t       s_last_tick_us;
static int64_t       s_game_start_us;
static int           s_level;

/* -- LVGL objects ------------------------------------------------------- */
static lv_obj_t     *s_ground_line  = NULL;
static lv_obj_t     *s_dino_body    = NULL;
static lv_obj_t     *s_dino_eye     = NULL;
static lv_obj_t     *s_dino_leg1    = NULL;
static lv_obj_t     *s_dino_leg2    = NULL;
static lv_obj_t     *s_obs_objs[MAX_OBS];
static lv_obj_t     *s_cloud_objs[CLOUD_COUNT];
static lv_obj_t     *s_score_lbl    = NULL;
static lv_obj_t     *s_hi_lbl       = NULL;
static lv_obj_t     *s_msg_lbl      = NULL;
static float         s_cloud_x[CLOUD_COUNT];

/* -- Simple xorshift RNG ---------------------------------------------- */
static uint32_t s_rng = 67890;
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

/* -- Helper: create a simple rect ------------------------------------- */
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

/* -- Spawn an obstacle ------------------------------------------------ */
static void spawn_obstacle(void)
{
    for (int i = 0; i < MAX_OBS; i++) {
        if (s_obs[i].active) continue;

        int roll = rng_range(0, 9);
        obs_type_t type;
        int w, h;
        float y;

        if (roll < 4) {
            type = OT_CACTUS_SM;
            w = CACTUS_W_SM; h = CACTUS_H_SM;
            y = (float)(GROUND_Y - h);
        } else if (roll < 8) {
            type = OT_CACTUS_LG;
            w = CACTUS_W_LG; h = CACTUS_H_LG;
            y = (float)(GROUND_Y - h);
        } else {
            type = OT_BIRD;
            w = BIRD_W; h = BIRD_H;
            y = (float)(BIRD_FLY_Y);
        }

        s_obs[i].x      = (float)(GW + 10);
        s_obs[i].y      = y;
        s_obs[i].w      = w;
        s_obs[i].h      = h;
        s_obs[i].type   = type;
        s_obs[i].active = true;

        if (s_obs_objs[i]) {
            lv_obj_set_size(s_obs_objs[i], w, h);
            if (type == OT_BIRD) {
                lv_obj_set_style_bg_color(s_obs_objs[i],
                    lv_color_hex(0x795548), 0);
                lv_obj_set_style_radius(s_obs_objs[i], 4, 0);
            } else {
                lv_obj_set_style_bg_color(s_obs_objs[i],
                    lv_color_hex(0x2E7D32), 0);
                lv_obj_set_style_radius(s_obs_objs[i], 2, 0);
            }
            lv_obj_clear_flag(s_obs_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
        break;
    }

    /* Cooldown varies inversely with speed */
    int gap = 22 - (int)(s_speed * 1.0f);
    if (gap < 6) gap = 6;
    s_spawn_cd = rng_range(gap, gap + 8);
}

/* -- Collision detect (AABB) ------------------------------------------ */
static bool check_collision(void)
{
    int dh = s_ducking ? DUCK_H : DINO_H;
    float dy = s_ducking ? (GROUND_Y - DUCK_H) : s_dino_y;
    float dx = (float)DINO_X;

    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].active) continue;
        if (dx + DINO_W - 4 > s_obs[i].x + 3 &&
            dx + 4 < s_obs[i].x + s_obs[i].w - 3 &&
            dy + dh - 4 > s_obs[i].y + 3 &&
            dy + 4 < s_obs[i].y + s_obs[i].h - 3) {
            return true;
        }
    }
    return false;
}

/* -- Destroy widget pointers ------------------------------------------ */
static void destroy_objs(void)
{
    s_ground_line = NULL;
    s_dino_body = NULL;
    s_dino_eye = NULL;
    s_dino_leg1 = NULL;
    s_dino_leg2 = NULL;
    s_score_lbl = NULL;
    s_hi_lbl = NULL;
    s_msg_lbl = NULL;
    memset(s_obs_objs, 0, sizeof(s_obs_objs));
    memset(s_cloud_objs, 0, sizeof(s_cloud_objs));
}

/* -- Build gameplay UI ------------------------------------------------ */
static void build_play_ui(void)
{
    lv_obj_clean(s_overlay);
    destroy_objs();

    /* Sky background */
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0D1117), 0);

    /* Clouds (decorative, slow parallax) */
    for (int i = 0; i < CLOUD_COUNT; i++) {
        s_cloud_x[i] = (float)(rng_range(50, GW - 50));
        int cy = rng_range(12, 45);
        s_cloud_objs[i] = make_rect(s_overlay, CLOUD_W, CLOUD_H,
                                     0x333333, LV_OPA_50);
        lv_obj_set_style_radius(s_cloud_objs[i], 5, 0);
        lv_obj_set_pos(s_cloud_objs[i], (int)s_cloud_x[i], cy);
    }

    /* Ground line */
    s_ground_line = make_rect(s_overlay, GW, GROUND_H, 0x555555, LV_OPA_COVER);
    lv_obj_set_pos(s_ground_line, 0, GROUND_Y);

    /* Ground fill below line */
    lv_obj_t *gfill = make_rect(s_overlay, GW, GH - GROUND_Y - GROUND_H,
                                 0x1A1A1A, LV_OPA_COVER);
    lv_obj_set_pos(gfill, 0, GROUND_Y + GROUND_H);

    /* Dino body */
    s_dino_body = make_rect(s_overlay, DINO_W, DINO_H, 0x66BB6A, LV_OPA_COVER);
    lv_obj_set_style_radius(s_dino_body, 4, 0);

    /* Dino eye */
    s_dino_eye = make_rect(s_overlay, 4, 4, 0xFFFFFF, LV_OPA_COVER);
    lv_obj_set_style_radius(s_dino_eye, 2, 0);

    /* Dino legs (two small rects) */
    s_dino_leg1 = make_rect(s_overlay, 4, 8, 0x4CAF50, LV_OPA_COVER);
    s_dino_leg2 = make_rect(s_overlay, 4, 8, 0x4CAF50, LV_OPA_COVER);

    /* Pre-allocate obstacle objects (hidden) */
    for (int i = 0; i < MAX_OBS; i++) {
        lv_obj_t *o = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, CACTUS_W_SM, CACTUS_H_SM);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x2E7D32), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 2, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s_obs_objs[i] = o;
    }

    /* HUD: score */
    s_score_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_score_lbl, "0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(0xBBBBBB), 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_RIGHT, -10, 6);

    /* HUD: best */
    s_hi_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_hi_lbl, "HI %d", s_hiscore);
    lv_obj_set_style_text_font(s_hi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hi_lbl, lv_color_hex(0x777777), 0);
    lv_obj_align(s_hi_lbl, LV_ALIGN_TOP_RIGHT, -10, 26);

    /* Centre message */
    s_msg_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_msg_lbl, "");
    lv_obj_set_style_text_font(s_msg_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_msg_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg_lbl, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* -- Position dino body + eye + legs ---------------------------------- */
static void position_dino(void)
{
    int dh = s_ducking ? DUCK_H : DINO_H;
    float dy = s_ducking ? (float)(GROUND_Y - DUCK_H) : s_dino_y;

    if (s_dino_body) {
        lv_obj_set_size(s_dino_body, DINO_W, dh);
        lv_obj_set_pos(s_dino_body, DINO_X, (int)dy);
    }
    if (s_dino_eye)
        lv_obj_set_pos(s_dino_eye, DINO_X + DINO_W - 6, (int)dy + 3);
    if (s_dino_leg1)
        lv_obj_set_pos(s_dino_leg1, DINO_X + 3, (int)dy + dh);
    if (s_dino_leg2)
        lv_obj_set_pos(s_dino_leg2, DINO_X + DINO_W - 7, (int)dy + dh);
}

/* -- Reset game state ------------------------------------------------- */
static void reset_game(void)
{
    s_dino_y = (float)DINO_GROUND_Y;
    s_dino_vy = 0.0f;
    s_on_ground = true;
    s_ducking = false;
    s_speed = START_SPEED;
    s_score = 0;
    s_spawn_cd = 12;
    s_last_tick_us = esp_timer_get_time();
    s_game_start_us = s_last_tick_us;
    s_level = 0;

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
static void jump_tap_cb(lv_event_t *e);

/* -- Ready screen ----------------------------------------------------- */
static void show_ready(void)
{
    build_play_ui();
    reset_game();
    position_dino();

    if (s_msg_lbl) {
        lv_label_set_text(s_msg_lbl, LV_SYMBOL_RIGHT "  DINO RUN\nTap to Start!");
        lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* Back button */
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

    s_state = DS_READY;
}

static void start_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != DS_READY) return;
    if (s_msg_lbl)
        lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Switch from "start tap" to "jump tap" */
    lv_obj_remove_event_cb(s_overlay, start_tap_cb);
    lv_obj_add_event_cb(s_overlay, jump_tap_cb, LV_EVENT_CLICKED, NULL);

    s_state = DS_PLAY;
    s_last_tick_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Dino Run started");
}

static void jump_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != DS_PLAY) return;
    if (s_on_ground) {
        s_dino_vy = JUMP_VEL;
        s_on_ground = false;
    }
}

static void gameover_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != DS_OVER) return;
    lv_obj_remove_event_cb(s_overlay, gameover_tap_cb);
    show_ready();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    game_dino_close();
}

/* ================================================================
 *  Public API
 * ================================================================ */
void game_dino_open(lv_obj_t *parent)
{
    if (s_active) return;
    load_hiscore();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, GW, GH);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_active = true;
    show_ready();
    ESP_LOGI(TAG, "Dino Run opened");
}

void game_dino_close(void)
{
    if (!s_active) return;
    destroy_objs();
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_state = DS_READY;
    ESP_LOGI(TAG, "Dino Run closed");
}

bool game_dino_is_active(void)
{
    return s_active;
}

/* ================================================================
 *  Game tick (~100 ms from screen_menu_update)
 * ================================================================ */
void game_dino_update(void)
{
    if (!s_active || s_state != DS_PLAY) return;

    /* Delta time */
    int64_t now_us = esp_timer_get_time();
    float dt = (float)(now_us - s_last_tick_us) / 100000.0f;
    s_last_tick_us = now_us;
    if (dt > 3.0f) dt = 3.0f;

    /* -- Dino jump physics ---------------------------------------------- */
    if (!s_on_ground) {
        s_dino_vy += GRAVITY * dt;
        s_dino_y += s_dino_vy * dt;
        if (s_dino_y >= (float)DINO_GROUND_Y) {
            s_dino_y = (float)DINO_GROUND_Y;
            s_dino_vy = 0.0f;
            s_on_ground = true;
        }
    }

    position_dino();

    /* Animate legs when running on ground */
    static int leg_tick = 0;
    leg_tick++;
    if (s_on_ground && s_dino_leg1 && s_dino_leg2) {
        int leg_phase = (leg_tick / 2) % 2;
        int dh = s_ducking ? DUCK_H : DINO_H;
        float dy = s_ducking ? (float)(GROUND_Y - DUCK_H) : s_dino_y;
        if (leg_phase == 0) {
            lv_obj_set_pos(s_dino_leg1, DINO_X + 3, (int)dy + dh);
            lv_obj_set_pos(s_dino_leg2, DINO_X + DINO_W - 7, (int)dy + dh - 3);
        } else {
            lv_obj_set_pos(s_dino_leg1, DINO_X + 3, (int)dy + dh - 3);
            lv_obj_set_pos(s_dino_leg2, DINO_X + DINO_W - 7, (int)dy + dh);
        }
    }

    /* -- Move obstacles left -------------------------------------------- */
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].active) continue;
        s_obs[i].x -= s_speed * dt;
        if (s_obs[i].x < -40) {
            s_obs[i].active = false;
            if (s_obs_objs[i])
                lv_obj_add_flag(s_obs_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else if (s_obs_objs[i]) {
            lv_obj_set_pos(s_obs_objs[i],
                           (int16_t)s_obs[i].x, (int16_t)s_obs[i].y);
        }
    }

    /* -- Spawn ---------------------------------------------------------- */
    s_spawn_cd--;
    if (s_spawn_cd <= 0)
        spawn_obstacle();

    /* -- Collision ------------------------------------------------------ */
    if (check_collision()) {
        s_state = DS_OVER;

        if (s_dino_body)
            lv_obj_set_style_bg_color(s_dino_body, lv_color_hex(0xFF0000), 0);

        if (s_score > s_hiscore) {
            s_hiscore = s_score;
            save_hiscore();
        }

        if (s_msg_lbl) {
            if (s_score >= s_hiscore && s_score > 0)
                lv_label_set_text_fmt(s_msg_lbl,
                    "GAME OVER   Score: %d\n" LV_SYMBOL_OK " NEW BEST!\nTap to retry",
                    s_score);
            else
                lv_label_set_text_fmt(s_msg_lbl,
                    "GAME OVER   Score: %d\nTap to retry", s_score);
            lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_hi_lbl)
            lv_label_set_text_fmt(s_hi_lbl, "HI %d", s_hiscore);

        /* Remove jump, add restart */
        lv_obj_remove_event_cb(s_overlay, jump_tap_cb);
        lv_obj_add_event_cb(s_overlay, gameover_tap_cb, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "Game over! Score: %d, Best: %d", s_score, s_hiscore);
        return;
    }

    /* -- Speed ramp ----------------------------------------------------- */
    s_speed += ACCEL * dt;

    /* Level-up bump every 15 seconds */
    int cur_level = (int)((now_us - s_game_start_us) / LEVEL_UP_US);
    if (cur_level > s_level) {
        s_level = cur_level;
        s_speed += LEVEL_UP_BUMP;
        ESP_LOGI(TAG, "Level %d! Speed -> %.1f", s_level, s_speed);
    }

    if (s_speed > MAX_SPEED) s_speed = MAX_SPEED;
    s_score++;

    /* -- Clouds parallax (slow scroll left) ----------------------------- */
    for (int i = 0; i < CLOUD_COUNT; i++) {
        s_cloud_x[i] -= s_speed * dt * 0.15f;
        if (s_cloud_x[i] < -CLOUD_W) s_cloud_x[i] = (float)(GW + rng_range(10, 60));
        if (s_cloud_objs[i])
            lv_obj_set_pos(s_cloud_objs[i], (int)s_cloud_x[i],
                           lv_obj_get_y(s_cloud_objs[i]));
    }

    /* -- Update HUD ----------------------------------------------------- */
    if (s_score_lbl)
        lv_label_set_text_fmt(s_score_lbl, "%05d", s_score);
    if (s_hi_lbl)
        lv_label_set_text_fmt(s_hi_lbl, "HI %05d", s_hiscore);
}
