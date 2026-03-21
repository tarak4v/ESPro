/**
 * @file task_manager.c
 * @brief Task list with timer, persisted to /log/tasks.json on LittleFS.
 */

#include "task_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "task_mgr";
#define TASKS_PATH "/log/tasks.json"

task_list_t g_tasks = {0};

/* ── LittleFS persistence ─────────────────────────────────── */
static void load_from_file(void)
{
    FILE *f = fopen(TASKS_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No saved tasks — starting empty");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }

    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return; }

    cJSON *arr = cJSON_GetObjectItem(root, "tasks");
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return; }

    g_tasks.count   = 0;
    g_tasks.current = cur ? (uint8_t)cur->valueint : 0;
    int n = cJSON_GetArraySize(arr);
    if (n > TASK_MAX_ITEMS) n = TASK_MAX_ITEMS;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *jt   = cJSON_GetObjectItem(item, "title");
        cJSON *jd   = cJSON_GetObjectItem(item, "dur");
        cJSON *jdn  = cJSON_GetObjectItem(item, "done");
        if (!jt || !cJSON_IsString(jt)) continue;

        hud_task_t *t = &g_tasks.items[g_tasks.count];
        t->id = g_tasks.count;
        snprintf(t->title, TASK_TITLE_LEN, "%s", jt->valuestring);
        t->duration_min = jd ? (uint16_t)jd->valueint : 25;
        t->done         = jdn ? cJSON_IsTrue(jdn) : false;
        g_tasks.count++;
    }

    if (g_tasks.current >= g_tasks.count)
        g_tasks.current = 0;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d tasks, current=%d", g_tasks.count, g_tasks.current);
}

void task_manager_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "tasks");
    cJSON_AddNumberToObject(root, "current", g_tasks.current);

    for (int i = 0; i < g_tasks.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "title", g_tasks.items[i].title);
        cJSON_AddNumberToObject(item, "dur",   g_tasks.items[i].duration_min);
        cJSON_AddBoolToObject(item, "done",    g_tasks.items[i].done);
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    FILE *f = fopen(TASKS_PATH, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
}

/* ── Public API ───────────────────────────────────────────── */
void task_manager_init(void)
{
    memset(&g_tasks, 0, sizeof(g_tasks));
    g_tasks.timer_remain_sec = -1;
    load_from_file();

    /* If no tasks loaded, seed some defaults */
    if (g_tasks.count == 0) {
        task_manager_add("Focus Work", 25);
        task_manager_add("Short Break", 5);
        task_manager_add("Code Review", 25);
        task_manager_save();
    }
}

int task_manager_add(const char *title, uint16_t duration_min)
{
    if (g_tasks.count >= TASK_MAX_ITEMS) return -1;
    hud_task_t *t = &g_tasks.items[g_tasks.count];
    t->id           = g_tasks.count;
    snprintf(t->title, TASK_TITLE_LEN, "%s", title);
    t->duration_min = duration_min;
    t->done         = false;
    return g_tasks.count++;
}

void task_manager_complete_current(void)
{
    if (g_tasks.count == 0) return;
    g_tasks.items[g_tasks.current].done = true;
    g_tasks.timer_running    = false;
    g_tasks.timer_remain_sec = -1;
    task_manager_next();
    task_manager_save();
}

void task_manager_next(void)
{
    if (g_tasks.count == 0) return;
    for (int i = 1; i <= g_tasks.count; i++) {
        uint8_t idx = (g_tasks.current + i) % g_tasks.count;
        if (!g_tasks.items[idx].done) {
            g_tasks.current = idx;
            g_tasks.timer_remain_sec = -1;
            g_tasks.timer_running    = false;
            return;
        }
    }
    /* All done — stay on last */
}

void task_manager_timer_start(void)
{
    if (g_tasks.count == 0) return;
    if (g_tasks.timer_remain_sec <= 0) {
        g_tasks.timer_remain_sec =
            g_tasks.items[g_tasks.current].duration_min * 60;
    }
    g_tasks.timer_running = true;
}

void task_manager_timer_pause(void)
{
    g_tasks.timer_running = false;
}

void task_manager_timer_snooze(void)
{
    g_tasks.timer_remain_sec += 300;  /* +5 minutes */
    g_tasks.timer_running = true;
    ESP_LOGI(TAG, "Timer snoozed +5min → %ld s", (long)g_tasks.timer_remain_sec);
}

void task_manager_timer_tick(void)
{
    if (!g_tasks.timer_running || g_tasks.timer_remain_sec <= 0) return;
    g_tasks.timer_remain_sec--;
    if (g_tasks.timer_remain_sec <= 0) {
        g_tasks.timer_running = false;
        ESP_LOGI(TAG, "Timer expired for task: %s",
                 g_tasks.items[g_tasks.current].title);
    }
}

void task_manager_timer_str(char *buf, int buf_len)
{
    if (g_tasks.timer_remain_sec <= 0) {
        snprintf(buf, buf_len, "--:--");
        return;
    }
    int m = g_tasks.timer_remain_sec / 60;
    int s = g_tasks.timer_remain_sec % 60;
    snprintf(buf, buf_len, "%02d:%02d", m, s);
}
