#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE *g_fp;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

double mono_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int log_open(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return 0;
	g_fp = fopen(path, "a");
	if (g_fp == NULL)
		return -1;
	setvbuf(g_fp, NULL, _IOLBF, 0);
	return 0;
}

void log_close(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_fp != NULL) {
		fclose(g_fp);
		g_fp = NULL;
	}
	pthread_mutex_unlock(&g_lock);
}

void log_write(const char *level, const char *fmt, ...)
{
	FILE *out;
	va_list ap;
	time_t now = time(NULL);
	struct tm tm;
	char stamp[32];

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

	pthread_mutex_lock(&g_lock);
	out = g_fp != NULL ? g_fp : stderr;
	fprintf(out, "%s [%10.3f] %s ", stamp, mono_sec(), level);
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
	pthread_mutex_unlock(&g_lock);
}
