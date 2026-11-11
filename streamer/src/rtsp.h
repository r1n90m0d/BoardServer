#ifndef STREAMER_RTSP_H
#define STREAMER_RTSP_H

#include <stdint.h>

#define RTSP_MAX_SESSIONS 2

/*
 * SDK librtsp(rtsp_demo) 래퍼.
 * - librtsp 는 스레드 안전을 보장하지 않으므로 모든 호출을 mutex 하나로 묶는다.
 * - 이벤트 스레드가 주기적으로 rtsp_do_event 를 불러, 영상이 멈춰도 접속을 받는다.
 * - 오디오 트랙은 with_audio 가 참일 때만 SDP 에 선언한다.
 */
int rtsp_srv_start(int port);
int rtsp_srv_add_session(int idx, const char *path, int with_audio);
void rtsp_srv_send_video(int idx, const uint8_t *data, int len, uint64_t pts_us);
/* 오디오 트랙을 선언한 모든 세션으로 보낸다. */
void rtsp_srv_send_audio(const uint8_t *data, int len, uint64_t pts_us);
void rtsp_srv_stop(void);

#endif
