#include "osd.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <rk_comm_rgn.h>
#include <rk_mpi_rgn.h>

#include "log.h"
#include "video.h"

#define GLYPH_W 5
#define GLYPH_H 7
#define CELL_W (GLYPH_W + 1)
#define TEXT_LEN 19 /* "YYYY-MM-DD HH:MM:SS" */
#define ALIGN16(x) (((x) + 15) & ~15)

#define COLOR_TEXT 0xFFFFF799u    /* ARGB: 불투명 연노랑 (rkipc 기본 OSD 색과 같음) */
#define COLOR_OUTLINE 0xFF000000u /* 불투명 검정 */
#define COLOR_NONE 0x00000000u    /* 투명 */

/* 5x7 글꼴: 한 행이 5비트(왼쪽이 상위 비트). 시각 표시에 필요한 글자만 둔다. */
static const uint8_t g_font_digits[10][GLYPH_H] = {
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
	{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
	{ 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
	{ 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
	{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
	{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
};
static const uint8_t g_font_dash[GLYPH_H] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
static const uint8_t g_font_colon[GLYPH_H] = { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 };
static const uint8_t g_font_blank[GLYPH_H] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

struct osd_chn {
	int active;
	int attached;
	int created;
	int chn;
	int scale;  /* 글꼴 한 점의 화면 픽셀 수 */
	int width;  /* 캔버스 크기 (16 의 배수) */
	int height;
	int pos_x;
	int pos_y;
	uint8_t *mask;    /* width*height, 글자 픽셀이면 1 */
	uint32_t *pixels; /* width*height ARGB8888 */
};

static struct osd_chn g_osd[VIDEO_MAX_CHN];
static pthread_t g_thread;
static atomic_int g_run;

static const uint8_t *glyph_for(char ch)
{
	if (ch >= '0' && ch <= '9')
		return g_font_digits[ch - '0'];
	if (ch == '-')
		return g_font_dash;
	if (ch == ':')
		return g_font_colon;
	return g_font_blank;
}

static void render(struct osd_chn *o, const char *text)
{
	const int margin = o->scale; /* 테두리가 들어갈 여백 */
	int i;
	int x;
	int y;

	memset(o->mask, 0, (size_t)(o->width * o->height));
	for (i = 0; i < TEXT_LEN && text[i] != '\0'; i++) {
		const uint8_t *g = glyph_for(text[i]);
		int gx;
		int gy;

		for (gy = 0; gy < GLYPH_H; gy++) {
			for (gx = 0; gx < GLYPH_W; gx++) {
				int px0;
				int py0;

				if (!(g[gy] & (1u << (GLYPH_W - 1 - gx))))
					continue;
				px0 = margin + (i * CELL_W + gx) * o->scale;
				py0 = margin + gy * o->scale;
				for (y = py0; y < py0 + o->scale; y++)
					memset(o->mask + y * o->width + px0, 1, (size_t)o->scale);
			}
		}
	}

	/* 글자는 색으로, 글자에 맞닿은 바깥 1픽셀은 검정 테두리로, 나머지는 투명으로. */
	for (y = 0; y < o->height; y++) {
		for (x = 0; x < o->width; x++) {
			uint32_t color = COLOR_NONE;

			if (o->mask[y * o->width + x]) {
				color = COLOR_TEXT;
			} else {
				int dx;
				int dy;

				for (dy = -1; dy <= 1 && color == COLOR_NONE; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						int nx = x + dx;
						int ny = y + dy;

						if (nx < 0 || ny < 0 || nx >= o->width || ny >= o->height)
							continue;
						if (o->mask[ny * o->width + nx]) {
							color = COLOR_OUTLINE;
							break;
						}
					}
				}
			}
			o->pixels[y * o->width + x] = color;
		}
	}
}

static int push_bitmap(struct osd_chn *o)
{
	BITMAP_S bmp;
	RK_S32 ret;

	memset(&bmp, 0, sizeof(bmp));
	bmp.enPixelFormat = RK_FMT_ARGB8888;
	bmp.u32Width = (RK_U32)o->width;
	bmp.u32Height = (RK_U32)o->height;
	bmp.pData = o->pixels;
	ret = RK_MPI_RGN_SetBitMap(o->chn, &bmp);
	if (ret != RK_SUCCESS) {
		LOGW("osd%d: SetBitMap 실패 %#x", o->chn, ret);
		return -1;
	}
	return 0;
}

static void now_text(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

static void *osd_thread(void *arg)
{
	char last[32] = "";
	char text[32];
	int i;

	(void)arg;
	prctl(PR_SET_NAME, "st_osd", 0, 0, 0);
	while (atomic_load(&g_run)) {
		now_text(text, sizeof(text));
		if (strcmp(text, last) != 0) {
			for (i = 0; i < VIDEO_MAX_CHN; i++) {
				if (!g_osd[i].attached)
					continue;
				render(&g_osd[i], text);
				push_bitmap(&g_osd[i]);
			}
			snprintf(last, sizeof(last), "%s", text);
		}
		usleep(100 * 1000);
	}
	return NULL;
}

static int chn_create(struct osd_chn *o, int chn, int frame_h)
{
	RGN_ATTR_S attr;
	RGN_CHN_ATTR_S chn_attr;
	MPP_CHN_S target;
	RK_S32 ret;
	char text[32];

	memset(o, 0, sizeof(*o));
	o->active = 1;
	o->chn = chn;
	o->scale = frame_h / 270;
	if (o->scale < 1)
		o->scale = 1;
	o->width = ALIGN16(TEXT_LEN * CELL_W * o->scale + 2 * o->scale);
	o->height = ALIGN16(GLYPH_H * o->scale + 2 * o->scale);
	o->pos_x = ALIGN16(8 * o->scale);
	o->pos_y = ALIGN16(8 * o->scale);
	o->mask = malloc((size_t)(o->width * o->height));
	o->pixels = malloc((size_t)(o->width * o->height) * sizeof(uint32_t));
	if (o->mask == NULL || o->pixels == NULL) {
		LOGE("osd%d: 메모리 부족", chn);
		return -1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.enType = OVERLAY_RGN;
	attr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
	attr.unAttr.stOverlay.u32CanvasNum = 2;
	attr.unAttr.stOverlay.stSize.u32Width = (RK_U32)o->width;
	attr.unAttr.stOverlay.stSize.u32Height = (RK_U32)o->height;
	ret = RK_MPI_RGN_Create(o->chn, &attr);
	if (ret != RK_SUCCESS) {
		LOGE("osd%d: RGN_Create 실패 %#x", chn, ret);
		return -1;
	}
	o->created = 1;

	memset(&target, 0, sizeof(target));
	target.enModId = RK_ID_VENC;
	target.s32DevId = 0;
	target.s32ChnId = chn;
	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.bShow = RK_TRUE;
	chn_attr.enType = OVERLAY_RGN;
	chn_attr.unChnAttr.stOverlayChn.stPoint.s32X = o->pos_x;
	chn_attr.unChnAttr.stOverlayChn.stPoint.s32Y = o->pos_y;
	chn_attr.unChnAttr.stOverlayChn.u32BgAlpha = 128;
	chn_attr.unChnAttr.stOverlayChn.u32FgAlpha = 128;
	chn_attr.unChnAttr.stOverlayChn.u32Layer = 0;
	ret = RK_MPI_RGN_AttachToChn(o->chn, &target, &chn_attr);
	if (ret != RK_SUCCESS) {
		LOGE("osd%d: RGN_AttachToChn 실패 %#x", chn, ret);
		return -1;
	}
	o->attached = 1;

	now_text(text, sizeof(text));
	render(o, text);
	push_bitmap(o);
	LOGI("osd%d: %dx%d 를 (%d,%d) 에 표시, 배율 %d", chn, o->width, o->height, o->pos_x, o->pos_y,
	     o->scale);
	return 0;
}

static void chn_destroy(struct osd_chn *o)
{
	MPP_CHN_S target;

	if (o->attached) {
		memset(&target, 0, sizeof(target));
		target.enModId = RK_ID_VENC;
		target.s32DevId = 0;
		target.s32ChnId = o->chn;
		RK_MPI_RGN_DetachFromChn(o->chn, &target);
		o->attached = 0;
	}
	if (o->created) {
		RK_MPI_RGN_Destroy(o->chn);
		o->created = 0;
	}
	free(o->mask);
	free(o->pixels);
	o->mask = NULL;
	o->pixels = NULL;
	o->active = 0;
}

int osd_start(const struct config *cfg)
{
	int i;
	int any = 0;

	if (!cfg->osd_enable)
		return 0;

	for (i = 0; i < VIDEO_MAX_CHN; i++) {
		int w;
		int h;

		if (!video_chn_size(i, &w, &h))
			continue;
		if (chn_create(&g_osd[i], i, h) != 0) {
			/* OSD 는 부가 기능이라 실패해도 송출은 계속한다. */
			chn_destroy(&g_osd[i]);
			continue;
		}
		any = 1;
	}
	if (!any)
		return -1;

	atomic_store(&g_run, 1);
	if (pthread_create(&g_thread, NULL, osd_thread, NULL) != 0) {
		LOGE("osd: 스레드 생성 실패");
		atomic_store(&g_run, 0);
		for (i = 0; i < VIDEO_MAX_CHN; i++)
			chn_destroy(&g_osd[i]);
		return -1;
	}
	return 0;
}

void osd_stop(void)
{
	int i;

	if (atomic_exchange(&g_run, 0))
		pthread_join(g_thread, NULL);
	for (i = 0; i < VIDEO_MAX_CHN; i++) {
		if (g_osd[i].active)
			chn_destroy(&g_osd[i]);
	}
}
