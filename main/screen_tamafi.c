/**
 * @file screen_tamafi.c
 * @brief TamaFi-style WiFi-fed virtual pet screen for 640×172 AMOLED.
 *
 * Inspired by cifertech/TamaFi (MIT License).
 * Original implementation for ESP-IDF + LVGL on Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Layout (640×172 landscape):
 *   [0-170]   Pet canvas with animated sprite
 *   [170-400] Stat bars (hunger, happiness, health) + mood/stage
 *   [400-640] Age, activity, personality info
 */

#include "screen_tamafi.h"
#include "app_manager.h"
#include "hw_config.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "tamafi";

/* ══════════════════════════════════════════════════════════════
 *  Pet Data Model
 * ══════════════════════════════════════════════════════════════ */

typedef enum { STAGE_BABY = 0, STAGE_TEEN, STAGE_ADULT, STAGE_ELDER } pet_stage_t;
typedef enum {
    MOOD_CALM = 0, MOOD_HAPPY, MOOD_HUNGRY, MOOD_CURIOUS,
    MOOD_BORED, MOOD_SICK, MOOD_EXCITED
} pet_mood_t;
typedef enum { ACT_IDLE = 0, ACT_HUNT, ACT_DISCOVER, ACT_REST } pet_activity_t;
typedef enum { REST_NONE = 0, REST_ENTER, REST_DEEP, REST_WAKE } rest_phase_t;

typedef struct {
    int hunger;
    int happiness;
    int health;
    uint32_t age_minutes;
    uint32_t age_hours;
    uint32_t age_days;
} pet_stats_t;

typedef struct {
    int net_count;
    int strong_count;
    int hidden_count;
    int open_count;
    int avg_rssi;
} wifi_env_t;

/* ── Pet state ─────────────────────────────────────────────── */
static pet_stats_t    pet  = { .hunger = 70, .happiness = 70, .health = 70 };
static pet_stage_t    pet_stage    = STAGE_BABY;
static pet_mood_t     pet_mood     = MOOD_CALM;
static pet_activity_t pet_activity = ACT_IDLE;
static rest_phase_t   rest_phase   = REST_NONE;
static wifi_env_t     wifi_env     = {0};
static bool           pet_alive    = true;

/* Personality traits (randomized at birth) */
static uint8_t trait_curiosity = 70;
static uint8_t trait_activity  = 60;
static uint8_t trait_stress    = 40;

/* ── Timers (millisecond ticks via esp_timer_get_time/1000) ── */
static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t tmr_hunger     = 0;
static uint32_t tmr_happiness  = 0;
static uint32_t tmr_health     = 0;
static uint32_t tmr_age        = 0;
static uint32_t tmr_decision   = 0;
static uint32_t tmr_autosave   = 0;
static uint32_t tmr_anim       = 0;
static uint32_t tmr_rest_phase = 0;
static uint32_t rest_duration  = 0;
static bool     rest_stats_applied = false;

/* Decision interval randomization */
#define DECISION_MIN_MS  8000
#define DECISION_MAX_MS  15000
static uint32_t decision_interval = 10000;

/* WiFi scan state */
static volatile bool wifi_scan_active  = false;
static uint32_t      last_wifi_scan_ms = 0;

/* Animation */
static int  anim_frame    = 0;
#define ANIM_FRAMES       4
#define ANIM_IDLE_MS      500
#define ANIM_FAST_MS      300
#define ANIM_SLOW_MS      700

/* Rest animation */
static int rest_anim_idx = 0;
#define REST_ENTER_DELAY  400
#define REST_WAKE_DELAY   400
#define REST_MIN_MS       8000
#define REST_MAX_MS       20000

/* ══════════════════════════════════════════════════════════════
 *  NVS Persistence
 * ══════════════════════════════════════════════════════════════ */
#define NVS_NS_PET  "tamafi"

static void pet_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PET, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "hunger",  pet.hunger);
    nvs_set_i32(h, "happy",   pet.happiness);
    nvs_set_i32(h, "health",  pet.health);
    nvs_set_u32(h, "ageMin",  pet.age_minutes);
    nvs_set_u32(h, "ageHr",   pet.age_hours);
    nvs_set_u32(h, "ageDay",  pet.age_days);
    nvs_set_u8(h,  "stage",   (uint8_t)pet_stage);
    nvs_set_u8(h,  "tCur",    trait_curiosity);
    nvs_set_u8(h,  "tAct",    trait_activity);
    nvs_set_u8(h,  "tStr",    trait_stress);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Pet state saved");
}

static void pet_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PET, NVS_READONLY, &h) != ESP_OK) {
        /* First boot — randomize traits */
        trait_curiosity = 40 + (esp_random() % 50);
        trait_activity  = 30 + (esp_random() % 60);
        trait_stress    = 20 + (esp_random() % 60);
        pet_save();
        return;
    }
    int32_t v;
    if (nvs_get_i32(h, "hunger", &v) == ESP_OK) pet.hunger    = v;
    if (nvs_get_i32(h, "happy",  &v) == ESP_OK) pet.happiness = v;
    if (nvs_get_i32(h, "health", &v) == ESP_OK) pet.health    = v;
    uint32_t u;
    if (nvs_get_u32(h, "ageMin", &u) == ESP_OK) pet.age_minutes = u;
    if (nvs_get_u32(h, "ageHr",  &u) == ESP_OK) pet.age_hours   = u;
    if (nvs_get_u32(h, "ageDay", &u) == ESP_OK) pet.age_days    = u;
    uint8_t b;
    if (nvs_get_u8(h, "stage", &b) == ESP_OK) pet_stage = (pet_stage_t)b;
    if (nvs_get_u8(h, "tCur",  &b) == ESP_OK) trait_curiosity = b;
    if (nvs_get_u8(h, "tAct",  &b) == ESP_OK) trait_activity  = b;
    if (nvs_get_u8(h, "tStr",  &b) == ESP_OK) trait_stress    = b;
    nvs_close(h);
    ESP_LOGI(TAG, "Pet loaded: H=%d Hp=%d Hl=%d stage=%d", pet.hunger, pet.happiness, pet.health, pet_stage);
}

void tamafi_load_from_nvs(void) { pet_load(); }

/* ══════════════════════════════════════════════════════════════
 *  Clamp helper
 * ══════════════════════════════════════════════════════════════ */
static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ══════════════════════════════════════════════════════════════
 *  WiFi Scan (blocking, in FreeRTOS task — matches screen_menu.c)
 * ══════════════════════════════════════════════════════════════ */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 100, .max = 300 },
    };
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 20) ap_count = 20;

        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);

            wifi_env.net_count    = ap_count;
            wifi_env.strong_count = 0;
            wifi_env.hidden_count = 0;
            wifi_env.open_count   = 0;
            int total_rssi = 0;

            for (int i = 0; i < ap_count; i++) {
                total_rssi += ap_list[i].rssi;
                if (ap_list[i].rssi > -60) wifi_env.strong_count++;
                if (ap_list[i].ssid[0] == '\0') wifi_env.hidden_count++;
                if (ap_list[i].authmode == WIFI_AUTH_OPEN) wifi_env.open_count++;
            }
            wifi_env.avg_rssi = ap_count > 0 ? total_rssi / ap_count : -100;
            free(ap_list);

            last_wifi_scan_ms = ms_now();
            ESP_LOGI(TAG, "WiFi scan: %d nets, avg RSSI %d", wifi_env.net_count, wifi_env.avg_rssi);
        }
    }
    wifi_scan_active = false;
    vTaskDelete(NULL);
}

static void start_wifi_scan(void)
{
    if (wifi_scan_active) return;
    wifi_scan_active = true;
    xTaskCreatePinnedToCore(wifi_scan_task, "tamafi_scan", 4096, NULL, 2, NULL, 1);
}

/* ══════════════════════════════════════════════════════════════
 *  Mood & Evolution Logic
 * ══════════════════════════════════════════════════════════════ */
static void update_mood(void)
{
    uint32_t now = ms_now();
    if (pet.health < 25 ||
        (wifi_env.net_count == 0 && last_wifi_scan_ms > 0 &&
         now - last_wifi_scan_ms > 60000)) {
        pet_mood = MOOD_SICK; return;
    }
    if (pet.hunger < 25) { pet_mood = MOOD_HUNGRY; return; }
    if (pet.happiness > 80 && wifi_env.net_count > 8) { pet_mood = MOOD_EXCITED; return; }
    if (pet.happiness > 60 && wifi_env.net_count > 0) { pet_mood = MOOD_HAPPY; return; }
    if (wifi_env.net_count == 0 && now - last_wifi_scan_ms > 30000) { pet_mood = MOOD_BORED; return; }
    if (wifi_env.hidden_count > 0 || wifi_env.open_count > 0) { pet_mood = MOOD_CURIOUS; return; }
    pet_mood = MOOD_CALM;
}

static void update_evolution(void)
{
    uint32_t a = pet.age_minutes + pet.age_hours * 60 + pet.age_days * 1440;
    int avg = (pet.hunger + pet.happiness + pet.health) / 3;

    if (a >= 180 && avg > 40 && pet_stage < STAGE_ELDER) {
        pet_stage = STAGE_ELDER;
    } else if (a >= 60 && avg > 45 && pet_stage < STAGE_ADULT) {
        pet_stage = STAGE_ADULT;
    } else if (a >= 20 && avg > 35 && pet_stage < STAGE_TEEN) {
        pet_stage = STAGE_TEEN;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Activity Resolution (WiFi-based feeding/discovering)
 * ══════════════════════════════════════════════════════════════ */
static void resolve_hunt(void)
{
    int n = wifi_env.net_count;
    if (n == 0) {
        pet.hunger    = clamp(pet.hunger - 15, 0, 100);
        pet.happiness = clamp(pet.happiness - 10, 0, 100);
        pet.health    = clamp(pet.health - 5, 0, 100);
    } else {
        int hd = n * 2 + wifi_env.strong_count * 3;
        if (hd > 35) hd = 35;
        pet.hunger = clamp(pet.hunger + hd, 0, 100);

        int variety = wifi_env.hidden_count * 2 + wifi_env.open_count;
        int hp = variety * 3 + (wifi_env.avg_rssi + 100) / 3;
        if (hp > 30) hp = 30;
        pet.happiness = clamp(pet.happiness + hp, 0, 100);

        int hl = 0;
        if (wifi_env.avg_rssi > -75) hl += 5;
        if (wifi_env.avg_rssi > -65) hl += 5;
        if (wifi_env.strong_count > 5) hl += 3;
        pet.health = clamp(pet.health + hl, 0, 100);
    }
}

static void resolve_discover(void)
{
    int n = wifi_env.net_count;
    if (n == 0) {
        pet.happiness = clamp(pet.happiness - 5, 0, 100);
        pet.hunger    = clamp(pet.hunger - 3, 0, 100);
    } else {
        int curiosity_score = wifi_env.hidden_count * 4 + wifi_env.open_count * 3 + n;
        int hp = curiosity_score / 2;
        if (hp > 35) hp = 35;
        pet.happiness = clamp(pet.happiness + hp, 0, 100);
        pet.hunger    = clamp(pet.hunger - 5, 0, 100);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Rest State Machine
 * ══════════════════════════════════════════════════════════════ */
static void step_rest(void)
{
    if (pet_activity != ACT_REST || rest_phase == REST_NONE) return;
    uint32_t now = ms_now();

    switch (rest_phase) {
    case REST_ENTER:
        if (now - tmr_rest_phase >= REST_ENTER_DELAY) {
            tmr_rest_phase = now;
            if (rest_anim_idx > 0) { rest_anim_idx--; }
            else { rest_phase = REST_DEEP; tmr_rest_phase = now; rest_stats_applied = false; }
        }
        break;
    case REST_DEEP:
        if (!rest_stats_applied && now - tmr_rest_phase > rest_duration / 2) {
            pet.hunger    = clamp(pet.hunger - 3, 0, 100);
            pet.happiness = clamp(pet.happiness + 10, 0, 100);
            pet.health    = clamp(pet.health + 15, 0, 100);
            rest_stats_applied = true;
        }
        if (now - tmr_rest_phase >= rest_duration) {
            rest_phase = REST_WAKE;
            tmr_rest_phase = now;
            rest_anim_idx = 0;
        }
        break;
    case REST_WAKE:
        if (now - tmr_rest_phase >= REST_WAKE_DELAY) {
            tmr_rest_phase = now;
            if (rest_anim_idx < 4) { rest_anim_idx++; }
            else { rest_phase = REST_NONE; pet_activity = ACT_IDLE; }
        }
        break;
    default: break;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Autonomous Decision Engine
 * ══════════════════════════════════════════════════════════════ */
static void decide_next_activity(void)
{
    if (pet_activity != ACT_IDLE || rest_phase != REST_NONE) return;
    uint32_t now = ms_now();
    if (now - tmr_decision < decision_interval) return;
    tmr_decision = now;
    decision_interval = DECISION_MIN_MS + (esp_random() % (DECISION_MAX_MS - DECISION_MIN_MS));

    int desire_hunt = (100 - pet.hunger) + trait_curiosity / 2;
    int desire_disc = trait_curiosity + wifi_env.hidden_count * 10 +
                      wifi_env.open_count * 6 + wifi_env.net_count * 2 + (esp_random() % 20);
    int desire_rest = (100 - pet.health) + trait_stress / 2;
    int desire_idle = 10;

    if (wifi_env.net_count == 0) { desire_hunt /= 2; desire_disc /= 2; }
    if (pet.hunger < 20) desire_rest -= 10;

    /* Mood modifiers */
    if (pet_mood == MOOD_HUNGRY)  { desire_hunt += 20; desire_rest -= 10; }
    if (pet_mood == MOOD_CURIOUS) { desire_disc += 15; }
    if (pet_mood == MOOD_SICK)    { desire_rest += 20; desire_disc -= 10; }
    if (pet_mood == MOOD_EXCITED) { desire_disc += 10; desire_hunt += 5; }
    if (pet_mood == MOOD_BORED)   { desire_disc += 10; desire_hunt += 5; }

    if (desire_hunt < 0) desire_hunt = 0;
    if (desire_disc < 0) desire_disc = 0;
    if (desire_rest < 0) desire_rest = 0;

    int best = desire_idle;
    pet_activity_t chosen = ACT_IDLE;
    if (desire_hunt > best) { best = desire_hunt; chosen = ACT_HUNT; }
    if (desire_disc > best) { best = desire_disc; chosen = ACT_DISCOVER; }
    if (desire_rest > best) { best = desire_rest; chosen = ACT_REST; }

    if (chosen == ACT_IDLE) return;

    if (chosen == ACT_HUNT || chosen == ACT_DISCOVER) {
        pet_activity = chosen;
        start_wifi_scan();
    } else if (chosen == ACT_REST) {
        pet_activity    = ACT_REST;
        rest_phase      = REST_ENTER;
        rest_anim_idx   = 4;
        tmr_rest_phase  = ms_now();
        rest_duration   = REST_MIN_MS + (esp_random() % (REST_MAX_MS - REST_MIN_MS));
        rest_stats_applied = false;
    }
    ESP_LOGI(TAG, "Decision: %s", chosen == ACT_HUNT ? "Hunt" :
             chosen == ACT_DISCOVER ? "Discover" : "Rest");
}

/* ══════════════════════════════════════════════════════════════
 *  Pet Reset
 * ══════════════════════════════════════════════════════════════ */
static void reset_pet(bool full)
{
    pet.hunger = 70; pet.happiness = 70; pet.health = 70;
    if (full) {
        pet.age_minutes = 0; pet.age_hours = 0; pet.age_days = 0;
        pet_stage = STAGE_BABY;
        trait_curiosity = 40 + (esp_random() % 50);
        trait_activity  = 30 + (esp_random() % 60);
        trait_stress    = 20 + (esp_random() % 60);
    }
    pet_activity = ACT_IDLE;
    rest_phase   = REST_NONE;
    pet_alive    = true;
    wifi_env = (wifi_env_t){0};
    pet_save();
}

/* ══════════════════════════════════════════════════════════════
 *  Main Logic Tick (called ~100ms from update)
 * ══════════════════════════════════════════════════════════════ */
static void logic_tick(void)
{
    if (!pet_alive) return;
    uint32_t now = ms_now();

    /* Stat decay */
    if (now - tmr_hunger >= 5000) {
        pet.hunger = clamp(pet.hunger - 2, 0, 100);
        tmr_hunger = now;
    }
    if (now - tmr_happiness >= 7000) {
        int dec = (wifi_env.net_count == 0 && now - last_wifi_scan_ms > 30000) ? 3 : 1;
        pet.happiness = clamp(pet.happiness - dec, 0, 100);
        tmr_happiness = now;
    }
    if (now - tmr_health >= 10000) {
        int dec = (pet.hunger < 20 || pet.happiness < 20) ? 2 : 1;
        pet.health = clamp(pet.health - dec, 0, 100);
        tmr_health = now;
    }

    /* Age */
    if (now - tmr_age >= 60000) {
        pet.age_minutes++;
        if (pet.age_minutes >= 60) { pet.age_minutes -= 60; pet.age_hours++; }
        if (pet.age_hours >= 24)   { pet.age_hours -= 24;   pet.age_days++; }
        tmr_age = now;
    }

    /* WiFi activity resolution */
    if ((pet_activity == ACT_HUNT || pet_activity == ACT_DISCOVER) && !wifi_scan_active) {
        if (last_wifi_scan_ms > 0) {
            if (pet_activity == ACT_HUNT) resolve_hunt();
            else resolve_discover();
            pet_activity = ACT_IDLE;
        }
    }

    step_rest();
    update_mood();
    update_evolution();

    /* Death check */
    if (pet.hunger <= 0 && pet.happiness <= 0 && pet.health <= 0) {
        pet_alive = false;
        pet_activity = ACT_IDLE;
        rest_phase = REST_NONE;
        ESP_LOGW(TAG, "Pet died!");
    }

    /* Autosave every 30s */
    if (now - tmr_autosave >= 30000) {
        tmr_autosave = now;
        pet_save();
    }

    /* Autonomous behavior */
    if (pet_activity == ACT_IDLE && rest_phase == REST_NONE) {
        decide_next_activity();
    }
}

/* ══════════════════════════════════════════════════════════════
 *  LVGL UI Elements
 * ══════════════════════════════════════════════════════════════ */
static lv_obj_t *scr = NULL;

/* Pet canvas */
static lv_obj_t *pet_canvas = NULL;
static lv_color_t *canvas_buf = NULL;
#define CANVAS_W 120
#define CANVAS_H 120

/* Stat bars */
static lv_obj_t *bar_hunger   = NULL;
static lv_obj_t *bar_happy    = NULL;
static lv_obj_t *bar_health   = NULL;
static lv_obj_t *lbl_hunger   = NULL;
static lv_obj_t *lbl_happy    = NULL;
static lv_obj_t *lbl_health   = NULL;

/* Info labels */
static lv_obj_t *lbl_mood     = NULL;
static lv_obj_t *lbl_stage    = NULL;
static lv_obj_t *lbl_age      = NULL;
static lv_obj_t *lbl_activity = NULL;
static lv_obj_t *lbl_wifi_env = NULL;
static lv_obj_t *lbl_death    = NULL;

/* Mode indicator dots */
static lv_obj_t *mode_dots[4];

/* Touch areas for feed/play */
static lv_obj_t *btn_feed  = NULL;
static lv_obj_t *btn_play  = NULL;
static lv_obj_t *btn_reset = NULL;

/* ══════════════════════════════════════════════════════════════
 *  Text Helpers
 * ══════════════════════════════════════════════════════════════ */
static const char *mood_str(pet_mood_t m)
{
    switch (m) {
    case MOOD_CALM:    return "Calm";
    case MOOD_HAPPY:   return "Happy";
    case MOOD_HUNGRY:  return "Hungry";
    case MOOD_CURIOUS: return "Curious";
    case MOOD_BORED:   return "Bored";
    case MOOD_SICK:    return "Sick";
    case MOOD_EXCITED: return "Excited";
    }
    return "?";
}

static const char *stage_str(pet_stage_t s)
{
    switch (s) {
    case STAGE_BABY:  return "Baby";
    case STAGE_TEEN:  return "Teen";
    case STAGE_ADULT: return "Adult";
    case STAGE_ELDER: return "Elder";
    }
    return "?";
}

static const char *activity_str(pet_activity_t a)
{
    switch (a) {
    case ACT_HUNT:     return "Hunting WiFi...";
    case ACT_DISCOVER: return "Discovering...";
    case ACT_REST:     return "Resting...";
    default:           return "Idle";
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Pet Drawing (Canvas-based pixel art)
 *
 *  Simple shapes drawn programmatically — no external bitmaps.
 *  The pet grows bigger and changes appearance with evolution.
 * ══════════════════════════════════════════════════════════════ */

static void canvas_clear(void)
{
    if (!pet_canvas) return;
    lv_canvas_fill_bg(pet_canvas, lv_color_black(), LV_OPA_COVER);
}

/* Color palette for the pet */
#define C_BODY    lv_color_make(100, 200, 255)   /* light blue body */
#define C_BODY2   lv_color_make(70, 160, 220)    /* darker blue accent */
#define C_EYE     lv_color_make(255, 255, 255)
#define C_PUPIL   lv_color_make(20, 20, 40)
#define C_MOUTH   lv_color_make(255, 100, 120)
#define C_CHEEK   lv_color_make(255, 180, 200)
#define C_SLEEP   lv_color_make(180, 180, 255)
#define C_SICK    lv_color_make(140, 200, 140)
#define C_ELDER   lv_color_make(180, 180, 190)

static void draw_rect(int x, int y, int w, int h, lv_color_t c)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            if (x+dx >= 0 && x+dx < CANVAS_W && y+dy >= 0 && y+dy < CANVAS_H)
                lv_canvas_set_px_color(pet_canvas, x+dx, y+dy, c);
}

static void draw_circle(int cx, int cy, int r, lv_color_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                if (cx+dx >= 0 && cx+dx < CANVAS_W && cy+dy >= 0 && cy+dy < CANVAS_H)
                    lv_canvas_set_px_color(pet_canvas, cx+dx, cy+dy, c);
}

static void draw_pet_body(int cx, int cy, int size, lv_color_t body_col)
{
    /* Main body: rounded blob */
    draw_circle(cx, cy, size, body_col);
    draw_circle(cx, cy - 2, size - 2, body_col);
    /* Accent belly */
    draw_circle(cx, cy + size/4, size * 2 / 3, C_BODY2);
}

static void draw_pet_eyes(int cx, int cy, int size, int frame, bool sleeping, bool sick)
{
    int eye_y = cy - size / 3;
    int eye_sep = size / 2;

    if (sleeping) {
        /* Closed line eyes */
        draw_rect(cx - eye_sep - 3, eye_y, 6, 2, C_EYE);
        draw_rect(cx + eye_sep - 3, eye_y, 6, 2, C_EYE);
    } else if (sick) {
        /* X eyes */
        draw_rect(cx - eye_sep - 2, eye_y - 2, 2, 2, C_EYE);
        draw_rect(cx - eye_sep + 1, eye_y + 1, 2, 2, C_EYE);
        draw_rect(cx - eye_sep + 1, eye_y - 2, 2, 2, C_EYE);
        draw_rect(cx - eye_sep - 2, eye_y + 1, 2, 2, C_EYE);
        draw_rect(cx + eye_sep - 2, eye_y - 2, 2, 2, C_EYE);
        draw_rect(cx + eye_sep + 1, eye_y + 1, 2, 2, C_EYE);
        draw_rect(cx + eye_sep + 1, eye_y - 2, 2, 2, C_EYE);
        draw_rect(cx + eye_sep - 2, eye_y + 1, 2, 2, C_EYE);
    } else {
        /* Normal eyes with blink on frame 3 */
        if (frame == 3) {
            draw_rect(cx - eye_sep - 3, eye_y, 6, 2, C_EYE);
            draw_rect(cx + eye_sep - 3, eye_y, 6, 2, C_EYE);
        } else {
            draw_circle(cx - eye_sep, eye_y, 4, C_EYE);
            draw_circle(cx + eye_sep, eye_y, 4, C_EYE);
            /* Pupils offset by frame for "looking around" */
            int px = (frame == 0) ? 0 : (frame == 1) ? 1 : -1;
            draw_circle(cx - eye_sep + px, eye_y, 2, C_PUPIL);
            draw_circle(cx + eye_sep + px, eye_y, 2, C_PUPIL);
        }
    }
}

static void draw_pet_mouth(int cx, int cy, int size, pet_mood_t mood)
{
    int my = cy + size / 4;
    switch (mood) {
    case MOOD_HAPPY:
    case MOOD_EXCITED:
        /* Wide smile */
        draw_rect(cx - 5, my, 10, 2, C_MOUTH);
        draw_rect(cx - 6, my - 1, 2, 2, C_MOUTH);
        draw_rect(cx + 5, my - 1, 2, 2, C_MOUTH);
        break;
    case MOOD_HUNGRY:
        /* Open mouth (O) */
        draw_circle(cx, my + 2, 4, C_MOUTH);
        draw_circle(cx, my + 2, 2, lv_color_make(40, 20, 30));
        break;
    case MOOD_SICK:
        /* Wavy line */
        draw_rect(cx - 4, my, 3, 2, C_MOUTH);
        draw_rect(cx - 1, my + 1, 3, 2, C_MOUTH);
        draw_rect(cx + 2, my, 3, 2, C_MOUTH);
        break;
    case MOOD_BORED:
        /* Flat line */
        draw_rect(cx - 4, my, 8, 2, C_MOUTH);
        break;
    default:
        /* Small smile */
        draw_rect(cx - 3, my, 6, 2, C_MOUTH);
        break;
    }
}

static void draw_pet_cheeks(int cx, int cy, int size)
{
    int cheek_y = cy;
    draw_circle(cx - size + 3, cheek_y, 3, C_CHEEK);
    draw_circle(cx + size - 3, cheek_y, 3, C_CHEEK);
}

/* Decorations for different stages */
static void draw_stage_decoration(int cx, int cy, int size, pet_stage_t stage)
{
    switch (stage) {
    case STAGE_BABY:
        /* Tiny antenna */
        draw_rect(cx, cy - size - 6, 2, 6, lv_color_make(200, 200, 100));
        draw_circle(cx, cy - size - 8, 3, lv_color_make(255, 255, 100));
        break;
    case STAGE_TEEN:
        /* Small ears/horns */
        draw_rect(cx - size + 2, cy - size + 2, 6, 8, C_BODY);
        draw_rect(cx + size - 7, cy - size + 2, 6, 8, C_BODY);
        break;
    case STAGE_ADULT:
        /* Crown-like spikes */
        draw_rect(cx - 6, cy - size - 4, 4, 6, lv_color_make(255, 215, 0));
        draw_rect(cx - 1, cy - size - 7, 4, 9, lv_color_make(255, 215, 0));
        draw_rect(cx + 4, cy - size - 4, 4, 6, lv_color_make(255, 215, 0));
        break;
    case STAGE_ELDER:
        /* Little halo */
        for (int a = 0; a < 360; a += 30) {
            float rad = a * 3.14159f / 180.0f;
            int hx = cx + (int)(12 * cosf(rad));
            int hy = (cy - size - 6) + (int)(4 * sinf(rad));
            if (hx >= 0 && hx < CANVAS_W && hy >= 0 && hy < CANVAS_H)
                lv_canvas_set_px_color(pet_canvas, hx, hy, lv_color_make(255, 255, 200));
        }
        break;
    }
}

/* Activity-specific overlays */
static void draw_activity_overlay(int cx, int cy, int size, pet_activity_t act, int frame)
{
    if (act == ACT_HUNT) {
        /* WiFi symbol arcs */
        int wx = cx + size + 8;
        int wy = cy - size / 2;
        for (int i = 0; i <= frame; i++) {
            int r = 4 + i * 5;
            for (int a = -45; a <= 45; a += 5) {
                float rad = a * 3.14159f / 180.0f;
                int px = wx + (int)(r * sinf(rad));
                int py = wy - (int)(r * cosf(rad));
                if (px >= 0 && px < CANVAS_W && py >= 0 && py < CANVAS_H)
                    lv_canvas_set_px_color(pet_canvas, px, py, lv_color_make(0, 200, 255));
            }
        }
        draw_circle(wx, wy, 2, lv_color_make(0, 200, 255));
    } else if (act == ACT_REST && rest_phase == REST_DEEP) {
        /* Zzz */
        int zx = cx + size + 5;
        int zy = cy - size;
        lv_color_t zc = C_SLEEP;
        draw_rect(zx,     zy,     8, 2, zc);
        draw_rect(zx + 2, zy + 3, 2, 2, zc);
        draw_rect(zx,     zy + 5, 8, 2, zc);
        draw_rect(zx + 8, zy + 8, 6, 2, zc);
        draw_rect(zx + 10,zy +10, 2, 2, zc);
        draw_rect(zx + 8, zy +12, 6, 2, zc);
    } else if (act == ACT_DISCOVER) {
        /* Sparkle/stars */
        lv_color_t sc = lv_color_make(255, 255, 100);
        int sx = cx + size + 6 + (frame * 3) % 10;
        int sy = cy - size / 2 - (frame * 5) % 8;
        draw_rect(sx, sy - 2, 2, 5, sc);
        draw_rect(sx - 2, sy, 6, 2, sc);
    }
}

/* Dead state */
static void draw_pet_dead(int cx, int cy, int size)
{
    lv_color_t gc = lv_color_make(100, 100, 100);
    draw_circle(cx, cy + size / 2, size, gc);
    /* X eyes */
    draw_rect(cx - size/2 - 2, cy - 2, 4, 2, C_EYE);
    draw_rect(cx - size/2, cy,     4, 2, C_EYE);
    draw_rect(cx + size/2 - 2, cy - 2, 4, 2, C_EYE);
    draw_rect(cx + size/2, cy,     4, 2, C_EYE);
    /* Ghost wisps going up */
    for (int i = 0; i < 3; i++) {
        int wx = cx - 8 + i * 8;
        int wy = cy - size - 5 - (anim_frame * 2 + i * 3) % 10;
        draw_rect(wx, wy, 2, 4, lv_color_make(180, 180, 180));
    }
}

static void draw_pet_frame(void)
{
    canvas_clear();
    int cx = 60, cy = 65;

    /* Size depends on evolution stage */
    int base_size;
    lv_color_t body_col;
    switch (pet_stage) {
    case STAGE_BABY:  base_size = 18; body_col = C_BODY; break;
    case STAGE_TEEN:  base_size = 24; body_col = C_BODY; break;
    case STAGE_ADULT: base_size = 28; body_col = C_BODY; break;
    case STAGE_ELDER: base_size = 26; body_col = C_ELDER; break;
    default:          base_size = 18; body_col = C_BODY; break;
    }

    /* Bounce animation: slight y offset based on frame */
    int bounce = (anim_frame == 1 || anim_frame == 3) ? -2 : 0;
    cy += bounce;

    if (!pet_alive) {
        draw_pet_dead(cx, cy, base_size);
        return;
    }

    bool sleeping = (pet_activity == ACT_REST &&
                     (rest_phase == REST_DEEP || rest_phase == REST_ENTER));
    bool sick = (pet_mood == MOOD_SICK);

    if (sick) body_col = C_SICK;

    /* Draw in order: body → decoration → eyes → mouth → cheeks → overlay */
    draw_pet_body(cx, cy, base_size, body_col);
    draw_stage_decoration(cx, cy, base_size, pet_stage);
    draw_pet_eyes(cx, cy, base_size, anim_frame, sleeping, sick);

    if (!sleeping) {
        draw_pet_mouth(cx, cy, base_size, pet_mood);
        if (pet_mood == MOOD_HAPPY || pet_mood == MOOD_EXCITED)
            draw_pet_cheeks(cx, cy, base_size);
    }

    draw_activity_overlay(cx, cy, base_size, pet_activity, anim_frame);
}

/* ══════════════════════════════════════════════════════════════
 *  Callbacks: Manual feed / play / reset
 * ══════════════════════════════════════════════════════════════ */
static void feed_cb(lv_event_t *e)
{
    (void)e;
    if (!pet_alive) return;
    ESP_LOGI(TAG, "Manual feed → WiFi hunt");
    pet_activity = ACT_HUNT;
    start_wifi_scan();
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    if (!pet_alive) return;
    ESP_LOGI(TAG, "Manual play → Discover");
    pet_activity = ACT_DISCOVER;
    start_wifi_scan();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    reset_pet(true);
    ESP_LOGI(TAG, "Pet reset!");
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    app_manager_set_mode(MODE_MENU);
}

/* ══════════════════════════════════════════════════════════════
 *  Screen Create / Destroy / Update
 * ══════════════════════════════════════════════════════════════ */

/* Mood color for bar accent */
static lv_color_t mood_color(void)
{
    switch (pet_mood) {
    case MOOD_HAPPY:   return lv_color_make(100, 255, 100);
    case MOOD_EXCITED: return lv_color_make(255, 215, 0);
    case MOOD_HUNGRY:  return lv_color_make(255, 100, 50);
    case MOOD_SICK:    return lv_color_make(100, 180, 100);
    case MOOD_BORED:   return lv_color_make(150, 150, 150);
    case MOOD_CURIOUS: return lv_color_make(100, 200, 255);
    default:           return lv_color_make(180, 180, 255);
    }
}

void screen_tamafi_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_make(10, 10, 25), 0);
    lv_disp_load_scr(scr);

    /* ── Init timers ─────────────────────────────────────── */
    uint32_t now = ms_now();
    tmr_hunger = tmr_happiness = tmr_health = tmr_age = now;
    tmr_decision = tmr_autosave = tmr_anim = now;

    /* ── Mode indicator dots (top center) ────────────────── */
    int num_modes = 3;  /* clock, menu, settings (tamafi is off-cycle) */
    int dot_start_x = (LCD_H_RES - num_modes * 14) / 2;
    for (int i = 0; i < num_modes; i++) {
        mode_dots[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(mode_dots[i]);
        lv_obj_set_size(mode_dots[i], 6, 6);
        lv_obj_set_pos(mode_dots[i], dot_start_x + i * 14, 3);
        lv_obj_set_style_radius(mode_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(mode_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mode_dots[i], lv_color_make(60, 60, 60), 0);
    }

    /* ── Back button (top-left) ──────────────────────────── */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 56, 24);
    lv_obj_set_pos(back, 6, 2);
    lv_obj_set_style_bg_color(back, lv_color_make(50, 50, 70), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* ── Pet canvas (left side) ──────────────────────────── */
    canvas_buf = heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer in PSRAM");
        return;
    }
    pet_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(pet_canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(pet_canvas, 15, 28);
    canvas_clear();

    /* ── Activity label (top left) ───────────────────────── */
    lbl_activity = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_activity, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_text_font(lbl_activity, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_activity, 15, 14);
    lv_label_set_text(lbl_activity, "Idle");

    /* ── Stat bars (middle area) ─────────────────────────── */
    int bar_x = 160, bar_w = 140, bar_h = 12, bar_gap = 22;
    int bar_y_start = 30;

    /* Hunger */
    lbl_hunger = lv_label_create(scr);
    lv_label_set_text(lbl_hunger, LV_SYMBOL_WARNING " Hunger");
    lv_obj_set_style_text_color(lbl_hunger, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(lbl_hunger, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_hunger, bar_x, bar_y_start);

    bar_hunger = lv_bar_create(scr);
    lv_obj_set_size(bar_hunger, bar_w, bar_h);
    lv_obj_set_pos(bar_hunger, bar_x, bar_y_start + 12);
    lv_bar_set_range(bar_hunger, 0, 100);
    lv_bar_set_value(bar_hunger, pet.hunger, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_hunger, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_hunger, lv_color_make(255, 80, 60), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_hunger, 4, 0);
    lv_obj_set_style_radius(bar_hunger, 4, LV_PART_INDICATOR);

    /* Happiness */
    lbl_happy = lv_label_create(scr);
    lv_label_set_text(lbl_happy, LV_SYMBOL_OK " Happy");
    lv_obj_set_style_text_color(lbl_happy, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(lbl_happy, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_happy, bar_x, bar_y_start + bar_gap + 2);

    bar_happy = lv_bar_create(scr);
    lv_obj_set_size(bar_happy, bar_w, bar_h);
    lv_obj_set_pos(bar_happy, bar_x, bar_y_start + bar_gap + 14);
    lv_bar_set_range(bar_happy, 0, 100);
    lv_bar_set_value(bar_happy, pet.happiness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_happy, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_happy, lv_color_make(255, 220, 50), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_happy, 4, 0);
    lv_obj_set_style_radius(bar_happy, 4, LV_PART_INDICATOR);

    /* Health */
    lbl_health = lv_label_create(scr);
    lv_label_set_text(lbl_health, LV_SYMBOL_CHARGE " Health");
    lv_obj_set_style_text_color(lbl_health, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(lbl_health, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_health, bar_x, bar_y_start + 2 * bar_gap + 4);

    bar_health = lv_bar_create(scr);
    lv_obj_set_size(bar_health, bar_w, bar_h);
    lv_obj_set_pos(bar_health, bar_x, bar_y_start + 2 * bar_gap + 16);
    lv_bar_set_range(bar_health, 0, 100);
    lv_bar_set_value(bar_health, pet.health, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_health, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_health, lv_color_make(60, 220, 80), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_health, 4, 0);
    lv_obj_set_style_radius(bar_health, 4, LV_PART_INDICATOR);

    /* ── Percentage labels on bars ───────────────────────── */
    /* (reuse existing labels — append % in update) */

    /* ── Info panel (right side) ─────────────────────────── */
    int info_x = 330;

    lbl_mood = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_mood, lv_color_make(200, 200, 255), 0);
    lv_obj_set_style_text_font(lbl_mood, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_mood, info_x, 28);
    lv_label_set_text(lbl_mood, "Mood: Calm");

    lbl_stage = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_stage, lv_color_make(255, 215, 100), 0);
    lv_obj_set_style_text_font(lbl_stage, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_stage, info_x, 48);
    lv_label_set_text(lbl_stage, "Stage: Baby");

    lbl_age = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_age, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(lbl_age, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_age, info_x, 66);
    lv_label_set_text(lbl_age, "Age: 0d 0h 0m");

    lbl_wifi_env = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_wifi_env, lv_color_make(0, 180, 220), 0);
    lv_obj_set_style_text_font(lbl_wifi_env, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_wifi_env, info_x, 82);
    lv_label_set_text(lbl_wifi_env, LV_SYMBOL_WIFI " 0 nets");

    /* ── Action buttons (right bottom) ───────────────────── */
    /* Feed button */
    btn_feed = lv_btn_create(scr);
    lv_obj_set_size(btn_feed, 70, 28);
    lv_obj_set_pos(btn_feed, 330, 105);
    lv_obj_set_style_bg_color(btn_feed, lv_color_make(200, 60, 40), 0);
    lv_obj_set_style_radius(btn_feed, 6, 0);
    lv_obj_add_event_cb(btn_feed, feed_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_feed);
    lv_label_set_text(lbl, LV_SYMBOL_REFRESH " Feed");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    /* Play button */
    btn_play = lv_btn_create(scr);
    lv_obj_set_size(btn_play, 70, 28);
    lv_obj_set_pos(btn_play, 410, 105);
    lv_obj_set_style_bg_color(btn_play, lv_color_make(50, 130, 200), 0);
    lv_obj_set_style_radius(btn_play, 6, 0);
    lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_play);
    lv_label_set_text(lbl, LV_SYMBOL_PLAY " Play");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    /* Reset button (small, bottom right) */
    btn_reset = lv_btn_create(scr);
    lv_obj_set_size(btn_reset, 55, 24);
    lv_obj_set_pos(btn_reset, 490, 108);
    lv_obj_set_style_bg_color(btn_reset, lv_color_make(80, 80, 80), 0);
    lv_obj_set_style_radius(btn_reset, 4, 0);
    lv_obj_add_event_cb(btn_reset, reset_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_reset);
    lv_label_set_text(lbl, "Reset");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    /* ── Personality traits (far right) ──────────────────── */
    int trait_x = 500;
    lv_obj_t *trait_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(trait_lbl, lv_color_make(140, 140, 160), 0);
    lv_obj_set_style_text_font(trait_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(trait_lbl, trait_x, 28);
    lv_label_set_text_fmt(trait_lbl, "Curiosity: %d\nActivity:  %d\nStress:    %d",
                          trait_curiosity, trait_activity, trait_stress);

    /* ── Death overlay (hidden by default) ───────────────── */
    lbl_death = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_death, lv_color_make(255, 60, 60), 0);
    lv_obj_set_style_text_font(lbl_death, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_death, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_death, "Your pet has passed away...\nTap Reset to start over");
    lv_obj_add_flag(lbl_death, LV_OBJ_FLAG_HIDDEN);

    /* ── Decorative separator lines ──────────────────────── */
    lv_obj_t *sep1 = lv_obj_create(scr);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 1, 140);
    lv_obj_set_pos(sep1, 148, 18);
    lv_obj_set_style_bg_color(sep1, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);

    lv_obj_t *sep2 = lv_obj_create(scr);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, 1, 140);
    lv_obj_set_pos(sep2, 320, 18);
    lv_obj_set_style_bg_color(sep2, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);

    /* Initial draw */
    draw_pet_frame();

    ESP_LOGI(TAG, "TamaFi screen created");
}

void screen_tamafi_destroy(void)
{
    if (scr) {
        lv_obj_del(scr);
        scr = NULL;
    }
    pet_canvas  = NULL;
    if (canvas_buf) {
        heap_caps_free(canvas_buf);
        canvas_buf = NULL;
    }
    bar_hunger  = NULL;
    bar_happy   = NULL;
    bar_health  = NULL;
    lbl_hunger  = NULL;
    lbl_happy   = NULL;
    lbl_health  = NULL;
    lbl_mood    = NULL;
    lbl_stage   = NULL;
    lbl_age     = NULL;
    lbl_activity = NULL;
    lbl_wifi_env = NULL;
    lbl_death   = NULL;
    btn_feed    = NULL;
    btn_play    = NULL;
    btn_reset   = NULL;
}

void screen_tamafi_update(void)
{
    if (!scr) return;

    /* Run game logic */
    logic_tick();

    /* ── Animate pet ─────────────────────────────────────── */
    uint32_t now = ms_now();
    int anim_speed = ANIM_IDLE_MS;
    if (pet_mood == MOOD_EXCITED || pet_activity == ACT_HUNT) anim_speed = ANIM_FAST_MS;
    if (pet_mood == MOOD_BORED || pet_mood == MOOD_SICK) anim_speed = ANIM_SLOW_MS;

    if (now - tmr_anim >= (uint32_t)anim_speed) {
        tmr_anim = now;
        anim_frame = (anim_frame + 1) % ANIM_FRAMES;

        if (pet_canvas) {
            draw_pet_frame();
            lv_obj_invalidate(pet_canvas);
        }
    }

    /* ── Update stat bars ────────────────────────────────── */
    if (bar_hunger)  lv_bar_set_value(bar_hunger, pet.hunger, LV_ANIM_ON);
    if (bar_happy)   lv_bar_set_value(bar_happy, pet.happiness, LV_ANIM_ON);
    if (bar_health)  lv_bar_set_value(bar_health, pet.health, LV_ANIM_ON);

    /* Color-code bars based on values */
    if (bar_hunger) {
        lv_color_t c = pet.hunger < 25 ? lv_color_make(255, 40, 40) : lv_color_make(255, 80, 60);
        lv_obj_set_style_bg_color(bar_hunger, c, LV_PART_INDICATOR);
    }
    if (bar_health) {
        lv_color_t c = pet.health < 25 ? lv_color_make(255, 40, 40) : lv_color_make(60, 220, 80);
        lv_obj_set_style_bg_color(bar_health, c, LV_PART_INDICATOR);
    }

    /* ── Update labels ───────────────────────────────────── */
    if (lbl_hunger) lv_label_set_text_fmt(lbl_hunger, LV_SYMBOL_WARNING " Hunger %d%%", pet.hunger);
    if (lbl_happy)  lv_label_set_text_fmt(lbl_happy,  LV_SYMBOL_OK " Happy %d%%", pet.happiness);
    if (lbl_health) lv_label_set_text_fmt(lbl_health, LV_SYMBOL_CHARGE " Health %d%%", pet.health);

    if (lbl_mood) {
        lv_label_set_text_fmt(lbl_mood, "Mood: %s", mood_str(pet_mood));
        lv_obj_set_style_text_color(lbl_mood, mood_color(), 0);
    }
    if (lbl_stage)
        lv_label_set_text_fmt(lbl_stage, "Stage: %s", stage_str(pet_stage));
    if (lbl_age)
        lv_label_set_text_fmt(lbl_age, "Age: %lud %luh %lum",
                              (unsigned long)pet.age_days,
                              (unsigned long)pet.age_hours,
                              (unsigned long)pet.age_minutes);
    if (lbl_activity)
        lv_label_set_text(lbl_activity, activity_str(pet_activity));
    if (lbl_wifi_env)
        lv_label_set_text_fmt(lbl_wifi_env, LV_SYMBOL_WIFI " %d nets  RSSI:%d",
                              wifi_env.net_count, wifi_env.avg_rssi);

    /* ── Death overlay ───────────────────────────────────── */
    if (!pet_alive && lbl_death) {
        lv_obj_clear_flag(lbl_death, LV_OBJ_FLAG_HIDDEN);
        if (btn_feed) lv_obj_add_flag(btn_feed, LV_OBJ_FLAG_HIDDEN);
        if (btn_play) lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    } else if (pet_alive && lbl_death) {
        lv_obj_add_flag(lbl_death, LV_OBJ_FLAG_HIDDEN);
        if (btn_feed) lv_obj_clear_flag(btn_feed, LV_OBJ_FLAG_HIDDEN);
        if (btn_play) lv_obj_clear_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    }
}
