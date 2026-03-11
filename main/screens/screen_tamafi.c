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
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "deskbuddy";

/* ════════════════════ States ═════════════════════════════ */
typedef enum {
    DB_NEUTRAL, DB_HAPPY, DB_SAD, DB_SCARED, DB_LOVED
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

/* Animation timing */
#define BLINK_MS          300
#define SCARED_MS         3000
#define LOVED_MS          2000
#define DECAY_MS          900000     /* 15 min */

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

/* ════════════════════ LVGL handles ══════════════════════ */
static lv_obj_t *scr        = NULL;
static lv_obj_t *eye_l      = NULL;
static lv_obj_t *eye_r      = NULL;
static lv_obj_t *state_lbl  = NULL;
static lv_obj_t *happy_bar  = NULL;
static lv_obj_t *happy_lbl  = NULL;
static lv_obj_t *mic_lbl    = NULL;

/* ════════════════════ Mood melodies ═════════════════════ */
static const tone_note_t mel_loved[]  = {{523,80},{659,80},{784,80},{1047,200}};
static const tone_note_t mel_scared[] = {{440,100},{330,100},{220,300}};
static const tone_note_t mel_happy[]  = {{523,100},{659,100},{784,200}};
static const tone_note_t mel_sad[]    = {{330,150},{262,150},{196,300}};

/* ════════════════════ Helpers ═══════════════════════════ */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static db_state_t current_state(void)
{
    if (s_has_ovr && now_ms() < s_ovr_end) return s_ovr;
    s_has_ovr = false;
    if (s_happy >  50) return DB_HAPPY;
    if (s_happy < -50) return DB_SAD;
    return DB_NEUTRAL;
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
        if (new_st == DB_HAPPY) play_melody_async(mel_happy, 3);
        else if (new_st == DB_SAD) play_melody_async(mel_sad, 3);
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

/* ════════════════════ Touch callbacks ═══════════════════ */
static void face_tap_cb(lv_event_t *e)
{
    (void)e;
    s_ovr = DB_LOVED;
    s_ovr_end = now_ms() + LOVED_MS;
    s_has_ovr = true;
    set_happy(s_happy + 10);
    play_melody_async(mel_loved, 4);
    save_nvs();
}

static void face_hold_cb(lv_event_t *e)
{
    (void)e;
    s_ovr = DB_SCARED;
    s_ovr_end = now_ms() + SCARED_MS;
    s_has_ovr = true;
    set_happy(s_happy - 10);
    play_melody_async(mel_scared, 3);
    save_nvs();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    save_nvs();
    app_manager_set_mode(MODE_MENU);
}

/* ════════════════════ Create ════════════════════════════ */
void screen_tamafi_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
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

    /* Mic level */
    mic_lbl = lv_label_create(info);
    lv_label_set_text(mic_lbl, LV_SYMBOL_AUDIO " Mic: --");
    lv_obj_set_style_text_font(mic_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(mic_lbl, lv_color_hex(0x00CCFF), 0);
    lv_obj_align(mic_lbl, LV_ALIGN_TOP_MID, 0, 104);

    /* Back button */
    lv_obj_t *back = lv_btn_create(info);
    lv_obj_set_size(back, 60, 24);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(th_text), 0);
    lv_obj_center(bl);

    /* ── Face touch zone (top of z-order, left 440 px) ──── */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, 440, LCD_V_RES);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, face_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(touch, face_hold_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── Init animation timers ──── */
    uint32_t t = now_ms();
    s_next_blink = t + 2000 + (esp_random() % 3000);
    s_next_move  = t + 1000 + (esp_random() % 2000);
    s_blinking   = false;
    s_eye_x      = 0;
    s_eye_x_tgt  = 0;
    s_last_save  = t;

    /* Start microphone */
    mic_start();

    lv_disp_load_scr(scr);
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
        eye_l     = NULL;
        eye_r     = NULL;
        state_lbl = NULL;
        happy_bar = NULL;
        happy_lbl = NULL;
        mic_lbl   = NULL;
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

    /* ── Horizontal drift ──── */
    if (t >= s_next_move) {
        s_eye_x_tgt = (float)((int)(esp_random() % 61) - 30);
        s_next_move = t + 2000 + (esp_random() % 3000);
    }
    s_eye_x += (s_eye_x_tgt - s_eye_x) * 0.15f;

    /* ── Determine state ──── */
    db_state_t st = current_state();

    int ew, eh, er, by = FACE_CY;
    uint32_t ec = EYE_COLOR_CYAN;
    int jx = 0, jy = 0;

    switch (st) {
    case DB_HAPPY:
        ew = EYE_W;  eh = (int)(EYE_H_HAPPY * bf);  er = EYE_R_HAPPY;
        by = FACE_CY - 15;                      /* raised */
        break;
    case DB_SAD:
        ew = EYE_W;  eh = (int)(EYE_H_SAD * bf);    er = EYE_R_SAD;
        by = FACE_CY + 15;                      /* lowered */
        break;
    case DB_SCARED:
        ew = EYE_SZ_SCARED;  eh = EYE_SZ_SCARED;  er = EYE_R_SCARED;
        jx = (int)(esp_random() % 13) - 6;      /* jitter */
        jy = (int)(esp_random() % 9) - 4;
        break;
    case DB_LOVED:
        ew = EYE_SZ_LOVED;  eh = (int)(EYE_SZ_LOVED * bf);  er = EYE_R_LOVED;
        ec = EYE_COLOR_RED;
        break;
    default: /* NEUTRAL */
        ew = EYE_W;  eh = (int)(EYE_H_NEUTRAL * bf);  er = EYE_R_NEUTRAL;
        break;
    }

    if (eh < 2) eh = 2;
    if (er > eh / 2) er = eh / 2;
    if (er > ew / 2) er = ew / 2;

    /* ── Position eyes ──── */
    int ox = (int)s_eye_x + jx;
    int oy = jy;
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
        case DB_HAPPY:   s = "Happy :)";  break;
        case DB_SAD:     s = "Sad :(";    break;
        case DB_SCARED:  s = "Scared!";   break;
        case DB_LOVED:   s = "Loved <3";  break;
        default:         s = "Neutral ~"; break;
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

    /* ── Mic level display + voice reaction ──── */
    if (mic_lbl) {
        uint8_t lvl = mic_get_level();
        if (mic_is_active()) {
            lv_label_set_text_fmt(mic_lbl, LV_SYMBOL_AUDIO " Mic: %d", lvl);
            /* Voice > 40 → buddy likes being talked to (+1 happy) */
            if (lvl > 40 && !s_has_ovr) {
                static uint32_t s_last_voice = 0;
                if (t - s_last_voice > 3000) {   /* throttle: every 3 s max */
                    set_happy(s_happy + 1);
                    s_last_voice = t;
                }
            }
        } else {
            lv_label_set_text(mic_lbl, LV_SYMBOL_AUDIO " Mic: off");
        }
    }

    /* ── Auto-save every 60 s ──── */
    if (t - s_last_save > 60000) {
        save_nvs();
        s_last_save = t;
    }
}
