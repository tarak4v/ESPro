/**
 * @file screen_tamafi.c
 * @brief DeskBuddy — animated desk companion with expressive eyes.
 *
 * Inspired by FamousWolf/deskbuddy (MIT License).
 * Adapted for ESP-IDF + LVGL v8.4 on Waveshare 640×172 AMOLED.
 *
 * States: Neutral, Happy (>50), Sad (<-50), Loved (tap), Scared (long-press)
 * Eyes animate with periodic blinking and horizontal drift on pure-black face.
 * Happiness decays 5 % toward 0 every 15 minutes.
 */

#include "screen_tamafi.h"
#include "app_manager.h"
#include "hw_config.h"
#include "boot_sound.h"
#include "screen_settings.h"
#include "mic_input.h"
#include "i2c_bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "deskbuddy";

/* ════════════════════ States ═════════════════════════════ */
typedef enum {
    DB_NEUTRAL, DB_HAPPY, DB_SAD, DB_SCARED, DB_LOVED,
    DB_SURPRISED, DB_SLEEPY, DB_CURIOUS, DB_VIBING, DB_ATTENTIVE, DB_DIZZY
} db_state_t;

/* ════════════════════ Layout constants ═══════════════════ */
#define EYE_COLOR_CYAN    0x00FFFF
#define EYE_COLOR_RED     0xFF3333
#define FACE_CX           220        /* centre of 440 px face area */
#define FACE_CY           86         /* centre of 172 px height   */
#define EYE_GAP           30         /* px between inner edges     */

/* Eye dimensions per state */
#define EYE_W             55
#define EYE_H_NEUTRAL     75
#define EYE_R_NEUTRAL     15
#define EYE_H_HAPPY       20
#define EYE_R_HAPPY       10
#define EYE_H_SAD         50
#define EYE_R_SAD         15
#define EYE_SZ_SCARED     18
#define EYE_R_SCARED      9
#define EYE_SZ_LOVED      45
#define EYE_R_LOVED       22

/* New state eye dimensions */
#define EYE_SZ_SURPRISED  65         /* wide open */
#define EYE_H_SURPRISED   65
#define EYE_R_SURPRISED   32         /* very round */
#define EYE_H_SLEEPY      20         /* half-closed droopy */
#define EYE_R_SLEEPY      10
#define EYE_H_ATTENTIVE   80         /* big open listening eyes */
#define EYE_W_ATTENTIVE   60
#define EYE_R_ATTENTIVE   20
#define EYE_H_CURIOUS     60         /* medium tall, searching */
#define EYE_W_CURIOUS     45
#define EYE_R_CURIOUS     22
#define EYE_H_VIBING      50         /* relaxed, rolling */
#define EYE_W_VIBING      55
#define EYE_R_VIBING      25
#define EYE_H_DIZZY       55         /* wobbly dizzy eyes */
#define EYE_W_DIZZY       50
#define EYE_R_DIZZY       27

/* Animation timing */
#define BLINK_MS          300
#define SCARED_MS         3000
#define LOVED_MS          2000
#define SURPRISED_MS      1500
#define SLEEPY_BLINK_MS   600        /* slower blink when sleepy */
#define CURIOUS_MS        4000       /* looking-around duration  */
#define VIBING_MS         6000       /* rolling-eyes duration    */
#define ATTENTIVE_MS      3000       /* big-eyes listening       */
#define DIZZY_MS          3500       /* dizzy from shaking        */
#define DECAY_MS          900000     /* 15 min */

/* Ambient sound thresholds */
#define SPIKE_DELTA       60         /* jump above avg to trigger surprised */
#define SPIKE_MIN_LEVEL   70         /* minimum absolute level for spike   */
#define SILENCE_LEVEL     10         /* below this = silence               */
#define SILENCE_MS        60000      /* 60 s quiet → sleepy                */
#define LOUD_LEVEL        100        /* sustained loud → scared            */
#define LOUD_MS           3000       /* how long loud must last            */
#define TALK_LO           25         /* talking range low                  */
#define TALK_HI           65         /* talking range high                 */
#define TALK_MS           5000       /* sustained talking threshold        */
#define LEVEL_HIST_SZ     10         /* rolling history buffer size        */
#define DB_VOL            ((uint8_t)(g_volume * 70 / 100))  /* DeskBuddy at 40% */

/* ── Mini-game: Catch the Fly ───────────────────────────── */
#define FG_DURATION       20000   /* 20 s round            */
#define FG_ESCAPE_MS      2500    /* fly escapes in 2.5 s  */
#define FG_SPAWN_DELAY    400     /* ms between catches    */
#define FG_FLY_SZ         24      /* visible fly dot size  */
#define FG_AREA_X1        40      /* game area bounds      */
#define FG_AREA_X2        400
#define FG_AREA_Y1        34
#define FG_AREA_Y2        152

/* ════════════════════ Runtime state ══════════════════════ */
static int      s_happy       = 0;   /* –100 … +100 */
static uint32_t s_last_change = 0;

/* Animations */
static float    s_eye_x     = 0, s_eye_x_tgt = 0;
static uint32_t s_blink_t0  = 0;
static bool     s_blinking  = false;
static uint32_t s_next_blink = 0;
static uint32_t s_next_move  = 0;

/* Temporary-state override (Loved / Scared) */
static db_state_t s_ovr     = DB_NEUTRAL;
static uint32_t   s_ovr_end = 0;
static bool       s_has_ovr = false;

/* Auto-save */
static uint32_t s_last_save = 0;

/* Ambient sound analysis */
static uint8_t  s_level_hist[LEVEL_HIST_SZ];
static int      s_hist_idx       = 0;
static uint32_t s_silence_start  = 0;     /* when silence began        */
static bool     s_is_silent      = false;
static uint32_t s_loud_start     = 0;     /* when loud noise began     */
static bool     s_is_loud        = false;
static uint32_t s_talk_start     = 0;     /* when talking range began  */
static bool     s_is_talking     = false;
static uint32_t s_last_talk_bonus= 0;
static uint32_t s_last_sleepy_pen= 0;
static uint32_t s_last_spike     = 0;     /* debounce for spikes       */
static uint32_t s_last_gesture   = 0;     /* debounce for eye gestures */

/* Eye gesture animation */
static float    s_eye_y     = 0;          /* vertical offset for gestures */
static float    s_eye_y_tgt = 0;
static float    s_roll_phase = 0;         /* phase for rolling eye motion */

/* IMU tilt tracking + shake detection */
static float    s_tilt_x      = 0;        /* smoothed tilt X offset (px) */
static float    s_tilt_y      = 0;        /* smoothed tilt Y offset (px) */
static float    s_gyro_mag    = 0;        /* smoothed gyro magnitude     */
static uint32_t s_shake_start = 0;
static bool     s_shaking     = false;

/* Mini-game state */
typedef enum { FG_IDLE, FG_COUNTDOWN, FG_PLAY, FG_OVER } fg_st_t;
static fg_st_t  s_fg_st       = FG_IDLE;
static uint32_t s_fg_t0       = 0;     /* round start time       */
static uint32_t s_fg_fly_t    = 0;     /* current fly spawn time */
static uint32_t s_fg_cd_t     = 0;     /* countdown tick time    */
static int      s_fg_cd_n     = 3;     /* countdown number       */
static int      s_fg_score    = 0;
static bool     s_fg_fly_vis  = false;
static int      s_fg_fly_x    = 0;     /* fly centre coords      */
static int      s_fg_fly_y    = 0;

/* ════════════════════ LVGL handles ══════════════════════ */
static lv_obj_t *scr        = NULL;
static lv_obj_t *eye_l      = NULL;
static lv_obj_t *eye_r      = NULL;
static lv_obj_t *state_lbl  = NULL;
static lv_obj_t *happy_bar  = NULL;
static lv_obj_t *happy_lbl  = NULL;

/* Mini-game handles */
static lv_obj_t *s_fg_fly_obj  = NULL;
static lv_obj_t *s_fg_scr_lbl  = NULL;
static lv_obj_t *s_fg_tmr_lbl  = NULL;
static lv_obj_t *s_fg_msg_lbl  = NULL;
static lv_obj_t *s_fg_play_btn = NULL;

/* ════════════════════ Mood melodies ═════════════════════ */
static const tone_note_t mel_loved[]    = {{523,80},{659,80},{784,80},{1047,200}};
static const tone_note_t mel_scared[]   = {{440,100},{330,100},{220,300}};
static const tone_note_t mel_happy[]    = {{523,100},{659,100},{784,200}};
static const tone_note_t mel_sad[]      = {{330,150},{262,150},{196,300}};
static const tone_note_t mel_surprised[]= {{880,60},{1047,60},{1319,120}};  /* gasp */
static const tone_note_t mel_yawn[]     = {{392,200},{330,200},{262,300}};  /* sleepy yawn */
static const tone_note_t mel_dizzy[]    = {{523,60},{392,60},{523,60},{392,60},{262,200}};
static const tone_note_t mel_catch[]    = {{1047,30},{1319,50}};   /* quick catch ping */

/* ════════════════════ Helpers ═══════════════════════════ */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static db_state_t current_state(void)
{
    if (s_has_ovr && now_ms() < s_ovr_end) return s_ovr;
    s_has_ovr = false;
    if (s_is_silent && (now_ms() - s_silence_start >= SILENCE_MS))
        return DB_SLEEPY;
    if (s_happy >  50) return DB_HAPPY;
    if (s_happy < -50) return DB_SAD;
    return DB_NEUTRAL;
}

static uint8_t level_avg(void)
{
    int sum = 0;
    for (int i = 0; i < LEVEL_HIST_SZ; i++) sum += s_level_hist[i];
    return (uint8_t)(sum / LEVEL_HIST_SZ);
}

static void set_happy(int v)
{
    if (v >  100) v =  100;
    if (v < -100) v = -100;
    db_state_t old_st = current_state();
    s_happy = v;
    s_last_change = now_ms();
    db_state_t new_st = current_state();
    if (old_st != new_st && !s_has_ovr) {
        if (new_st == DB_HAPPY) play_melody_async(mel_happy, 3, DB_VOL);
        else if (new_st == DB_SAD) play_melody_async(mel_sad, 3, DB_VOL);
    }
}

static void decay(void)
{
    if (s_happy == 0) return;
    if (now_ms() - s_last_change < DECAY_MS) return;
    int d;
    if (s_happy > 0) {
        d = (int)fmaxf(s_happy * 0.05f, 1.0f);
        set_happy(s_happy - d);
    } else {
        d = (int)fminf(s_happy * 0.05f, -1.0f);
        set_happy(s_happy - d);
    }
}

/* ════════════════════ NVS persistence ═══════════════════ */
static void save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("deskbuddy", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, "happy", (int16_t)s_happy);
        nvs_commit(h);
        nvs_close(h);
    }
}

void tamafi_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("deskbuddy", NVS_READWRITE, &h) == ESP_OK) {
        int16_t v = 0;
        nvs_get_i16(h, "happy", &v);
        s_happy = v;
        nvs_close(h);
    }
    s_last_change = now_ms();
    ESP_LOGI(TAG, "DeskBuddy loaded happiness=%d", s_happy);
}

/* ════════════════════ IMU helpers ═══════════════════════ */
static void read_imu_data(float *ax_g, float *ay_g, float *gyro_mag_dps)
{
    uint8_t buf[6];
    *ax_g = 0;  *ay_g = 0;  *gyro_mag_dps = 0;

    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) == 0) {
        int16_t ax = (int16_t)((buf[1] << 8) | buf[0]);
        int16_t ay = (int16_t)((buf[3] << 8) | buf[2]);
        *ax_g = ax / 4096.0f;
        *ay_g = ay / 4096.0f;
    }

    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_GX_L, buf, 6) == 0) {
        int16_t gx = (int16_t)((buf[1] << 8) | buf[0]);
        int16_t gy = (int16_t)((buf[3] << 8) | buf[2]);
        int16_t gz = (int16_t)((buf[5] << 8) | buf[4]);
        float gxd = gx / 64.0f, gyd = gy / 64.0f, gzd = gz / 64.0f;
        *gyro_mag_dps = sqrtf(gxd * gxd + gyd * gyd + gzd * gzd);
    }
}

/* ════════════════════ Touch callbacks ═══════════════════ */
static void face_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_fg_st != FG_IDLE) return;
    s_ovr = DB_LOVED;
    s_ovr_end = now_ms() + LOVED_MS;
    s_has_ovr = true;
    set_happy(s_happy + 10);
    play_melody_async(mel_loved, 4, DB_VOL);
    save_nvs();
}

static void face_hold_cb(lv_event_t *e)
{
    (void)e;
    if (s_fg_st != FG_IDLE) return;
    s_ovr = DB_SCARED;
    s_ovr_end = now_ms() + SCARED_MS;
    s_has_ovr = true;
    set_happy(s_happy - 10);
    play_melody_async(mel_scared, 3, DB_VOL);
    save_nvs();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    save_nvs();
    app_manager_set_mode(MODE_MENU);
}

/* ═══════ Mini-game: Catch the Fly ══════════════════════ */

static void fg_show_ui(bool show)
{
    if (!s_fg_scr_lbl) return;
    if (show) {
        lv_obj_clear_flag(s_fg_scr_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_fg_tmr_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_fg_scr_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fg_tmr_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fg_fly_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fg_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fg_spawn_fly(void)
{
    s_fg_fly_x = FG_AREA_X1 + (int)(esp_random() % (FG_AREA_X2 - FG_AREA_X1));
    s_fg_fly_y = FG_AREA_Y1 + (int)(esp_random() % (FG_AREA_Y2 - FG_AREA_Y1));
    lv_obj_set_pos(s_fg_fly_obj, s_fg_fly_x - FG_FLY_SZ / 2,
                                  s_fg_fly_y - FG_FLY_SZ / 2);
    lv_obj_clear_flag(s_fg_fly_obj, LV_OBJ_FLAG_HIDDEN);
    s_fg_fly_vis = true;
    s_fg_fly_t   = now_ms();
}

static void fg_fly_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_fg_st != FG_PLAY || !s_fg_fly_vis) return;
    s_fg_score++;
    s_fg_fly_vis = false;
    lv_obj_add_flag(s_fg_fly_obj, LV_OBJ_FLAG_HIDDEN);
    s_fg_fly_t = now_ms();
    lv_label_set_text_fmt(s_fg_scr_lbl, "Caught: %d", s_fg_score);
    set_happy(s_happy + 1);
    s_ovr = DB_LOVED;
    s_ovr_end = now_ms() + 350;
    s_has_ovr = true;
    play_melody_async(mel_catch, 2, DB_VOL);
}

static void fg_start_cb(lv_event_t *e)
{
    (void)e;
    if (s_fg_st != FG_IDLE) return;
    s_fg_st      = FG_COUNTDOWN;
    s_fg_score   = 0;
    s_fg_cd_n    = 3;
    s_fg_cd_t    = now_ms();
    s_fg_fly_vis = false;
    lv_label_set_text(s_fg_scr_lbl, "Caught: 0");
    lv_label_set_text(s_fg_tmr_lbl, "20s");
    lv_label_set_text(s_fg_msg_lbl, "3");
    fg_show_ui(true);
    lv_obj_clear_flag(s_fg_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_fg_play_btn, LV_OBJ_FLAG_HIDDEN);
}

/* ════════════════════ Create ════════════════════════════ */
void screen_tamafi_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(th_bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Eyes (styled rectangles, positioned in update) ──── */
    eye_l = lv_obj_create(scr);
    lv_obj_remove_style_all(eye_l);
    lv_obj_set_style_bg_color(eye_l, lv_color_hex(EYE_COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(eye_l, LV_OPA_COVER, 0);
    lv_obj_clear_flag(eye_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    eye_r = lv_obj_create(scr);
    lv_obj_remove_style_all(eye_r);
    lv_obj_set_style_bg_color(eye_r, lv_color_hex(EYE_COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(eye_r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(eye_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ── Info panel (right side, glass card) ────────────── */
    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 188, 152);
    lv_obj_set_pos(info, 444, 10);
    lv_obj_set_style_bg_color(info, lv_color_hex(th_card), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_70, 0);
    lv_obj_set_style_radius(info, 12, 0);
    lv_obj_set_style_pad_all(info, 10, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(info);
    lv_label_set_text(title, "DeskBuddy");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* State */
    state_lbl = lv_label_create(info);
    lv_label_set_text(state_lbl, "Neutral ~");
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(th_text), 0);
    lv_obj_align(state_lbl, LV_ALIGN_TOP_MID, 0, 24);

    /* Happiness bar (0-100 range, shows intensity; colour = direction) */
    happy_bar = lv_bar_create(info);
    lv_obj_set_size(happy_bar, 160, 12);
    lv_bar_set_range(happy_bar, 0, 100);
    lv_bar_set_value(happy_bar, abs(s_happy), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(happy_bar, lv_color_hex(th_btn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(happy_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(happy_bar, lv_color_hex(0x00FF88), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(happy_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(happy_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(happy_bar, 6, LV_PART_INDICATOR);
    lv_obj_align(happy_bar, LV_ALIGN_TOP_MID, 0, 50);

    /* Percentage */
    happy_lbl = lv_label_create(info);
    lv_label_set_text_fmt(happy_lbl, "%d%%", s_happy);
    lv_obj_set_style_text_font(happy_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(happy_lbl, lv_color_hex(th_label), 0);
    lv_obj_align(happy_lbl, LV_ALIGN_TOP_MID, 0, 66);

    /* Hint */
    lv_obj_t *hint = lv_label_create(info);
    lv_label_set_text(hint, "Tap: love  Hold: scare");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(th_label), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 84);

    /* Play mini-game button */
    s_fg_play_btn = lv_btn_create(info);
    lv_obj_set_size(s_fg_play_btn, 90, 26);
    lv_obj_align(s_fg_play_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(s_fg_play_btn, lv_color_hex(0x00AA44), 0);
    lv_obj_set_style_radius(s_fg_play_btn, 8, 0);
    lv_obj_add_event_cb(s_fg_play_btn, fg_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(s_fg_play_btn);
    lv_label_set_text(pl, LV_SYMBOL_PLAY " Catch!");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pl);

    /* ── Face touch zone (left 440 px) ──── */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, 440, LCD_V_RES);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, face_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(touch, face_hold_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── Back button (top-left, above touch zone) ──── */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 56, 26);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(th_btn), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(th_text), 0);
    lv_obj_center(bl);

    /* ── Mini-game objects (created hidden) ──── */
    s_fg_fly_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_fg_fly_obj);
    lv_obj_set_size(s_fg_fly_obj, FG_FLY_SZ, FG_FLY_SZ);
    lv_obj_set_style_bg_color(s_fg_fly_obj, lv_color_hex(0x44FF44), 0);
    lv_obj_set_style_bg_opa(s_fg_fly_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_fg_fly_obj, FG_FLY_SZ / 2, 0);
    lv_obj_set_style_shadow_width(s_fg_fly_obj, 12, 0);
    lv_obj_set_style_shadow_color(s_fg_fly_obj, lv_color_hex(0x44FF44), 0);
    lv_obj_set_style_shadow_opa(s_fg_fly_obj, LV_OPA_60, 0);
    lv_obj_add_flag(s_fg_fly_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(s_fg_fly_obj, 10);
    lv_obj_add_event_cb(s_fg_fly_obj, fg_fly_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);

    s_fg_scr_lbl = lv_label_create(scr);
    lv_label_set_text(s_fg_scr_lbl, "");
    lv_obj_set_style_text_font(s_fg_scr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fg_scr_lbl, lv_color_hex(0x44FF44), 0);
    lv_obj_set_pos(s_fg_scr_lbl, 70, 6);
    lv_obj_add_flag(s_fg_scr_lbl, LV_OBJ_FLAG_HIDDEN);

    s_fg_tmr_lbl = lv_label_create(scr);
    lv_label_set_text(s_fg_tmr_lbl, "");
    lv_obj_set_style_text_font(s_fg_tmr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fg_tmr_lbl, lv_color_hex(0xFFFF44), 0);
    lv_obj_set_pos(s_fg_tmr_lbl, 360, 6);
    lv_obj_add_flag(s_fg_tmr_lbl, LV_OBJ_FLAG_HIDDEN);

    s_fg_msg_lbl = lv_label_create(scr);
    lv_label_set_text(s_fg_msg_lbl, "");
    lv_obj_set_width(s_fg_msg_lbl, 440);
    lv_obj_set_style_text_font(s_fg_msg_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_fg_msg_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_fg_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_fg_msg_lbl, 0, 60);
    lv_obj_add_flag(s_fg_msg_lbl, LV_OBJ_FLAG_HIDDEN);

    s_fg_st = FG_IDLE;

    /* ── Init animation timers ──── */
    uint32_t t = now_ms();
    s_next_blink = t + 2000 + (esp_random() % 3000);
    s_next_move  = t + 1000 + (esp_random() % 2000);
    s_blinking   = false;
    s_eye_x      = 0;
    s_eye_x_tgt  = 0;
    s_eye_y      = 0;
    s_eye_y_tgt  = 0;
    s_roll_phase = 0;
    s_last_gesture = 0;
    s_last_save  = t;
    s_tilt_x     = 0;
    s_tilt_y     = 0;
    s_gyro_mag   = 0;
    s_shake_start = 0;
    s_shaking    = false;

    /* Start microphone */
    mic_start();

    /* Init ambient sound analysis */
    memset(s_level_hist, 0, sizeof(s_level_hist));
    s_hist_idx        = 0;
    s_silence_start   = t;
    s_is_silent       = true;
    s_loud_start      = 0;
    s_is_loud         = false;
    s_talk_start      = 0;
    s_is_talking      = false;
    s_last_talk_bonus = 0;
    s_last_sleepy_pen = 0;
    s_last_spike      = 0;

    g_pending_scr = scr;
    screen_tamafi_update();
    ESP_LOGI(TAG, "DeskBuddy created (happiness=%d)", s_happy);
}

/* ════════════════════ Destroy ═══════════════════════════ */
void screen_tamafi_destroy(void)
{
    mic_stop();
    if (scr) {
        save_nvs();
        lv_obj_del(scr);
        scr       = NULL;
        eye_l          = NULL;
        eye_r          = NULL;
        state_lbl      = NULL;
        happy_bar      = NULL;
        happy_lbl      = NULL;
        s_fg_fly_obj   = NULL;
        s_fg_scr_lbl   = NULL;
        s_fg_tmr_lbl   = NULL;
        s_fg_msg_lbl   = NULL;
        s_fg_play_btn  = NULL;
        s_fg_st        = FG_IDLE;
    }
}

/* ════════════════════ Update (~100 ms) ══════════════════ */
void screen_tamafi_update(void)
{
    if (!scr || !eye_l || !eye_r) return;
    uint32_t t = now_ms();

    /* ── Happiness decay ──── */
    decay();

    /* ── Blink animation (V-shape 1→0→1 over BLINK_MS) ── */
    float bf = 1.0f;
    if (s_blinking) {
        uint32_t el = t - s_blink_t0;
        if (el >= BLINK_MS) {
            s_blinking = false;
        } else {
            float p = (float)el / BLINK_MS;
            bf = fabsf(2.0f * p - 1.0f);
            if (bf < 0.05f) bf = 0.05f;
        }
    } else if (t >= s_next_blink) {
        s_blinking   = true;
        s_blink_t0   = t;
        s_next_blink = t + BLINK_MS + 2000 + (esp_random() % 4000);
    }

    /* ── Horizontal drift (skip target during game) ──── */
    if (s_fg_st == FG_IDLE) {
        if (t >= s_next_move) {
            s_eye_x_tgt = (float)((int)(esp_random() % 61) - 30);
            s_next_move = t + 2000 + (esp_random() % 3000);
        }
    }
    s_eye_x += (s_eye_x_tgt - s_eye_x) * 0.15f;

    /* ── Vertical drift (faster during game for fly tracking) ──── */
    if (s_fg_st == FG_IDLE)
        s_eye_y += (s_eye_y_tgt - s_eye_y) * 0.12f;
    else
        s_eye_y += (s_eye_y_tgt - s_eye_y) * 0.25f;

    /* ── IMU tilt tracking + shake detection ──── */
    {
        float ax_g, ay_g, gyro_mag;
        read_imu_data(&ax_g, &ay_g, &gyro_mag);

        /* Smooth tilt (maps ±0.5g → ±35px eye offset) */
        float tx = -ax_g * 70.0f;
        float ty =  ay_g * 50.0f;
        if (tx >  35) tx =  35;
        if (tx < -35) tx = -35;
        if (ty >  25) ty =  25;
        if (ty < -25) ty = -25;
        s_tilt_x += (tx - s_tilt_x) * 0.2f;
        s_tilt_y += (ty - s_tilt_y) * 0.2f;

        /* Smooth gyro magnitude */
        s_gyro_mag = s_gyro_mag * 0.7f + gyro_mag * 0.3f;

        /* Shake → Dizzy (skip during mini-game) */
        if (s_fg_st == FG_IDLE) {
            if (s_gyro_mag > 150.0f) {
                if (!s_shaking) { s_shake_start = t; s_shaking = true; }
                if ((t - s_shake_start > 300) && !s_has_ovr) {
                    s_ovr     = DB_DIZZY;
                    s_ovr_end = t + DIZZY_MS;
                    s_has_ovr = true;
                    s_shaking = false;
                    s_roll_phase = 0;
                    set_happy(s_happy - 3);
                    play_melody_async(mel_dizzy, 5, DB_VOL);
                }
            } else {
                s_shaking = false;
            }
        }
    }

    /* ── Determine state ──── */
    db_state_t st = current_state();

    int ew, eh, er, by = FACE_CY;
    uint32_t ec = EYE_COLOR_CYAN;
    int jx = 0, jy = 0;

    switch (st) {
    case DB_HAPPY:
        ew = EYE_W;  eh = (int)(EYE_H_HAPPY * bf);  er = EYE_R_HAPPY;
        by = FACE_CY - 15;
        break;
    case DB_SAD:
        ew = EYE_W;  eh = (int)(EYE_H_SAD * bf);    er = EYE_R_SAD;
        by = FACE_CY + 15;
        break;
    case DB_SCARED:
        ew = EYE_SZ_SCARED;  eh = EYE_SZ_SCARED;  er = EYE_R_SCARED;
        jx = (int)(esp_random() % 13) - 6;
        jy = (int)(esp_random() % 9) - 4;
        break;
    case DB_LOVED:
        ew = EYE_SZ_LOVED;  eh = (int)(EYE_SZ_LOVED * bf);  er = EYE_R_LOVED;
        ec = EYE_COLOR_RED;
        break;
    case DB_SURPRISED:
        ew = EYE_SZ_SURPRISED;  eh = (int)(EYE_H_SURPRISED * bf);
        er = EYE_R_SURPRISED;
        break;
    case DB_SLEEPY:
        ew = EYE_W;  eh = (int)(EYE_H_SLEEPY * bf);  er = EYE_R_SLEEPY;
        by = FACE_CY + 10;
        break;
    case DB_ATTENTIVE:
        /* Big wide eyes — listening intently */
        ew = EYE_W_ATTENTIVE;  eh = (int)(EYE_H_ATTENTIVE * bf);
        er = EYE_R_ATTENTIVE;
        by = FACE_CY - 5;
        break;
    case DB_CURIOUS:
        /* Searching eyes — rapid horizontal + vertical look-around */
        ew = EYE_W_CURIOUS;  eh = (int)(EYE_H_CURIOUS * bf);
        er = EYE_R_CURIOUS;
        /* Override drift with fast searching motion */
        s_eye_x_tgt = 50.0f * sinf(s_roll_phase * 1.7f);
        s_eye_y_tgt = 20.0f * cosf(s_roll_phase * 2.3f);
        s_roll_phase += 0.15f;
        break;
    case DB_VIBING:
        /* Rolling relaxed eyes — smooth circular motion */
        ew = EYE_W_VIBING;  eh = (int)(EYE_H_VIBING * bf);
        er = EYE_R_VIBING;
        jx = (int)(30.0f * sinf(s_roll_phase));
        jy = (int)(15.0f * cosf(s_roll_phase));
        s_roll_phase += 0.08f;
        ec = 0x55FFDD;   /* slightly green tint when vibing */
        break;
    case DB_DIZZY:
        /* Spiral wobble after being shaken */
        ew = EYE_W_DIZZY;  eh = (int)(EYE_H_DIZZY * bf);
        er = EYE_R_DIZZY;
        jx = (int)(25.0f * sinf(s_roll_phase * 3.0f));
        jy = (int)(15.0f * cosf(s_roll_phase * 2.5f));
        s_roll_phase += 0.2f;
        ec = 0xFFFF55;   /* yellowish tint when dizzy */
        break;
    default: /* NEUTRAL */
        ew = EYE_W;  eh = (int)(EYE_H_NEUTRAL * bf);  er = EYE_R_NEUTRAL;
        break;
    }

    if (eh < 2) eh = 2;
    if (er > eh / 2) er = eh / 2;
    if (er > ew / 2) er = ew / 2;

    /* ── Position eyes ──── */
    int ox = (int)(s_eye_x + s_tilt_x) + jx;
    int oy = (int)(s_eye_y + s_tilt_y) + jy;
    int lx = FACE_CX - EYE_GAP / 2 - ew + ox;
    int rx = FACE_CX + EYE_GAP / 2 + ox;
    int ey = by - eh / 2 + oy;

    lv_obj_set_size(eye_l, ew, eh);
    lv_obj_set_pos(eye_l, lx, ey);
    lv_obj_set_style_radius(eye_l, er, 0);
    lv_obj_set_style_bg_color(eye_l, lv_color_hex(ec), 0);

    lv_obj_set_size(eye_r, ew, eh);
    lv_obj_set_pos(eye_r, rx, ey);
    lv_obj_set_style_radius(eye_r, er, 0);
    lv_obj_set_style_bg_color(eye_r, lv_color_hex(ec), 0);

    /* ── Update info labels ──── */
    if (state_lbl) {
        const char *s;
        switch (st) {
        case DB_HAPPY:     s = "Happy :)";     break;
        case DB_SAD:       s = "Sad :(";       break;
        case DB_SCARED:    s = "Scared!";      break;
        case DB_LOVED:     s = "Loved <3";     break;
        case DB_SURPRISED: s = "Surprised!";   break;
        case DB_SLEEPY:    s = "Sleepy...";    break;
        case DB_ATTENTIVE: s = "Listening!";   break;
        case DB_CURIOUS:   s = "What's that?"; break;
        case DB_VIBING:    s = "Vibing ~";     break;
        case DB_DIZZY:     s = "Dizzy @_@";    break;
        default:           s = "Neutral ~";    break;
        }
        lv_label_set_text(state_lbl, s);
    }

    if (happy_bar) {
        lv_bar_set_value(happy_bar, abs(s_happy), LV_ANIM_ON);
        uint32_t bc = s_happy >= 0 ? 0x00FF88 : 0xFF6644;
        lv_obj_set_style_bg_color(happy_bar, lv_color_hex(bc), LV_PART_INDICATOR);
    }

    if (happy_lbl)
        lv_label_set_text_fmt(happy_lbl, "%d%%", s_happy);

    /* ── Mini-game: Catch the Fly update ──── */
    if (s_fg_st != FG_IDLE) {
        switch (s_fg_st) {
        case FG_COUNTDOWN: {
            uint32_t el = t - s_fg_cd_t;
            int n = 3 - (int)(el / 1000);
            if (n != s_fg_cd_n && n >= 0) {
                s_fg_cd_n = n;
                if (n > 0) lv_label_set_text_fmt(s_fg_msg_lbl, "%d", n);
                else       lv_label_set_text(s_fg_msg_lbl, "GO!");
            }
            if (el >= 4000) {
                s_fg_st    = FG_PLAY;
                s_fg_t0    = t;
                s_fg_fly_t = t;
                lv_obj_add_flag(s_fg_msg_lbl, LV_OBJ_FLAG_HIDDEN);
                fg_spawn_fly();
            }
            break;
        }
        case FG_PLAY: {
            uint32_t elapsed = t - s_fg_t0;
            int rem = ((int)FG_DURATION - (int)elapsed + 999) / 1000;
            if (rem < 0) rem = 0;
            lv_label_set_text_fmt(s_fg_tmr_lbl, "%ds", rem);

            if (elapsed >= FG_DURATION) {
                s_fg_st  = FG_OVER;
                s_fg_cd_t = t;
                lv_obj_add_flag(s_fg_fly_obj, LV_OBJ_FLAG_HIDDEN);
                s_fg_fly_vis = false;
                int bonus = s_fg_score >= 8 ? 10 : s_fg_score >= 5 ? 5 : 0;
                set_happy(s_happy + bonus);
                lv_label_set_text_fmt(s_fg_msg_lbl, "%d caught!  +%d",
                                      s_fg_score, s_fg_score + bonus);
                lv_obj_clear_flag(s_fg_msg_lbl, LV_OBJ_FLAG_HIDDEN);
                s_ovr = (s_fg_score >= 5) ? DB_HAPPY : DB_SAD;
                s_ovr_end = t + 3000;
                s_has_ovr = true;
                play_melody_async(s_fg_score >= 5 ? mel_happy : mel_sad, 3, DB_VOL);
                save_nvs();
            } else if (s_fg_fly_vis) {
                if (t - s_fg_fly_t >= FG_ESCAPE_MS) {
                    s_fg_fly_vis = false;
                    lv_obj_add_flag(s_fg_fly_obj, LV_OBJ_FLAG_HIDDEN);
                    s_fg_fly_t = t;
                    s_ovr = DB_CURIOUS; s_ovr_end = t + 400; s_has_ovr = true;
                } else {
                    int jx = (int)(esp_random() % 5) - 2;
                    int jy = (int)(esp_random() % 5) - 2;
                    lv_obj_set_pos(s_fg_fly_obj,
                                   s_fg_fly_x - FG_FLY_SZ / 2 + jx,
                                   s_fg_fly_y - FG_FLY_SZ / 2 + jy);
                    s_eye_x_tgt = (float)(s_fg_fly_x - FACE_CX) * 0.6f;
                    s_eye_y_tgt = (float)(s_fg_fly_y - FACE_CY) * 0.4f;
                }
            } else {
                if (t - s_fg_fly_t >= FG_SPAWN_DELAY) {
                    fg_spawn_fly();
                }
            }
            break;
        }
        case FG_OVER:
            if (t - s_fg_cd_t >= 3000) {
                s_fg_st = FG_IDLE;
                fg_show_ui(false);
                lv_obj_clear_flag(s_fg_play_btn, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        default: break;
        }
    }

    /* ── Ambient sound → eye gestures (skip during game) ──── */
    if (s_fg_st == FG_IDLE && mic_is_active()) {
        uint8_t lvl = mic_get_level();

        /* Update rolling history */
        s_level_hist[s_hist_idx] = lvl;
        s_hist_idx = (s_hist_idx + 1) % LEVEL_HIST_SZ;
        uint8_t avg = level_avg();

        /* --- Spike detection → Surprised (wide eyes) --- */
        if (lvl >= SPIKE_MIN_LEVEL && (lvl - avg) >= SPIKE_DELTA
            && !s_has_ovr && (t - s_last_spike > 5000)) {
            s_ovr     = DB_SURPRISED;
            s_ovr_end = t + SURPRISED_MS;
            s_has_ovr = true;
            s_last_spike = t;
            s_last_gesture = t;
            s_roll_phase = 0;
            set_happy(s_happy + 3);
            play_melody_async(mel_surprised, 3, DB_VOL);
        }
        /* --- Very loud sustained → Scared (tiny jittering eyes) --- */
        else if (lvl > LOUD_LEVEL) {
            if (!s_is_loud) { s_loud_start = t; s_is_loud = true; }
            if (t - s_loud_start > LOUD_MS && !s_has_ovr) {
                s_ovr     = DB_SCARED;
                s_ovr_end = t + SCARED_MS;
                s_has_ovr = true;
                s_last_gesture = t;
                set_happy(s_happy - 5);
                play_melody_async(mel_scared, 3, DB_VOL);
                s_is_loud = false;
            }
        }
        /* --- Talking range → Attentive first, then Vibing --- */
        else if (lvl >= TALK_LO && lvl <= TALK_HI) {
            s_is_loud = false;
            if (!s_is_talking) { s_talk_start = t; s_is_talking = true; }
            uint32_t talk_dur = t - s_talk_start;

            if (talk_dur > TALK_MS && !s_has_ovr && (t - s_last_gesture > 8000)) {
                /* Sustained conversation → Vibing (rolling eyes, enjoying it) */
                s_ovr     = DB_VIBING;
                s_ovr_end = t + VIBING_MS;
                s_has_ovr = true;
                s_last_gesture = t;
                s_roll_phase = 0;
                if (t - s_last_talk_bonus > 5000) {
                    set_happy(s_happy + 2);
                    s_last_talk_bonus = t;
                }
            } else if (talk_dur > 1000 && talk_dur <= TALK_MS
                       && !s_has_ovr && (t - s_last_gesture > 6000)) {
                /* Starting to hear something → Attentive (big listening eyes) */
                s_ovr     = DB_ATTENTIVE;
                s_ovr_end = t + ATTENTIVE_MS;
                s_has_ovr = true;
                s_last_gesture = t;
            }
            s_is_silent = false;
        }
        /* --- Quiet ambient (not silence, not talking) → occasionally Curious --- */
        else if (lvl >= SILENCE_LEVEL && lvl < TALK_LO) {
            s_is_loud    = false;
            s_is_talking = false;
            s_is_silent  = false;
            /* Occasional curious look-around when there's light ambient noise */
            if (!s_has_ovr && (t - s_last_gesture > 15000)) {
                s_ovr     = DB_CURIOUS;
                s_ovr_end = t + CURIOUS_MS;
                s_has_ovr = true;
                s_last_gesture = t;
                s_roll_phase = 0;
            }
        }
        /* --- Silence → Sleepy --- */
        else if (lvl < SILENCE_LEVEL) {
            s_is_loud    = false;
            s_is_talking = false;
            if (!s_is_silent) { s_silence_start = t; s_is_silent = true; }
            if (t - s_silence_start >= SILENCE_MS) {
                if (t - s_last_sleepy_pen > 30000) {
                    set_happy(s_happy - 1);
                    s_last_sleepy_pen = t;
                }
                /* Play yawn once when entering sleepy */
                if (t - s_silence_start < SILENCE_MS + 200) {
                    play_melody_async(mel_yawn, 3, DB_VOL);
                }
            }
        }
        /* --- Any other level --- */
        else {
            s_is_loud    = false;
            s_is_talking = false;
            s_is_silent  = false;
        }

        /* Wake up from sleepy when sound arrives */
        if (s_is_silent && lvl >= TALK_LO) {
            s_is_silent = false;
        }

        /* Reset vertical drift when not in a gesture that uses it */
        if (st != DB_CURIOUS && st != DB_VIBING) {
            s_eye_y_tgt = 0;
        }
    }

    /* ── Auto-save every 60 s ──── */
    if (t - s_last_save > 60000) {
        save_nvs();
        s_last_save = t;
    }
}
