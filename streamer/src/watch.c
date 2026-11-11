#include "watch.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "log.h"
#include "video.h"

#define CIF_PROC "/proc/rkcif-mipi-lvds"
#define FPS_WINDOW 10          /* 평균 fps 를 내는 구간(초) */
#define DEGRADED_RATIO 0.7     /* 평균 입력 fps 가 목표의 이 비율보다 낮으면 degraded */
#define QUIT_DEADLINE_S 15.0   /* 종료 요청 뒤 해제를 기다리는 시간 */

enum state { ST_STARTING, ST_RUNNING, ST_DEGRADED, ST_STALLED, ST_REINIT, ST_FAILED };

static const char *const g_state_name[] = { "starting", "running", "degraded",
	                                        "stalled",  "reinit",  "failed" };

struct cif_info {
	long frames;    /* frame amount, 읽지 못하면 -1 */
	int buf_in_drv; /* buf_cnt in drv (스트림 0), 없으면 -1 */
	int buf_total;  /* total buf_cnt (스트림 0), 없으면 -1 */
};

struct sample {
	double t;
	long cif;
	unsigned long long v0;
	unsigned long long v1;
};

static struct config g_cfg;
static const volatile sig_atomic_t *g_quit;
static pthread_t g_thread;
static atomic_int g_run;
static atomic_int g_reinit_req;      /* main 이 가져갈 요청 */
static atomic_int g_reinit_active;   /* main 이 재초기화 중 */
static atomic_int g_reinit_result;   /* 0 없음, 1 성공, 2 실패 (감시 스레드가 가져감) */
static atomic_int g_manual_req;      /* SIGUSR1 */
static atomic_int g_fake_stall;      /* SIGUSR2: 카운터가 멈춘 것으로 취급(시험용) */

static void read_cif(struct cif_info *ci)
{
	FILE *fp = fopen(CIF_PROC, "r");
	char line[160];

	ci->frames = -1;
	ci->buf_in_drv = -1;
	ci->buf_total = -1;
	if (fp == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *p;

		if ((p = strstr(line, "frame amount:")) != NULL)
			ci->frames = strtol(p + strlen("frame amount:"), NULL, 10);
		else if ((p = strstr(line, "buf_cnt in drv:")) != NULL)
			ci->buf_in_drv = (int)strtol(p + strlen("buf_cnt in drv:"), NULL, 10);
		else if ((p = strstr(line, "total buf_cnt:")) != NULL)
			ci->buf_total = (int)strtol(p + strlen("total buf_cnt:"), NULL, 10);
	}
	fclose(fp);
}

static void write_status(enum state st, double now, const struct sample *cur, double cif_fps,
                         double v0_fps, double v1_fps, const struct cif_info *ci, int stalls,
                         int reinits, int reinit_ok)
{
	char tmp[160];
	FILE *fp;

	if (g_cfg.status_file[0] == '\0')
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.status_file);
	fp = fopen(tmp, "w");
	if (fp == NULL)
		return;
	fprintf(fp, "state=%s\n", g_state_name[st]);
	fprintf(fp, "uptime=%.1f\n", now);
	fprintf(fp, "cif_frames=%ld\nvenc0_frames=%llu\nvenc1_frames=%llu\n", cur->cif, cur->v0,
	        cur->v1);
	fprintf(fp, "cif_fps=%.1f\nvenc0_fps=%.1f\nvenc1_fps=%.1f\n", cif_fps, v0_fps, v1_fps);
	fprintf(fp, "cif_buf_in_drv=%d\ncif_buf_total=%d\n", ci->buf_in_drv, ci->buf_total);
	fprintf(fp, "stalls=%d\nreinits=%d\nreinit_ok=%d\n", stalls, reinits, reinit_ok);
	fclose(fp);
	rename(tmp, g_cfg.status_file);
}

static void *watch_thread(void *arg)
{
	struct sample ring[FPS_WINDOW + 1];
	int ring_len = 0;
	enum state st = ST_STARTING;
	double phase_start = mono_sec(); /* 시작 또는 마지막 재초기화 완료 시각 */
	double cif_changed = phase_start;
	double v0_changed = phase_start;
	double reinit_started = 0.0;
	double quit_seen = 0.0;
	long last_cif = -2;
	unsigned long long last_v0 = 0;
	int stalls = 0;
	int reinits = 0;
	int reinit_ok = 0;
	int after_reinit = 0;     /* 재초기화 뒤 첫 판정을 기다리는 중 */
	int stall_reported = 0;   /* 지금의 정지를 이미 기록했는지 */
	int hung_reported = 0;
	int fake_logged = 0;
	int first = 1;

	(void)arg;
	prctl(PR_SET_NAME, "st_watch", 0, 0, 0);

	while (atomic_load(&g_run)) {
		struct cif_info ci;
		struct sample cur;
		double cif_fps = 0.0;
		double v0_fps = 0.0;
		double v1_fps = 0.0;
		double now = mono_sec();
		int result;

		read_cif(&ci);
		cur.t = now;
		cur.cif = ci.frames;
		cur.v0 = video_frames_out(0);
		cur.v1 = video_frames_out(1);

		if (atomic_load(&g_fake_stall)) {
			if (!fake_logged) {
				fake_logged = 1;
				LOGW("watch: 시험용 정지 모의 시작(SIGUSR2) - 실제 프레임은 계속 나오고 있음");
			}
			/* 카운터 변화를 무시해 정지처럼 보이게 한다. */
			cur.cif = last_cif;
			cur.v0 = last_v0;
		} else {
			fake_logged = 0;
		}
		if (first) {
			last_cif = cur.cif;
			last_v0 = cur.v0;
			first = 0;
		}
		if (cur.cif != last_cif) {
			cif_changed = now;
			last_cif = cur.cif;
		}
		if (cur.v0 != last_v0) {
			v0_changed = now;
			last_v0 = cur.v0;
		}

		/* 최근 FPS_WINDOW 초 평균. CIF 카운터는 재초기화 때 0 으로 돌아가므로 음수는 버린다. */
		if (ring_len == FPS_WINDOW + 1) {
			memmove(&ring[0], &ring[1], sizeof(ring[0]) * FPS_WINDOW);
			ring_len--;
		}
		ring[ring_len++] = cur;
		if (ring_len >= 2) {
			const struct sample *old = &ring[0];
			double dt = cur.t - old->t;

			if (dt > 0.5) {
				if (cur.cif >= old->cif && old->cif >= 0)
					cif_fps = (double)(cur.cif - old->cif) / dt;
				v0_fps = (double)(cur.v0 - old->v0) / dt;
				v1_fps = (double)(cur.v1 - old->v1) / dt;
			}
		}

		/* main 의 재초기화 진행 상황 반영 */
		result = atomic_exchange(&g_reinit_result, 0);
		if (result != 0) {
			atomic_store(&g_fake_stall, 0);
			first = 1; /* 카운터 기준을 다시 잡는다 */
			phase_start = now;
			cif_changed = now;
			v0_changed = now;
			ring_len = 0;
			stall_reported = 0;
			hung_reported = 0;
			if (result == 1) {
				st = ST_STARTING;
				after_reinit = 1;
				LOGI("watch: 재초기화 끝, %d초 뒤부터 다시 판정", g_cfg.stall_grace_s);
			} else {
				st = ST_FAILED;
				LOGE("watch: 재초기화 중 파이프라인을 다시 띄우지 못함 -> failed");
			}
		} else if (atomic_load(&g_reinit_active)) {
			if (st != ST_REINIT && st != ST_FAILED) {
				st = ST_REINIT;
				reinit_started = now;
			}
			if (!hung_reported && now - reinit_started > (double)g_cfg.reinit_timeout_s) {
				hung_reported = 1;
				st = ST_FAILED;
				LOGE("watch: 재초기화가 %d초 안에 끝나지 않음(해제 또는 초기화가 멈춤) -> failed",
				     g_cfg.reinit_timeout_s);
			}
		} else if (st == ST_REINIT) {
			/* main 이 방금 재초기화를 끝냈고 결과는 다음 주기에 읽는다. */
		} else {
			int in_grace = now - phase_start < (double)g_cfg.stall_grace_s;
			int cif_stuck = now - cif_changed >= (double)g_cfg.stall_timeout_s;
			int v0_stuck = now - v0_changed >= (double)g_cfg.stall_timeout_s;

			if (in_grace) {
				if (st != ST_FAILED)
					st = ST_STARTING;
			} else if (cif_stuck || v0_stuck) {
				if (!stall_reported) {
					stall_reported = 1;
					stalls++;
					LOGE("watch: 정지 감지 #%d (%s%s%d초 이상 불변) cif=%ld venc0=%llu venc1=%llu "
					     "cif_buf_in_drv=%d cif_buf_total=%d",
					     stalls, cif_stuck ? "CIF " : "", v0_stuck ? "VENC0 " : "",
					     g_cfg.stall_timeout_s, cur.cif, cur.v0, cur.v1, ci.buf_in_drv,
					     ci.buf_total);
					if (after_reinit) {
						after_reinit = 0;
						LOGE("watch: 재초기화 뒤에도 프레임이 나오지 않음(복구 실패)");
					}
					if (strcmp(g_cfg.stall_action, "reinit") == 0 && reinits < g_cfg.reinit_max) {
						reinits++;
						LOGW("watch: 재초기화 요청 %d/%d", reinits, g_cfg.reinit_max);
						atomic_store(&g_reinit_req, 1);
						st = ST_STALLED;
					} else if (strcmp(g_cfg.stall_action, "reinit") == 0) {
						LOGE("watch: 재초기화 %d회로도 복구되지 않음 -> failed (재부팅 필요)",
						     g_cfg.reinit_max);
						st = ST_FAILED;
					} else {
						st = ST_STALLED;
					}
				}
			} else {
				int degraded = cif_fps > 0.0 && ring_len > FPS_WINDOW / 2 &&
				               cif_fps < (double)g_cfg.fps * DEGRADED_RATIO;
				enum state next = degraded ? ST_DEGRADED : ST_RUNNING;

				if (stall_reported) {
					stall_reported = 0;
					LOGI("watch: 프레임이 다시 나옴 (cif=%ld venc0=%llu)", cur.cif, cur.v0);
				}
				if (after_reinit) {
					after_reinit = 0;
					reinit_ok++;
					LOGI("watch: 재초기화로 복구됨 (누적 %d회)", reinit_ok);
				}
				if (next != st) {
					if (next == ST_DEGRADED)
						LOGW("watch: 입력 fps 저하 (최근 %d초 평균 %.1f, 목표 %d)", FPS_WINDOW,
						     cif_fps, g_cfg.fps);
					else if (st == ST_DEGRADED)
						LOGI("watch: 입력 fps 회복 (%.1f)", cif_fps);
					st = next;
				}
			}
		}

		if (atomic_exchange(&g_manual_req, 0) && !atomic_load(&g_reinit_active) &&
		    st != ST_REINIT) {
			LOGW("watch: 수동 재초기화 요청(SIGUSR1)");
			atomic_store(&g_reinit_req, 1);
		}

		write_status(st, now, &cur, cif_fps, v0_fps, v1_fps, &ci, stalls, reinits, reinit_ok);

		/* 종료 요청 뒤에도 main 이 해제에서 돌아오지 못하면 여기서 끝낸다. */
		if (g_quit != NULL && *g_quit) {
			if (quit_seen == 0.0) {
				quit_seen = now;
			} else if (now - quit_seen > QUIT_DEADLINE_S) {
				LOGE("watch: 종료 요청 뒤 %.0f초가 지나도 해제가 끝나지 않아 강제 종료",
				     QUIT_DEADLINE_S);
				log_close();
				_exit(3);
			}
		}
		usleep(1000 * 1000);
	}
	return NULL;
}

int watch_start(const struct config *cfg, const volatile sig_atomic_t *quit_flag)
{
	g_cfg = *cfg;
	g_quit = quit_flag;
	atomic_store(&g_run, 1);
	if (pthread_create(&g_thread, NULL, watch_thread, NULL) != 0) {
		atomic_store(&g_run, 0);
		LOGE("watch: 스레드 생성 실패");
		return -1;
	}
	LOGI("watch: 정지 판정 %d초, 시작 유예 %d초, 동작 %s(최대 %d회), 상태 파일 %s",
	     cfg->stall_timeout_s, cfg->stall_grace_s, cfg->stall_action, cfg->reinit_max,
	     cfg->status_file[0] ? cfg->status_file : "없음");
	return 0;
}

void watch_stop(void)
{
	if (atomic_exchange(&g_run, 0))
		pthread_join(g_thread, NULL);
}

int watch_take_reinit_request(void)
{
	return atomic_exchange(&g_reinit_req, 0);
}

void watch_request_reinit(void)
{
	atomic_store(&g_manual_req, 1);
}

void watch_simulate_stall(void)
{
	atomic_store(&g_fake_stall, 1);
}

void watch_reinit_begin(void)
{
	atomic_store(&g_reinit_active, 1);
}

void watch_reinit_end(int ok)
{
	atomic_store(&g_reinit_result, ok ? 1 : 2);
	atomic_store(&g_reinit_active, 0);
}
