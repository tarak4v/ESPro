/**
 * @file task_manager.h
 * @brief Task list management with timers, persisted to LittleFS JSON.
 */
#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_MAX_ITEMS   16
#define TASK_TITLE_LEN   48

typedef struct {
    uint8_t  id;
    char     title[TASK_TITLE_LEN];
    uint16_t duration_min;       /* planned duration in minutes */
    bool     done;
} hud_task_t;

typedef struct {
    hud_task_t items[TASK_MAX_ITEMS];
    uint8_t    count;
    uint8_t    current;          /* index of active task */
    int32_t    timer_remain_sec; /* countdown seconds for current task (-1 = no timer) */
    bool       timer_running;
} task_list_t;

/** Global task list state */
extern task_list_t g_tasks;

/** Initialise task manager — load tasks from LittleFS */
void task_manager_init(void);

/** Save current task list to LittleFS */
void task_manager_save(void);

/** Add a task. Returns index or -1 on failure. */
int  task_manager_add(const char *title, uint16_t duration_min);

/** Mark current task as done, advance to next. */
void task_manager_complete_current(void);

/** Move to the next undone task. */
void task_manager_next(void);

/** Start/resume timer for current task. */
void task_manager_timer_start(void);

/** Pause timer. */
void task_manager_timer_pause(void);

/** Snooze timer (add 5 minutes). */
void task_manager_timer_snooze(void);

/** Tick the timer — call once per second. */
void task_manager_timer_tick(void);

/** Get formatted timer string "MM:SS" into buf (at least 6 bytes). */
void task_manager_timer_str(char *buf, int buf_len);

#ifdef __cplusplus
}
#endif

#endif /* TASK_MANAGER_H */
