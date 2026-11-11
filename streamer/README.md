# streamer

RKMPI로 만든 최소 RTSP 스트리밍 프로그램. 보드의 `rkipc`를 대체하는 것이 목표이며, 지금은 `/tmp`에서 실행하는 시험 단계다. 설계와 근거는 [docs/streamer/design.md](../docs/streamer/design.md).

- `/live/0`: H.265 1920×1080, `/live/1`: H.265 960×540 (둘 다 18fps, GOP 18, CBR)
- RTSP 554, 인증 없음, TCP·UDP. 오디오를 켜지 않으면 SDP에 오디오 트랙이 없다
- 날짜·시각 OSD, 정지 감지와 상태 파일, 앱 내부 재초기화

## 빌드 (VM)

SDK(`~/luckfox-pico`)를 한 번 빌드한 상태여야 한다.

```sh
make -C streamer            # 결과: streamer/build/streamer (저장소에 넣지 않음)
make -C streamer SDK=/다른/경로/luckfox-pico
```

경고는 오류로 처리한다(`-Wall -Wextra -Werror`).

## 보드에서 시험 실행

`/oem`과 부팅 스크립트는 바꾸지 않는다. 재부팅하면 `/tmp`가 비워지고 rkipc로 돌아간다.

```sh
adb push streamer/build/streamer /tmp/streamer
adb shell 'kill -TERM $(pidof rkipc)'      # 554 포트와 카메라를 비운다
adb shell 'export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib; cd /tmp; \
  start-stop-daemon -S -b -m -p /tmp/streamer.pid -x /tmp/streamer -- -l /tmp/streamer.log'
adb shell 'cat /tmp/streamer.status; tail /tmp/streamer.log'
adb shell 'kill -TERM $(cat /tmp/streamer.pid)'   # 종료
```

## 옵션과 설정

```
streamer [-c 설정파일] [-l 로그파일] [-o key=value]... [-h]
```

우선순위는 `-o` > 설정 파일 > 기본값. 모든 키와 기본값은 [streamer.conf.example](streamer.conf.example)와 `streamer -h`에 있다. 자주 쓰는 것:

| 키 | 기본 | 뜻 |
| --- | --- | --- |
| `fps` / `ae_fps_x10` | 18 / 120 | 실제 목표 fps와 rkaiq에 넘기는 표기 fps×10. 이 보드에서는 실제 fps = 표기 × 1.5 (design.md 4.1) |
| `raw_buf_cnt` | 3 | CIF raw 버퍼 수 |
| `vi_buf_cnt` / `vi_depth` | 3 / 1 | 메인 VI 버퍼 수, Depth |
| `stall_action` | `reinit` | 정지 때 `log`(기록만) 또는 `reinit`(파이프라인 재초기화) |
| `audio_enable` | 0 | 오디오. 보드에서 검증하지 않았으므로 마이크 확인 전에는 켜지 말 것 |

## 신호

| 신호 | 동작 |
| --- | --- |
| `TERM`, `INT` | 정상 종료. 15초 안에 해제가 끝나지 않으면 강제 종료 |
| `USR1` | 파이프라인 재초기화 1회 (시험용) |
| `USR2` | 정지 모의: 감시가 프레임이 멈춘 것으로 취급 (시험용) |

## 상태 파일

`/tmp/streamer.status`를 1초마다 갱신한다.

```
state=running        # starting | running | degraded | stalled | reinit | failed
uptime=10868.0       # 단조 시계(초)
cif_frames=360       venc0_frames=359     venc1_frames=359
cif_fps=18.0         venc0_fps=18.0       venc1_fps=18.0
cif_buf_in_drv=2     cif_buf_total=3
stalls=0             reinits=0            reinit_ok=0
```

- `running`: 프레임이 정상으로 나옴. `degraded`: 입력 fps가 목표의 70% 미만
- `stalled`/`reinit`: 정지를 감지했고 재초기화 중. `failed`: 재초기화로 복구하지 못함(재부팅 필요)
- 파일 수정 시각이 5초 넘게 오래됐으면 프로그램이 멈췄거나 죽은 것으로 본다

## 소스 구성

| 파일 | 역할 |
| --- | --- |
| `src/main.c` | 인자, 신호, 시작·재초기화·종료 순서 |
| `src/config.c` | 설정 표(기본값, 파일, `-o`) |
| `src/isp.c` | rkaiq 초기화·해제, raw 버퍼 수, AE 고정 fps |
| `src/video.c` | MPI sys, VI, VENC, bind, 스트림 스레드 |
| `src/rtsp.c` | librtsp 래퍼(mutex, 이벤트 스레드) |
| `src/osd.c` | 시각 OSD(내장 5×7 글꼴, RGN overlay) |
| `src/watch.c` | 정지 감지, 상태 파일, 재초기화 요청 |
| `src/audio.c` | AI→AENC(G.711A)→RTSP, 기본 꺼짐 |
| `src/log.c` | 로그 |
