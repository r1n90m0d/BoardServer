#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

enum key_type { KEY_INT, KEY_STR };

struct key_desc {
	const char *name;
	enum key_type type;
	size_t offset;
	size_t size; /* KEY_STR 일 때 버퍼 크기 */
	int min;
	int max;
	const char *help;
};

#define K_INT(field, lo, hi, text) \
	{ #field, KEY_INT, offsetof(struct config, field), 0, lo, hi, text }
#define K_STR(field, text) \
	{ #field, KEY_STR, offsetof(struct config, field), sizeof(((struct config *)0)->field), 0, 0, text }

static const struct key_desc g_keys[] = {
	K_STR(iq_dir, "rkaiq IQ json 디렉토리"),
	K_STR(log_file, "로그 파일 (비우면 stderr)"),
	K_INT(rtsp_port, 1, 65535, "RTSP 포트"),
	K_INT(fps, 1, 30, "실제 목표 fps (인코더 프레임레이트)"),
	K_INT(ae_fps_x10, 0, 300, "rkaiq AE 고정 fps x10 (센서 표기 기준, 0 = 고정 안 함)"),
	K_INT(raw_buf_cnt, 2, 4, "CIF raw 버퍼 수"),
	K_INT(vi_buf_cnt, 2, 6, "메인 VI 버퍼 수"),
	K_INT(vi_depth, 0, 6, "메인 VI Depth"),
	K_INT(main_width, 320, 3840, "메인 스트림 폭"),
	K_INT(main_height, 240, 2160, "메인 스트림 높이"),
	K_INT(main_kbps, 64, 20000, "메인 스트림 비트레이트(kbps, CBR)"),
	K_INT(main_gop, 1, 300, "메인 스트림 GOP(프레임 수)"),
	K_INT(sub_enable, 0, 1, "서브 스트림(/live/1) 사용"),
	K_INT(sub_vi_buf_cnt, 2, 6, "서브 VI 버퍼 수"),
	K_INT(sub_vi_depth, 0, 6, "서브 VI Depth"),
	K_INT(sub_width, 160, 1920, "서브 스트림 폭"),
	K_INT(sub_height, 120, 1080, "서브 스트림 높이"),
	K_INT(sub_kbps, 64, 20000, "서브 스트림 비트레이트(kbps, CBR)"),
	K_INT(sub_gop, 1, 300, "서브 스트림 GOP(프레임 수)"),
	K_INT(osd_enable, 0, 1, "날짜·시각 OSD"),
	K_INT(stall_timeout_s, 2, 600, "정지 판정 시간(초)"),
	K_INT(stall_grace_s, 0, 600, "시작·재초기화 뒤 판정 유예(초)"),
	K_STR(stall_action, "정지 때 동작: log 또는 reinit"),
	K_INT(reinit_max, 0, 100, "정지 때 재초기화 시도 횟수 상한"),
	K_INT(reinit_timeout_s, 5, 600, "재초기화 제한 시간(초)"),
	K_STR(status_file, "상태 파일 (비우면 쓰지 않음)"),
};

#define KEY_COUNT (sizeof(g_keys) / sizeof(g_keys[0]))

void config_defaults(struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->iq_dir, sizeof(cfg->iq_dir), "%s", "/oem/usr/share/iqfiles");
	cfg->rtsp_port = 554;
	cfg->fps = 18;
	cfg->ae_fps_x10 = 120;
	cfg->raw_buf_cnt = 3;
	cfg->vi_buf_cnt = 3;
	cfg->vi_depth = 1;
	cfg->main_width = 1920;
	cfg->main_height = 1080;
	cfg->main_kbps = 4096;
	cfg->main_gop = 18;
	cfg->sub_enable = 1;
	cfg->sub_vi_buf_cnt = 2;
	cfg->sub_vi_depth = 0;
	cfg->sub_width = 960;
	cfg->sub_height = 540;
	cfg->sub_kbps = 1024;
	cfg->sub_gop = 18;
	cfg->osd_enable = 1;
	cfg->stall_timeout_s = 5;
	cfg->stall_grace_s = 15;
	snprintf(cfg->stall_action, sizeof(cfg->stall_action), "%s", "reinit");
	cfg->reinit_max = 3;
	cfg->reinit_timeout_s = 30;
	snprintf(cfg->status_file, sizeof(cfg->status_file), "%s", "/tmp/streamer.status");
}

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static int apply(struct config *cfg, const char *key, const char *value)
{
	size_t i;

	for (i = 0; i < KEY_COUNT; i++) {
		const struct key_desc *k = &g_keys[i];
		char *base = (char *)cfg + k->offset;

		if (strcmp(k->name, key) != 0)
			continue;
		if (k->type == KEY_STR) {
			if (strlen(value) >= k->size) {
				LOGE("config: %s 값이 너무 김", key);
				return -1;
			}
			snprintf(base, k->size, "%s", value);
			return 0;
		} else {
			char *end = NULL;
			long v;

			errno = 0;
			v = strtol(value, &end, 10);
			if (errno != 0 || end == value || *end != '\0' || v < k->min || v > k->max) {
				LOGE("config: %s=%s 는 범위(%d~%d)를 벗어남", key, value, k->min, k->max);
				return -1;
			}
			*(int *)(void *)base = (int)v;
			return 0;
		}
	}
	LOGE("config: 모르는 키 '%s'", key);
	return -1;
}

int config_set(struct config *cfg, const char *assignment)
{
	char buf[256];
	char *eq;

	if (strlen(assignment) >= sizeof(buf)) {
		LOGE("config: 항목이 너무 김");
		return -1;
	}
	snprintf(buf, sizeof(buf), "%s", assignment);
	eq = strchr(buf, '=');
	if (eq == NULL) {
		LOGE("config: '%s' 는 key=value 형식이 아님", assignment);
		return -1;
	}
	*eq = '\0';
	return apply(cfg, trim(buf), trim(eq + 1));
}

int config_load_file(struct config *cfg, const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	int lineno = 0;
	int ret = 0;

	if (fp == NULL) {
		LOGE("config: %s 를 열 수 없음: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *s;
		char *hash;

		lineno++;
		hash = strpbrk(line, "#;");
		if (hash != NULL)
			*hash = '\0';
		s = trim(line);
		if (*s == '\0')
			continue;
		if (config_set(cfg, s) != 0) {
			LOGE("config: %s %d번째 줄 오류", path, lineno);
			ret = -1;
		}
	}
	fclose(fp);
	return ret;
}

int config_validate(const struct config *cfg, char *err, int err_len)
{
	if (cfg->vi_depth > cfg->vi_buf_cnt) {
		snprintf(err, (size_t)err_len, "vi_depth(%d)가 vi_buf_cnt(%d)보다 큼", cfg->vi_depth,
		         cfg->vi_buf_cnt);
		return -1;
	}
	if ((cfg->main_width % 2) != 0 || (cfg->main_height % 2) != 0) {
		snprintf(err, (size_t)err_len, "main_width/main_height 는 짝수여야 함");
		return -1;
	}
	if (strcmp(cfg->stall_action, "log") != 0 && strcmp(cfg->stall_action, "reinit") != 0) {
		snprintf(err, (size_t)err_len, "stall_action 은 log 또는 reinit 이어야 함");
		return -1;
	}
	if (cfg->sub_enable) {
		if (cfg->sub_vi_depth > cfg->sub_vi_buf_cnt) {
			snprintf(err, (size_t)err_len, "sub_vi_depth(%d)가 sub_vi_buf_cnt(%d)보다 큼",
			         cfg->sub_vi_depth, cfg->sub_vi_buf_cnt);
			return -1;
		}
		if ((cfg->sub_width % 2) != 0 || (cfg->sub_height % 2) != 0) {
			snprintf(err, (size_t)err_len, "sub_width/sub_height 는 짝수여야 함");
			return -1;
		}
		if (cfg->sub_width > cfg->main_width || cfg->sub_height > cfg->main_height) {
			snprintf(err, (size_t)err_len, "서브 해상도가 메인보다 큼");
			return -1;
		}
	}
	return 0;
}

void config_log(const struct config *cfg)
{
	size_t i;

	for (i = 0; i < KEY_COUNT; i++) {
		const struct key_desc *k = &g_keys[i];
		const char *base = (const char *)cfg + k->offset;

		if (k->type == KEY_STR)
			LOGI("config: %s = %s", k->name, base);
		else
			LOGI("config: %s = %d", k->name, *(const int *)(const void *)base);
	}
}

void config_print_keys(void)
{
	struct config def;
	size_t i;

	config_defaults(&def);
	for (i = 0; i < KEY_COUNT; i++) {
		const struct key_desc *k = &g_keys[i];
		const char *base = (const char *)&def + k->offset;

		if (k->type == KEY_STR)
			printf("  %-18s %s (기본: %s)\n", k->name, k->help, base[0] ? base : "없음");
		else
			printf("  %-18s %s (기본: %d, 범위 %d~%d)\n", k->name, k->help,
			       *(const int *)(const void *)base, k->min, k->max);
	}
}
