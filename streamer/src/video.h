#ifndef STREAMER_VIDEO_H
#define STREAMER_VIDEO_H

#include <stdint.h>

#include "config.h"

#define VIDEO_MAX_CHN 2

/*
 * 영상 경로: VI(dev0) 채널 -> (bind) -> VENC 채널 -> 스트림 스레드 -> RTSP 세션.
 * 채널 0 = ISP mainpath(메인), 채널 1 = ISP selfpath(서브).
 * video_start 는 RK_MPI_SYS_Init 부터, video_stop 은 RK_MPI_SYS_Exit 직전까지 맡는다.
 */
int video_start(const struct config *cfg);
void video_stop(void);
/* 채널이 지금까지 내보낸 인코딩 프레임 수(재초기화해도 이어서 센다). */
uint64_t video_frames_out(int chn);

#endif
