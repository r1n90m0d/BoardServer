#ifndef STREAMER_CONFIG_H
#define STREAMER_CONFIG_H

/*
 * 설정. 우선순위: 명령행(-o key=value) > 설정 파일(-c) > 기본값.
 * 키 목록과 기본값은 config.c 의 표와 streamer.conf.example 참고.
 */
struct config {
	char iq_dir[128];   /* rkaiq IQ json 디렉토리 */
	char log_file[128]; /* 비어 있으면 stderr */
	int rtsp_port;
	int fps;            /* 실제 목표 fps. VENC 프레임레이트에 쓴다 */
	int ae_fps_x10;     /* rkaiq AE 에 넘기는 고정 fps x10 (0 = 고정 안 함). isp.c 설명 참고 */
	int raw_buf_cnt;    /* CIF raw 버퍼 수 (rkaiq rkraw_rx) */
	int vi_buf_cnt;     /* 메인 VI 버퍼 수 */
	int vi_depth;       /* 메인 VI Depth */
	int main_width;
	int main_height;
	int main_kbps;
	int main_gop;
	int sub_enable;     /* 서브 채널(/live/1) 사용 */
	int sub_vi_buf_cnt;
	int sub_vi_depth;
	int sub_width;
	int sub_height;
	int sub_kbps;
	int sub_gop;
	int osd_enable;     /* 날짜·시각 OSD */
	int stall_timeout_s;   /* 프레임 카운터가 이 시간 동안 늘지 않으면 정지 */
	int stall_grace_s;     /* 시작·재초기화 직후 판정하지 않는 시간 */
	char stall_action[16]; /* "log" 또는 "reinit" */
	int reinit_max;        /* 정지 때 재초기화를 시도하는 횟수 상한 */
	int reinit_timeout_s;  /* 재초기화가 이 시간 안에 끝나지 않으면 failed */
	char status_file[128]; /* 상태 파일 (비우면 쓰지 않음) */
};

void config_defaults(struct config *cfg);
/* 파일의 "key = value" 줄을 읽는다. '#', ';' 뒤는 주석. 실패하면 -1. */
int config_load_file(struct config *cfg, const char *path);
/* "key=value" 한 건을 적용한다. 모르는 키나 범위를 벗어난 값이면 -1. */
int config_set(struct config *cfg, const char *assignment);
/* 값들 사이의 관계를 검사한다. 문제가 있으면 메시지를 err에 쓰고 -1. */
int config_validate(const struct config *cfg, char *err, int err_len);
void config_log(const struct config *cfg);
void config_print_keys(void);

#endif
