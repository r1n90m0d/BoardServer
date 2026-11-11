#ifndef STREAMER_LOG_H
#define STREAMER_LOG_H

/* 로그: "날짜 시각 [단조초] 수준 내용" 한 줄. path가 NULL이거나 열 수 없으면 stderr. */
int log_open(const char *path);
void log_close(void);
void log_write(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 단조 시계 기준 초(소수). 로그·감시 공통으로 쓴다. */
double mono_sec(void);

#define LOGI(...) log_write("I", __VA_ARGS__)
#define LOGW(...) log_write("W", __VA_ARGS__)
#define LOGE(...) log_write("E", __VA_ARGS__)

#endif
