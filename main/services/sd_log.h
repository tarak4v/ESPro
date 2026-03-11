#ifndef SD_LOG_H
#define SD_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/** Mount SPIFFS storage and create log file. */
void sd_log_init(void);

/** Append a timestamped message to the log file. */
void sd_log_write(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* SD_LOG_H */
