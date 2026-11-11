#include "video.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#include <rk_comm_rc.h>
#include <rk_comm_venc.h>
#include <rk_comm_vi.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>

#include "log.h"
#include "rtsp.h"

#define VI_DEV 0
#define VI_PIPE 0
#define STREAM_TIMEOUT_MS 1000
#define VENC_STREAM_BUF_CNT 4

struct vchan {
	int id;
	int active;
	int width;
	int height;
	int kbps;
	int gop;
	int fps;
	int vi_buf_cnt;
	int vi_depth;
	int vi_enabled;
	int venc_created;
	int bound;
	int thread_started;
	pthread_t thread;
};

static struct vchan g_chn[VIDEO_MAX_CHN];
static atomic_ullong g_frames[VIDEO_MAX_CHN];
static atomic_int g_run;
static int g_sys_inited;
static int g_dev_enabled;

static void *stream_thread(void *arg)
{
	struct vchan *c = arg;
	VENC_STREAM_S stream;
	char name[16];

	snprintf(name, sizeof(name), "st_venc%d", c->id);
	prctl(PR_SET_NAME, name, 0, 0, 0);

	memset(&stream, 0, sizeof(stream));
	stream.pstPack = malloc(sizeof(VENC_PACK_S));
	if (stream.pstPack == NULL) {
		LOGE("video%d: 메모리 부족", c->id);
		return NULL;
	}

	while (atomic_load(&g_run)) {
		RK_S32 ret = RK_MPI_VENC_GetStream(c->id, &stream, STREAM_TIMEOUT_MS);

		if (ret != RK_SUCCESS)
			continue; /* 시간 초과 포함. 정지 판정은 감시 쪽에서 프레임 수로 한다. */

		rtsp_srv_send_video(c->id, RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk),
		                    (int)stream.pstPack->u32Len, stream.pstPack->u64PTS);
		atomic_fetch_add(&g_frames[c->id], 1);

		ret = RK_MPI_VENC_ReleaseStream(c->id, &stream);
		if (ret != RK_SUCCESS)
			LOGW("video%d: ReleaseStream 실패 %#x", c->id, ret);
	}
	free(stream.pstPack);
	return NULL;
}

static int vi_dev_init(void)
{
	VI_DEV_ATTR_S dev_attr;
	VI_DEV_BIND_PIPE_S bind_pipe;
	RK_S32 ret;

	memset(&dev_attr, 0, sizeof(dev_attr));
	memset(&bind_pipe, 0, sizeof(bind_pipe));

	ret = RK_MPI_VI_GetDevAttr(VI_DEV, &dev_attr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		ret = RK_MPI_VI_SetDevAttr(VI_DEV, &dev_attr);
		if (ret != RK_SUCCESS) {
			LOGE("video: VI_SetDevAttr 실패 %#x", ret);
			return -1;
		}
	}
	if (RK_MPI_VI_GetDevIsEnable(VI_DEV) != RK_SUCCESS) {
		ret = RK_MPI_VI_EnableDev(VI_DEV);
		if (ret != RK_SUCCESS) {
			LOGE("video: VI_EnableDev 실패 %#x", ret);
			return -1;
		}
		bind_pipe.u32Num = 1;
		bind_pipe.PipeId[0] = VI_PIPE;
		ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind_pipe);
		if (ret != RK_SUCCESS) {
			LOGE("video: VI_SetDevBindPipe 실패 %#x", ret);
			RK_MPI_VI_DisableDev(VI_DEV);
			return -1;
		}
	}
	g_dev_enabled = 1;
	return 0;
}

static int vi_chn_init(struct vchan *c)
{
	VI_CHN_ATTR_S attr;
	RK_S32 ret;

	memset(&attr, 0, sizeof(attr));
	attr.stIspOpt.u32BufCount = (RK_U32)c->vi_buf_cnt;
	attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	attr.stIspOpt.stMaxSize.u32Width = (RK_U32)c->width;
	attr.stIspOpt.stMaxSize.u32Height = (RK_U32)c->height;
	attr.stSize.u32Width = (RK_U32)c->width;
	attr.stSize.u32Height = (RK_U32)c->height;
	attr.enPixelFormat = RK_FMT_YUV420SP;
	attr.enCompressMode = COMPRESS_MODE_NONE;
	attr.u32Depth = (RK_U32)c->vi_depth;

	ret = RK_MPI_VI_SetChnAttr(VI_PIPE, c->id, &attr);
	if (ret != RK_SUCCESS) {
		LOGE("video%d: VI_SetChnAttr 실패 %#x", c->id, ret);
		return -1;
	}
	ret = RK_MPI_VI_EnableChn(VI_PIPE, c->id);
	if (ret != RK_SUCCESS) {
		LOGE("video%d: VI_EnableChn 실패 %#x", c->id, ret);
		return -1;
	}
	c->vi_enabled = 1;
	return 0;
}

static int venc_init(struct vchan *c)
{
	VENC_CHN_ATTR_S attr;
	VENC_CHN_REF_BUF_SHARE_S ref_share;
	VENC_RECV_PIC_PARAM_S recv;
	RK_S32 ret;

	memset(&attr, 0, sizeof(attr));
	attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
	attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	attr.stVencAttr.u32MaxPicWidth = (RK_U32)c->width;
	attr.stVencAttr.u32MaxPicHeight = (RK_U32)c->height;
	attr.stVencAttr.u32PicWidth = (RK_U32)c->width;
	attr.stVencAttr.u32PicHeight = (RK_U32)c->height;
	attr.stVencAttr.u32VirWidth = (RK_U32)c->width;
	attr.stVencAttr.u32VirHeight = (RK_U32)c->height;
	attr.stVencAttr.u32StreamBufCnt = VENC_STREAM_BUF_CNT;
	attr.stVencAttr.u32BufSize = (RK_U32)(c->width * c->height * 3 / 2);
	attr.stVencAttr.enMirror = MIRROR_NONE;

	attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
	attr.stRcAttr.stH265Cbr.u32Gop = (RK_U32)c->gop;
	attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)c->kbps;
	attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)c->fps;
	attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
	attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)c->fps;
	attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
	attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

	ret = RK_MPI_VENC_CreateChn(c->id, &attr);
	if (ret != RK_SUCCESS) {
		LOGE("video%d: VENC_CreateChn 실패 %#x", c->id, ret);
		return -1;
	}
	c->venc_created = 1;

	/* 참조 프레임 버퍼 공유: RV1106 에서 인코더 메모리를 줄인다(rkipc 기본 설정과 같음). */
	memset(&ref_share, 0, sizeof(ref_share));
	ref_share.bEnable = RK_TRUE;
	ret = RK_MPI_VENC_SetChnRefBufShareAttr(c->id, &ref_share);
	if (ret != RK_SUCCESS)
		LOGW("video%d: RefBufShare 설정 실패 %#x", c->id, ret);

	memset(&recv, 0, sizeof(recv));
	recv.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(c->id, &recv);
	if (ret != RK_SUCCESS) {
		LOGE("video%d: VENC_StartRecvFrame 실패 %#x", c->id, ret);
		return -1;
	}
	return 0;
}

static void chn_endpoints(const struct vchan *c, MPP_CHN_S *src, MPP_CHN_S *dst)
{
	src->enModId = RK_ID_VI;
	src->s32DevId = VI_DEV;
	src->s32ChnId = c->id;
	dst->enModId = RK_ID_VENC;
	dst->s32DevId = 0;
	dst->s32ChnId = c->id;
}

static int chn_start(struct vchan *c)
{
	MPP_CHN_S src;
	MPP_CHN_S dst;
	RK_S32 ret;

	if (vi_chn_init(c) != 0 || venc_init(c) != 0)
		return -1;

	if (pthread_create(&c->thread, NULL, stream_thread, c) != 0) {
		LOGE("video%d: 스트림 스레드 생성 실패", c->id);
		return -1;
	}
	c->thread_started = 1;

	chn_endpoints(c, &src, &dst);
	ret = RK_MPI_SYS_Bind(&src, &dst);
	if (ret != RK_SUCCESS) {
		LOGE("video%d: VI->VENC bind 실패 %#x", c->id, ret);
		return -1;
	}
	c->bound = 1;
	LOGI("video%d: %dx%d H.265 CBR %dkbps GOP %d fps %d, VI 버퍼 %d Depth %d", c->id, c->width,
	     c->height, c->kbps, c->gop, c->fps, c->vi_buf_cnt, c->vi_depth);
	return 0;
}

/* 스레드는 video_stop 에서 먼저 모두 끝낸 뒤 이 함수를 부른다. */
static void chn_stop(struct vchan *c)
{
	MPP_CHN_S src;
	MPP_CHN_S dst;
	RK_S32 ret;

	if (c->bound) {
		chn_endpoints(c, &src, &dst);
		ret = RK_MPI_SYS_UnBind(&src, &dst);
		if (ret != RK_SUCCESS)
			LOGW("video%d: unbind 실패 %#x", c->id, ret);
		c->bound = 0;
	}
	if (c->venc_created) {
		ret = RK_MPI_VENC_StopRecvFrame(c->id);
		if (ret != RK_SUCCESS)
			LOGW("video%d: VENC_StopRecvFrame 실패 %#x", c->id, ret);
		ret = RK_MPI_VENC_DestroyChn(c->id);
		if (ret != RK_SUCCESS)
			LOGW("video%d: VENC_DestroyChn 실패 %#x", c->id, ret);
		c->venc_created = 0;
	}
	if (c->vi_enabled) {
		ret = RK_MPI_VI_DisableChn(VI_PIPE, c->id);
		if (ret != RK_SUCCESS)
			LOGW("video%d: VI_DisableChn 실패 %#x", c->id, ret);
		c->vi_enabled = 0;
	}
}

static void chn_setup(struct vchan *c, int id, int width, int height, int kbps, int gop, int fps,
                      int vi_buf_cnt, int vi_depth)
{
	memset(c, 0, sizeof(*c));
	c->id = id;
	c->active = 1;
	c->width = width;
	c->height = height;
	c->kbps = kbps;
	c->gop = gop;
	c->fps = fps;
	c->vi_buf_cnt = vi_buf_cnt;
	c->vi_depth = vi_depth;
}

int video_start(const struct config *cfg)
{
	RK_S32 ret;
	int i;

	memset(g_chn, 0, sizeof(g_chn));
	chn_setup(&g_chn[0], 0, cfg->main_width, cfg->main_height, cfg->main_kbps, cfg->main_gop,
	          cfg->fps, cfg->vi_buf_cnt, cfg->vi_depth);

	ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS) {
		LOGE("video: RK_MPI_SYS_Init 실패 %#x", ret);
		return -1;
	}
	g_sys_inited = 1;

	if (vi_dev_init() != 0) {
		video_stop();
		return -1;
	}

	atomic_store(&g_run, 1);
	for (i = 0; i < VIDEO_MAX_CHN; i++) {
		if (!g_chn[i].active)
			continue;
		if (chn_start(&g_chn[i]) != 0) {
			video_stop();
			return -1;
		}
	}
	return 0;
}

void video_stop(void)
{
	int i;

	atomic_store(&g_run, 0);
	for (i = 0; i < VIDEO_MAX_CHN; i++) {
		if (g_chn[i].thread_started) {
			pthread_join(g_chn[i].thread, NULL);
			g_chn[i].thread_started = 0;
		}
	}
	for (i = 0; i < VIDEO_MAX_CHN; i++) {
		if (g_chn[i].active)
			chn_stop(&g_chn[i]);
	}
	if (g_dev_enabled) {
		RK_MPI_VI_DisableDev(VI_DEV);
		g_dev_enabled = 0;
	}
	if (g_sys_inited) {
		RK_MPI_SYS_Exit();
		g_sys_inited = 0;
	}
	LOGI("video: 해제");
}

uint64_t video_frames_out(int chn)
{
	if (chn < 0 || chn >= VIDEO_MAX_CHN)
		return 0;
	return (uint64_t)atomic_load(&g_frames[chn]);
}
