/*
 * streamer: RKMPI 기반 최소 RTSP 스트리밍 프로그램 (rkipc 대체용).
 * 설계는 docs/streamer/design.md 참고.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "isp.h"
#include "log.h"
#include "rtsp.h"
#include "video.h"

#define STREAMER_VERSION "0.1-stage1"

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void usage(const char *prog)
{
	printf("사용법: %s [-c 설정파일] [-l 로그파일] [-o key=value]... [-h]\n", prog);
	printf("  -c 파일        key = value 형식 설정 파일\n");
	printf("  -l 파일        로그 파일 (log_file 과 같음, 기본 stderr)\n");
	printf("  -o key=value   설정 한 건 지정 (여러 번 가능, 설정 파일보다 우선)\n");
	printf("설정 키:\n");
	config_print_keys();
}

static int pipeline_start(const struct config *cfg)
{
	if (isp_start(cfg) != 0)
		return -1;
	if (video_start(cfg) != 0) {
		isp_stop();
		return -1;
	}
	return 0;
}

static void pipeline_stop(void)
{
	video_stop();
	isp_stop();
}

int main(int argc, char **argv)
{
	struct config cfg;
	struct sigaction sa;
	const char *overrides[32];
	int n_over = 0;
	const char *conf_path = NULL;
	const char *log_path = NULL;
	char err[128];
	int opt;
	int i;

	config_defaults(&cfg);

	while ((opt = getopt(argc, argv, "c:l:o:h")) != -1) {
		switch (opt) {
		case 'c':
			conf_path = optarg;
			break;
		case 'l':
			log_path = optarg;
			break;
		case 'o':
			if (n_over >= (int)(sizeof(overrides) / sizeof(overrides[0]))) {
				fprintf(stderr, "-o 가 너무 많음\n");
				return 2;
			}
			overrides[n_over++] = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (conf_path != NULL && config_load_file(&cfg, conf_path) != 0)
		return 2;
	for (i = 0; i < n_over; i++) {
		if (config_set(&cfg, overrides[i]) != 0)
			return 2;
	}
	if (log_path != NULL)
		snprintf(cfg.log_file, sizeof(cfg.log_file), "%s", log_path);
	if (config_validate(&cfg, err, (int)sizeof(err)) != 0) {
		fprintf(stderr, "설정 오류: %s\n", err);
		return 2;
	}
	if (log_open(cfg.log_file) != 0)
		fprintf(stderr, "로그 파일 %s 를 열 수 없어 stderr 로 기록함\n", cfg.log_file);

	LOGI("streamer %s 시작 (pid %d)", STREAMER_VERSION, (int)getpid());
	config_log(&cfg);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (rtsp_srv_start(cfg.rtsp_port) != 0)
		return 1;
	if (rtsp_srv_add_session(0, "/live/0", 0) != 0) {
		rtsp_srv_stop();
		return 1;
	}

	if (pipeline_start(&cfg) != 0) {
		LOGE("파이프라인 시작 실패");
		rtsp_srv_stop();
		log_close();
		return 1;
	}
	LOGI("송출 시작");

	while (!g_quit)
		usleep(200 * 1000);

	LOGI("종료 요청, 해제 시작 (메인 %llu 프레임 송출)",
	     (unsigned long long)video_frames_out(0));
	pipeline_stop();
	rtsp_srv_stop();
	LOGI("종료");
	log_close();
	return 0;
}
