#include "audio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#include <rk_comm_aenc.h>
#include <rk_comm_aio.h>
#include <rk_mpi_aenc.h>
#include <rk_mpi_ai.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>

#include "log.h"
#include "rtsp.h"

#define AI_DEV 0
#define AI_CHN 0
#define AENC_CHN 0
#define AI_CARD "hw:0,0"
#define AI_FRAME_SAMPLES 1024 /* SDK 예제(simple_ai_bind_aenc)와 같은 값 */
#define STREAM_TIMEOUT_MS 1000

static pthread_t g_thread;
static atomic_int g_run;
static int g_ai_enabled;
static int g_ai_chn_enabled;
static int g_aenc_created;
static int g_bound;
static int g_thread_started;

static void endpoints(MPP_CHN_S *src, MPP_CHN_S *dst)
{
	src->enModId = RK_ID_AI;
	src->s32DevId = AI_DEV;
	src->s32ChnId = AI_CHN;
	dst->enModId = RK_ID_AENC;
	dst->s32DevId = 0;
	dst->s32ChnId = AENC_CHN;
}

static void *audio_thread(void *arg)
{
	AUDIO_STREAM_S stream;

	(void)arg;
	prctl(PR_SET_NAME, "st_audio", 0, 0, 0);
	while (atomic_load(&g_run)) {
		RK_S32 ret;

		memset(&stream, 0, sizeof(stream));
		ret = RK_MPI_AENC_GetStream(AENC_CHN, &stream, STREAM_TIMEOUT_MS);
		if (ret != RK_SUCCESS)
			continue;
		if (stream.u32Len > 0)
			rtsp_srv_send_audio(RK_MPI_MB_Handle2VirAddr(stream.pMbBlk), (int)stream.u32Len,
			                    stream.u64TimeStamp);
		RK_MPI_AENC_ReleaseStream(AENC_CHN, &stream);
	}
	return NULL;
}

static int ai_init(void)
{
	AIO_ATTR_S attr;
	AI_CHN_PARAM_S param;
	RK_S32 ret;

	memset(&attr, 0, sizeof(attr));
	snprintf((char *)attr.u8CardName, sizeof(attr.u8CardName), "%s", AI_CARD);
	attr.soundCard.channels = 2; /* 내장 코덱은 2채널로 열고 왼쪽만 쓴다 */
	attr.soundCard.sampleRate = AUDIO_SAMPLE_RATE;
	attr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
	attr.enBitwidth = AUDIO_BIT_WIDTH_16;
	attr.enSamplerate = (AUDIO_SAMPLE_RATE_E)AUDIO_SAMPLE_RATE;
	attr.enSoundmode = AUDIO_SOUND_MODE_MONO;
	attr.u32PtNumPerFrm = AI_FRAME_SAMPLES;
	attr.u32FrmNum = 4;
	attr.u32EXFlag = 0;
	attr.u32ChnCnt = 2;

	ret = RK_MPI_AI_SetPubAttr(AI_DEV, &attr);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AI_SetPubAttr 실패 %#x", ret);
		return -1;
	}
	ret = RK_MPI_AI_Enable(AI_DEV);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AI_Enable 실패 %#x", ret);
		return -1;
	}
	g_ai_enabled = 1;

	memset(&param, 0, sizeof(param));
	param.enLoopbackMode = AUDIO_LOOPBACK_NONE;
	param.s32UsrFrmDepth = 1;
	ret = RK_MPI_AI_SetChnParam(AI_DEV, AI_CHN, &param);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AI_SetChnParam 실패 %#x", ret);
		return -1;
	}
	RK_MPI_AI_SetTrackMode(AI_DEV, AUDIO_TRACK_FRONT_LEFT);
	ret = RK_MPI_AI_EnableChn(AI_DEV, AI_CHN);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AI_EnableChn 실패 %#x", ret);
		return -1;
	}
	g_ai_chn_enabled = 1;
	return 0;
}

static int aenc_init(void)
{
	AENC_CHN_ATTR_S attr;
	RK_S32 ret;

	memset(&attr, 0, sizeof(attr));
	attr.enType = RK_AUDIO_ID_PCM_ALAW; /* G.711A */
	attr.stCodecAttr.enType = RK_AUDIO_ID_PCM_ALAW;
	attr.stCodecAttr.u32Channels = AUDIO_CHANNELS;
	attr.stCodecAttr.u32SampleRate = AUDIO_SAMPLE_RATE;
	attr.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
	attr.u32BufCount = 4;
	attr.u32Depth = 4;

	ret = RK_MPI_AENC_CreateChn(AENC_CHN, &attr);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AENC_CreateChn 실패 %#x", ret);
		return -1;
	}
	g_aenc_created = 1;
	return 0;
}

int audio_start(const struct config *cfg)
{
	MPP_CHN_S src;
	MPP_CHN_S dst;
	RK_S32 ret;

	if (!cfg->audio_enable)
		return -1;

	if (ai_init() != 0 || aenc_init() != 0) {
		audio_stop();
		return -1;
	}
	endpoints(&src, &dst);
	ret = RK_MPI_SYS_Bind(&src, &dst);
	if (ret != RK_SUCCESS) {
		LOGE("audio: AI->AENC bind 실패 %#x", ret);
		audio_stop();
		return -1;
	}
	g_bound = 1;

	atomic_store(&g_run, 1);
	if (pthread_create(&g_thread, NULL, audio_thread, NULL) != 0) {
		LOGE("audio: 스레드 생성 실패");
		atomic_store(&g_run, 0);
		audio_stop();
		return -1;
	}
	g_thread_started = 1;
	LOGI("audio: %s -> G.711A %dHz %dch", AI_CARD, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
	return 0;
}

void audio_stop(void)
{
	MPP_CHN_S src;
	MPP_CHN_S dst;

	atomic_store(&g_run, 0);
	if (g_thread_started) {
		pthread_join(g_thread, NULL);
		g_thread_started = 0;
	}
	if (g_bound) {
		endpoints(&src, &dst);
		RK_MPI_SYS_UnBind(&src, &dst);
		g_bound = 0;
	}
	if (g_ai_chn_enabled) {
		RK_MPI_AI_DisableChn(AI_DEV, AI_CHN);
		g_ai_chn_enabled = 0;
	}
	if (g_ai_enabled) {
		RK_MPI_AI_Disable(AI_DEV);
		g_ai_enabled = 0;
	}
	if (g_aenc_created) {
		RK_MPI_AENC_DestroyChn(AENC_CHN);
		g_aenc_created = 0;
	}
}
