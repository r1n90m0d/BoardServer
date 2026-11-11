#ifndef STREAMER_AUDIO_H
#define STREAMER_AUDIO_H

#include "config.h"

#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_CHANNELS 1

/*
 * 오디오 경로(옵션, 기본 꺼짐): AI dev0/chn0 (hw:0,0) -> (bind) -> AENC chn0 (G.711A 8kHz mono)
 *   -> 오디오 스레드 -> RTSP 오디오 트랙.
 * RK_MPI_SYS_Init 뒤(video_start 뒤)에 불러야 한다.
 *
 * 주의: 이 경로는 보드에서 아직 실행해 본 적이 없다. 마이크 하드웨어 확인 전에는
 * audio_enable 을 켜지 말 것 (docs/streamer/design.md 5절).
 */
int audio_start(const struct config *cfg);
void audio_stop(void);

#endif
