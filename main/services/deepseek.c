/**
 * @file deepseek.c
 * @brief DeepSeek API client — chat completions (OpenAI-compatible REST).
 *
 * Also handles local intent classification for offline commands.
 */

#include "deepseek.h"
#include "task_manager.h"
#include "secrets.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "deepseek";

#define LLM_URL         "https://api.groq.com/openai/v1/chat/completions"
#define LLM_MODEL       "llama-3.3-70b-versatile"
#define MAX_RESP_BUF    (8 * 1024)

#define SYSTEM_PROMPT \
    "You are a smartwatch productivity assistant. " \
    "Keep responses under 30 words. Be direct and actionable. " \
    "You can help manage tasks, timers, and provide brief info."

volatile deepseek_result_t g_ds_result = {0};
volatile bool              g_ds_busy   = false;

/* ── HTTP response accumulator ────────────────────────────── */
typedef struct {
    char  *buf;
    int    len;
    int    cap;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int new_len = ctx->len + evt->data_len;
        if (new_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len = new_len;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Local intent matching ────────────────────────────────── */
static bool str_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    /* Case-insensitive substring search */
    int nlen = strlen(needle);
    int hlen = strlen(hay);
    for (int i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

bool deepseek_local_intent(const char *transcript)
{
    if (!transcript) return false;

    if (str_contains(transcript, "start timer") ||
        str_contains(transcript, "begin timer"))
    {
        task_manager_timer_start();
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "Timer started for: %s", g_tasks.items[g_tasks.current].title);
        g_ds_result.ok = true;
        return true;
    }

    if (str_contains(transcript, "stop timer") ||
        str_contains(transcript, "pause timer"))
    {
        task_manager_timer_pause();
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX, "Timer paused.");
        g_ds_result.ok = true;
        return true;
    }

    if (str_contains(transcript, "snooze")) {
        task_manager_timer_snooze();
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "Snoozed +5 min.");
        g_ds_result.ok = true;
        return true;
    }

    if (str_contains(transcript, "next task")) {
        task_manager_next();
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "Now: %s", g_tasks.items[g_tasks.current].title);
        g_ds_result.ok = true;
        return true;
    }

    if (str_contains(transcript, "done") ||
        str_contains(transcript, "complete"))
    {
        task_manager_complete_current();
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "Done! Now: %s", g_tasks.items[g_tasks.current].title);
        g_ds_result.ok = true;
        return true;
    }

    if (str_contains(transcript, "what time")) {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "It's %02d:%02d", ti.tm_hour, ti.tm_min);
        g_ds_result.ok = true;
        return true;
    }

    return false;  /* Not a local command — need API */
}

/* ── DeepSeek API call ────────────────────────────────────── */
void deepseek_query(const char *user_msg)
{
    g_ds_busy = true;
    g_ds_result.ok = false;
    g_ds_result.text[0] = '\0';

    /* Try local intent first */
    if (deepseek_local_intent(user_msg)) {
        g_ds_busy = false;
        return;
    }

    /* Build JSON body */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", LLM_MODEL);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", SYSTEM_PROMPT);
    cJSON_AddItemToArray(msgs, sys_msg);

    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_msg);
    cJSON_AddItemToArray(msgs, usr_msg);

    cJSON_AddNumberToObject(root, "max_tokens", 100);
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX, "Error: JSON build failed");
        g_ds_busy = false;
        return;
    }

    /* Allocate response buffer in PSRAM */
    resp_ctx_t ctx = {
        .buf = heap_caps_malloc(MAX_RESP_BUF, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = MAX_RESP_BUF - 1,
    };
    if (!ctx.buf) {
        free(body);
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX, "Error: no memory");
        g_ds_busy = false;
        return;
    }
    ctx.buf[0] = '\0';

    /* Build auth header */
    char auth_hdr[128];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", GROQ_API_KEY);

    esp_http_client_config_t cfg = {
        .url               = LLM_URL,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP error: %s, status=%d", esp_err_to_name(err), status);
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                 "API error (%d)", status);
        heap_caps_free(ctx.buf);
        g_ds_busy = false;
        return;
    }

    /* Parse response JSON */
    cJSON *resp = cJSON_Parse(ctx.buf);
    heap_caps_free(ctx.buf);

    if (!resp) {
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX, "JSON parse error");
        g_ds_busy = false;
        return;
    }

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *first   = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (cJSON_IsString(content)) {
            snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                     "%s", content->valuestring);
            g_ds_result.ok = true;
        }
    }

    cJSON_Delete(resp);
    if (!g_ds_result.ok) {
        snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX, "No response content");
    }

    g_ds_busy = false;
    ESP_LOGI(TAG, "LLM response: %.60s...", g_ds_result.text);
}
