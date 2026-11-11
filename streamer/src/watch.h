#ifndef STREAMER_WATCH_H
#define STREAMER_WATCH_H

#include <signal.h>

#include "config.h"

/*
 * 파이프라인 감시.
 * - 1초마다 CIF 누적 프레임(/proc/rkcif-mipi-lvds)과 VENC 출력 프레임 수를 본다.
 * - 유예 시간이 지난 뒤 CIF 나 VENC0 이 stall_timeout_s 동안 늘지 않으면 정지로 판정한다.
 * - 상태 파일을 1초마다 갱신한다(임시 파일에 쓰고 rename).
 * - 정지 시 stall_action 이 reinit 이면 재초기화를 요청한다. 실제 해제·초기화는 main 이 한다.
 * - 해제가 멈춰 돌아오지 않는 경우를 대비해, 종료 요청 뒤 일정 시간이 지나면 프로세스를 끝낸다.
 */
int watch_start(const struct config *cfg, const volatile sig_atomic_t *quit_flag);
void watch_stop(void);

/* 재초기화 요청이 있으면 1 을 돌려주고 요청을 지운다. main 루프가 부른다. */
int watch_take_reinit_request(void);
/* 정지와 무관하게 재초기화를 한 번 요청한다(SIGUSR1, 시험용). */
void watch_request_reinit(void);
/*
 * 시험용(SIGUSR2): 다음 재초기화가 끝날 때까지 프레임 카운터가 멈춘 것으로 취급한다.
 * 실제 정지 없이 "정지 감지 -> 재초기화 요청" 경로를 확인하는 데 쓴다.
 */
void watch_simulate_stall(void);
/* main 이 재초기화를 시작·끝낼 때 알린다. ok 는 파이프라인을 다시 띄웠는지. */
void watch_reinit_begin(void);
void watch_reinit_end(int ok);

#endif
