/**
 * @file deepseek.h
 * @brief DeepSeek chat completions API client (OpenAI-compatible).
 */
#ifndef DEEPSEEK_H
#define DEEPSEEK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEEPSEEK_RESP_MAX  512

/** Result of a DeepSeek query. */
typedef struct {
    bool ok;
    char text[DEEPSEEK_RESP_MAX];
} deepseek_result_t;

/** Last result — updated by deepseek_query(). */
extern volatile deepseek_result_t g_ds_result;
extern volatile bool              g_ds_busy;

/**
 * Send a chat completion request to DeepSeek API.
 * Runs synchronously (blocks). Call from a task with 4096+ stack.
 * Result stored in g_ds_result.
 */
void deepseek_query(const char *user_msg);

/**
 * Parse a voice transcript for local commands.
 * Returns true if handled locally (no API call needed).
 */
bool deepseek_local_intent(const char *transcript);

#ifdef __cplusplus
}
#endif

#endif /* DEEPSEEK_H */
