#include "isp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rk_aiq_user_api2_ae.h>
#include <rk_aiq_user_api2_sysctl.h>

#include "log.h"

#define CAM_ID 0

static rk_aiq_sys_ctx_t *g_ctx;

/*
 * AE 의 프레임레이트를 고정한다. AE 는 이 값으로 센서의 프레임 길이(VTS)를 정한다.
 *
 * 주의: 이 보드(IMX415 3864x2192 "20fps" 모드)에서는 센서가 드라이버 표기의 1.5배로
 * 프레임을 낸다(표기 20fps = VTS 2250 일 때 실제 주기 33.3ms). 그래서 여기 넘기는 값은
 * "표기 fps"이고 실제 fps 는 그 1.5배다. 예: 12.0 -> VTS 3750 -> 실제 18fps.
 * 근거와 측정은 docs/streamer/design.md 4.1 절.
 */
static int set_fixed_fps(float fps)
{
	Uapi_ExpSwAttrV2_t attr;
	XCamReturn ret;

	memset(&attr, 0, sizeof(attr));
	ret = rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &attr);
	if (ret != XCAM_RETURN_NO_ERROR) {
		LOGE("isp: getExpSwAttr 실패 %d", (int)ret);
		return -1;
	}
	attr.stAuto.stFrmRate.isFpsFix = true;
	attr.stAuto.stFrmRate.FpsValue = fps;
	ret = rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, attr);
	if (ret != XCAM_RETURN_NO_ERROR) {
		LOGE("isp: setExpSwAttr 실패 %d", (int)ret);
		return -1;
	}
	return 0;
}

int isp_start(const struct config *cfg)
{
	rk_aiq_static_info_t info;
	const char *sensor;
	XCamReturn ret;

	/* rkaiq 는 init 전에 HDR_MODE 환경변수를 읽는다. 0 = 선형 모드. */
	setenv("HDR_MODE", "0", 1);

	memset(&info, 0, sizeof(info));
	ret = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(CAM_ID, &info);
	if (ret != XCAM_RETURN_NO_ERROR || info.sensor_info.sensor_name[0] == '\0') {
		LOGE("isp: 센서 정보를 얻지 못함 (ret %d)", (int)ret);
		return -1;
	}
	sensor = info.sensor_info.sensor_name;
	LOGI("isp: 센서 %s, IQ %s, raw 버퍼 %d개", sensor, cfg->iq_dir, cfg->raw_buf_cnt);

	ret = rk_aiq_uapi2_sysctl_preInit_devBufCnt(sensor, "rkraw_rx", cfg->raw_buf_cnt);
	if (ret != XCAM_RETURN_NO_ERROR)
		LOGW("isp: raw 버퍼 수 지정 실패 %d (기본값으로 진행)", (int)ret);

	ret = rk_aiq_uapi2_sysctl_preInit_scene(sensor, "normal", "day");
	if (ret != XCAM_RETURN_NO_ERROR) {
		LOGE("isp: preInit_scene 실패 %d", (int)ret);
		return -1;
	}

	g_ctx = rk_aiq_uapi2_sysctl_init(sensor, cfg->iq_dir, NULL, NULL);
	if (g_ctx == NULL) {
		LOGE("isp: sysctl_init 실패");
		return -1;
	}
	ret = rk_aiq_uapi2_sysctl_prepare(g_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL);
	if (ret != XCAM_RETURN_NO_ERROR) {
		LOGE("isp: prepare 실패 %d", (int)ret);
		rk_aiq_uapi2_sysctl_deinit(g_ctx);
		g_ctx = NULL;
		return -1;
	}
	ret = rk_aiq_uapi2_sysctl_start(g_ctx);
	if (ret != XCAM_RETURN_NO_ERROR) {
		LOGE("isp: start 실패 %d", (int)ret);
		rk_aiq_uapi2_sysctl_deinit(g_ctx);
		g_ctx = NULL;
		return -1;
	}
	if (cfg->ae_fps_x10 == 0)
		LOGI("isp: 시작, AE fps 고정 안 함");
	else if (set_fixed_fps((float)cfg->ae_fps_x10 / 10.0f) != 0)
		LOGW("isp: AE fps %.1f 고정 실패 (AE 기본 동작으로 진행)", cfg->ae_fps_x10 / 10.0);
	else
		LOGI("isp: 시작, AE fps %.1f 고정(센서 표기 기준)", cfg->ae_fps_x10 / 10.0);
	return 0;
}

void isp_stop(void)
{
	if (g_ctx == NULL)
		return;
	rk_aiq_uapi2_sysctl_stop(g_ctx, false);
	rk_aiq_uapi2_sysctl_deinit(g_ctx);
	g_ctx = NULL;
	LOGI("isp: 해제");
}
