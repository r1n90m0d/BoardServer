#include "rtsp.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <rtsp_demo.h>

#include "audio.h"
#include "log.h"

#define EVENT_INTERVAL_US 10000

static rtsp_demo_handle g_demo;
static rtsp_session_handle g_session[RTSP_MAX_SESSIONS];
static int g_has_audio[RTSP_MAX_SESSIONS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static atomic_int g_run;

static void *event_thread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "st_rtsp", 0, 0, 0);
	while (atomic_load(&g_run)) {
		pthread_mutex_lock(&g_lock);
		if (g_demo != NULL)
			rtsp_do_event(g_demo);
		pthread_mutex_unlock(&g_lock);
		usleep(EVENT_INTERVAL_US);
	}
	return NULL;
}

int rtsp_srv_start(int port)
{
	g_demo = create_rtsp_demo(port);
	if (g_demo == NULL) {
		LOGE("rtsp: 포트 %d 에서 서버를 만들지 못함", port);
		return -1;
	}
	atomic_store(&g_run, 1);
	if (pthread_create(&g_thread, NULL, event_thread, NULL) != 0) {
		LOGE("rtsp: 이벤트 스레드 생성 실패");
		atomic_store(&g_run, 0);
		rtsp_del_demo(g_demo);
		g_demo = NULL;
		return -1;
	}
	LOGI("rtsp: 포트 %d 에서 대기", port);
	return 0;
}

int rtsp_srv_add_session(int idx, const char *path, int with_audio)
{
	rtsp_session_handle s;

	if (idx < 0 || idx >= RTSP_MAX_SESSIONS || g_demo == NULL)
		return -1;

	pthread_mutex_lock(&g_lock);
	s = rtsp_new_session(g_demo, path);
	if (s != NULL) {
		/* 코덱 정보(VPS/SPS/PPS)는 첫 키프레임에서 라이브러리가 뽑는다. */
		rtsp_set_video(s, RTSP_CODEC_ID_VIDEO_H265, NULL, 0);
		rtsp_sync_video_ts(s, rtsp_get_reltime(), rtsp_get_ntptime());
		/* 오디오 트랙은 요청했을 때만 선언한다. 선언하지 않으면 SDP 에 m=audio 가 없다. */
		if (with_audio) {
			rtsp_set_audio(s, RTSP_CODEC_ID_AUDIO_G711A, NULL, 0);
			rtsp_set_audio_sample_rate(s, AUDIO_SAMPLE_RATE);
			rtsp_set_audio_channels(s, AUDIO_CHANNELS);
			rtsp_sync_audio_ts(s, rtsp_get_reltime(), rtsp_get_ntptime());
		}
		g_has_audio[idx] = with_audio ? 1 : 0;
		g_session[idx] = s;
	}
	pthread_mutex_unlock(&g_lock);

	if (s == NULL) {
		LOGE("rtsp: 세션 %s 생성 실패", path);
		return -1;
	}
	LOGI("rtsp: 세션 %s (영상 H.265, 오디오 %s)", path, with_audio ? "G.711A" : "없음");
	return 0;
}

void rtsp_srv_send_video(int idx, const uint8_t *data, int len, uint64_t pts_us)
{
	if (idx < 0 || idx >= RTSP_MAX_SESSIONS)
		return;
	pthread_mutex_lock(&g_lock);
	if (g_demo != NULL && g_session[idx] != NULL) {
		rtsp_tx_video(g_session[idx], data, len, pts_us);
		rtsp_do_event(g_demo);
	}
	pthread_mutex_unlock(&g_lock);
}

void rtsp_srv_send_audio(const uint8_t *data, int len, uint64_t pts_us)
{
	int i;

	pthread_mutex_lock(&g_lock);
	if (g_demo != NULL) {
		for (i = 0; i < RTSP_MAX_SESSIONS; i++) {
			if (g_session[i] != NULL && g_has_audio[i])
				rtsp_tx_audio(g_session[i], data, len, pts_us);
		}
		rtsp_do_event(g_demo);
	}
	pthread_mutex_unlock(&g_lock);
}

void rtsp_srv_stop(void)
{
	int i;

	if (atomic_exchange(&g_run, 0))
		pthread_join(g_thread, NULL);

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < RTSP_MAX_SESSIONS; i++) {
		if (g_session[i] != NULL) {
			rtsp_del_session(g_session[i]);
			g_session[i] = NULL;
		}
	}
	if (g_demo != NULL) {
		rtsp_del_demo(g_demo);
		g_demo = NULL;
	}
	pthread_mutex_unlock(&g_lock);
}
