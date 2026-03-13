/**
 * @file screen_more.c
 * @brief Third swipeable screen — overflow/utility app cards
 *        (Macropad, Pomodoro, Settings).
 */

#include "screen_more.h"
#include "app_manager.h"
#include "hw_config.h"
#include "screen_settings.h"
#include "macropad.h"
#include "pomodoro.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "more";

static lv_obj_t *scr = NULL;

/* ── App entries ──────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *icon;
    uint32_t    color;
} more_entry_t;

static const more_entry_t apps[] = {
    { "Macropad", LV_SYMBOL_KEYBOARD, 0x37474F },
    { "Pomodoro", LV_SYMBOL_BELL,     0xBF360C },
    { "Settings", LV_SYMBOL_SETTINGS, 0x546E7A },
};
#define APP_COUNT (sizeof(apps) / sizeof(apps[0]))

/* ── Styles ───────────────────────────────────────────────── */
static lv_style_t style_bg, style_card, style_icon, style_name;
static lv_style_t style_dot_active, style_dot_inactive;
static lv_obj_t  *mode_dot[3];

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_card);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_pad_all(&style_card, 6);
    lv_style_set_width(&style_card, 80);
    lv_style_set_height(&style_card, 100);

    lv_style_init(&style_icon);
    lv_style_set_text_color(&style_icon, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_icon, &lv_font_montserrat_28);

    lv_style_init(&style_name);
    lv_style_set_text_color(&style_name, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_name, &lv_font_montserrat_12);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(0xFF8844));
    lv_style_set_bg_opa(&style_dot_active, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_active, 8);
    lv_style_set_height(&style_dot_active, 8);

    lv_style_init(&style_dot_inactive);
    lv_style_set_bg_color(&style_dot_inactive, lv_color_hex(th_btn));
    lv_style_set_bg_opa(&style_dot_inactive, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_inactive, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_inactive, 6);
    lv_style_set_height(&style_dot_inactive, 6);
}

/* ── Card click handler ───────────────────────────────────── */
static void card_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "More app tapped: %s (#%d)", apps[idx].name, (int)idx);
    switch (idx) {
        case 0: macropad_open(scr);     break;
        case 1: pomodoro_open(scr);     break;
        case 2: app_manager_set_mode(MODE_SETTINGS); break;
        default: break;
    }
}

/* ── Periodic update ──────────────────────────────────────── */
void screen_more_update(void)
{
    if (!scr) return;

    if (pomodoro_is_active()) {
        pomodoro_update();
    }
}

/* ── Create ───────────────────────────────────────────────── */
void screen_more_create(void)
{
    init_styles();
    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "More");
    lv_obj_set_style_text_color(title, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Card row — centred */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 620, 110);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < APP_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(apps[i].color), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, apps[i].icon);
        lv_obj_add_style(icon, &style_icon, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, apps[i].name);
        lv_obj_add_style(name, &style_name, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    /* Page dots (3) — dot 2 active for "More" */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 50, 12);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 2)
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        else
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
    }

    g_pending_scr = scr;
    ESP_LOGI(TAG, "More screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_more_destroy(void)
{
    if (scr) {
        macropad_close();
        pomodoro_close();
        for (int i = 0; i < 3; i++) mode_dot[i] = NULL;
        lv_obj_del(scr);
        scr = NULL;
    }
}
