#ifndef STREAMER_ISP_H
#define STREAMER_ISP_H

#include "config.h"

/*
 * rkaiq(ISP 3A) 초기화와 해제. 카메라 0, 선형(비 HDR) 모드만 다룬다.
 * raw_buf_cnt 는 rkaiq 가 CIF 에서 받는 raw 버퍼(rkraw_rx) 개수로, init 전에 지정한다.
 */
int isp_start(const struct config *cfg);
void isp_stop(void);

#endif
