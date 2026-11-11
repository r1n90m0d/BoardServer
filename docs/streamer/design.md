# streamer 설계

rkipc를 대체할 최소 RTSP 스트리밍 프로그램. I2 일정과 별개인 병행 작업이며, 지금은 보드에서 `/tmp` 실행으로만 시험한다(`/oem`, 부팅 스크립트는 건드리지 않음).

- 대상 보드: LuckFox Pico Zero (RV1106G3, 256MB), 센서 IMX415
- 배경 기록: `docs/progress/I2.md` (live/0 저fps, 입력 10fps 고정, 파이프라인 정지, 빈 오디오 트랙)
- 이 문서에서 "확실"은 소스·보드 조회로 확인한 것, "추측"·"확인 필요"는 그렇지 않은 것

## 0. 목표와 범위

| 항목 | 내용 |
| --- | --- |
| RTSP | 서버 mediamtx가 접속해 가져가는 pull 방식(ICD 4.1.2). 554 포트, 인증 없음, RTSP over TCP 지원 |
| `/live/0` | H.265 1920×1080, 18fps, GOP 18~36(기본 18), CBR 4096kbps |
| `/live/1` | H.265 960×540, 18fps, GOP 18~36(기본 18), CBR 1024kbps |
| 오디오 | 옵션, 기본 꺼짐. 꺼져 있으면 SDP에 오디오 트랙을 선언하지 않음 |
| 버퍼 | 메인 VI 버퍼 수·Depth, CIF raw 버퍼 수를 설정값으로 노출. 기본 VI 3 / Depth 1 / raw 3 |
| OSD | 날짜·시각(localtime, `/etc/TZ` 반영) |
| 상태 보고 | 상태 파일로 "프레임이 실제로 나오는지"를 heartbeat가 읽을 수 있게 함 |
| 정지 감지 | CIF·VENC 프레임 카운터가 일정 시간 늘지 않으면 감지하고 로그 |

하지 않는 것: 웹 설정 서버, 녹화, RTMP, IVS/NPU, JPEG 캡처, 주야 전환, rkipc ini 호환.

## 1. 파이프라인 구조

```
IMX415 (3864×2192@20, 10bit)                         센서 모드 A
   │ MIPI CSI-2 4lane
   ▼
rkcif (CIF, ping-pong)  ── raw 버퍼 raw_buf_cnt개(기본 3, 각 약 10.2 MiB, CMA)
   │ rkraw_rx (rkaiq가 관리)
   ▼
rkisp (unite 모드: 프레임당 2회 읽기)  ◀── rkaiq (AE/AWB 등, IQ json)
   ├─ rkisp_mainpath ─▶ VI dev0/chn0 1920×1080 NV12  버퍼 vi_buf_cnt(3), Depth vi_depth(1)
   │                        │ RK_MPI_SYS_Bind
   │                        ▼
   │                    VENC chn0 H.265 CBR 4096k, GOP 18 ─▶ 스트림 스레드 0 ─▶ RTSP /live/0
   │                        ▲ RGN overlay(OSD, 핸들 0)
   └─ rkisp_selfpath ─▶ VI dev0/chn1 960×540 NV12   버퍼 2, Depth 0
                            │ RK_MPI_SYS_Bind
                            ▼
                        VENC chn1 H.265 CBR 1024k, GOP 18 ─▶ 스트림 스레드 1 ─▶ RTSP /live/1
                            ▲ RGN overlay(OSD, 핸들 1)

감시 스레드: /proc/rkcif-mipi-lvds 의 frame amount, VENC0/1 출력 프레임 수를 1초마다 확인
RTSP 이벤트 스레드: rtsp_do_event 를 주기적으로 호출(프레임이 없어도 접속을 받음)
(옵션) AI dev0 ─▶ AENC G.711A ─▶ 오디오 스레드 ─▶ RTSP 오디오 트랙 (기본 꺼짐)
```

- VI 채널 번호와 ISP 출력 경로의 대응(0=mainpath, 1=selfpath, 2=bypasspath)은 SDK 예제 도움말과 rkipc 구성으로 확인(확실)
- 두 채널 모두 VI→VENC bind만 쓰고 `RK_MPI_VI_GetChnFrame`은 쓰지 않음
- fps: 센서의 프레임 길이(VTS)를 늘려 **센서가 실제로 18fps를 내게** 한다. rkaiq AE의 고정 fps(`isFpsFix`, `FpsValue`)에 `ae_fps_x10`(기본 120 = 12.0)을 주면 VTS 3750, 실제 18.0fps가 된다(4.1절). VENC의 src/dst 프레임레이트는 `fps`(기본 18). VI 채널의 프레임레이트 제어는 쓰지 않음(rkipc처럼 0으로 둠)
- 타임스탬프: VENC가 주는 `u64PTS`(단조 시계 기준, MPI 가이드 6.9절)를 그대로 RTSP에 넘김

### 스레드

| 스레드 | 역할 |
| --- | --- |
| main | 초기화, 신호 처리, 재초기화·종료 순서 제어 |
| venc0, venc1 | `RK_MPI_VENC_GetStream` → `rtsp_tx_video` → `ReleaseStream`, 프레임 카운터 증가 |
| rtsp | 10ms마다 `rtsp_do_event` (librtsp 호출은 모두 하나의 mutex로 보호) |
| watch | 1초마다 카운터 확인, 상태 파일 갱신, 정지 판정 (4단계) |
| osd | 1초마다 시각 비트맵 갱신 (3단계) |
| audio | AENC 스트림 → `rtsp_tx_audio` (5단계, 기본 꺼짐) |

## 2. 참고 근거

rkipc 소스를 복사하지 않고, 아래에서 호출 순서와 속성 값을 참고해 새로 작성한다. 경로는 SDK(`~/luckfox-pico`) 기준.

| 가져올 것 | 출처 (파일:줄) |
| --- | --- |
| rkaiq 초기화 순서: `enumStaticMetasByPhyId` → `preInit_devBufCnt("rkraw_rx", N)` → `preInit_scene` → `sysctl_init` → `prepare` → `start`, `HDR_MODE` 환경변수 | `project/app/rkipc/rkipc/common/isp/rv1106/isp.c`:72-150 |
| raw 버퍼 수가 2로 고정된 곳(설정값으로 바꿀 지점) | 같은 파일:104 |
| AE 고정 fps 설정(`getExpSwAttr`/`setExpSwAttr`, `isFpsFix`, `FpsValue`) | 같은 파일:269-283 |
| rkaiq 해제 순서: `sysctl_stop` → `sysctl_deinit` | 같은 파일:1933-1944 |
| VI dev 초기화(`GetDevAttr`/`SetDevAttr`/`EnableDev`/`SetDevBindPipe`) | `project/app/rkipc/rkipc/src/rv1106_ipc/video/video.c`:665-706, `media/samples/simple_test/simple_vi_bind_venc_rtsp.c`:152-196 |
| 메인 VI 채널 속성(DMABUF, `u32BufCount`, `u32Depth`, `stMaxSize`) — 버퍼 2 고정 지점 | `video.c`:714-748 (720행 `buf_cnt = 2`, 737행 `u32Depth = 1`) |
| 서브 VI 채널 속성(Depth 0) | `video.c`:1106-1136 |
| VENC 생성(H.265 CBR, 스트림 버퍼 수·크기), `StartRecvFrame`, VI→VENC bind | `video.c`:752-826, 1053-1068, 예제 `simple_vi_bind_venc_rtsp.c`:112-150, 448-460 |
| VENC 프레임레이트 필드(`u32SrcFrameRateNum` 등) | `video.c`:2551-2610 |
| 스트림 꺼내기 루프(`GetStream` → `Handle2VirAddr` → 전송 → `ReleaseStream`) | `video.c`:117-170, 예제:67-110 |
| RGN overlay 생성·VENC 부착·비트맵 설정(ARGB8888) | `video.c`:2925-3010 |
| OSD 시각 문자열(`localtime`) | `project/app/rkipc/rkipc/common/osd/osd.c`:142-154 |
| librtsp 사용법(세션, 코덱, 타임스탬프 동기) | `project/app/rkipc/rkipc/common/rtsp/rtsp.c`:25-55, 예제:426-436 |
| rkipc가 오디오 트랙을 항상 넣는 곳 | `rtsp.c`:34-38, 50-54 (`rtsp_set_audio`를 조건 없이 호출) |
| AI·AENC 초기화(G.711A) | `project/app/rkipc/rkipc/common/audio/rv1106/audio.c`:237-301, 474-500, 76-95, 예제 `media/samples/simple_test/simple_ai_bind_aenc.c` |
| VI Depth와 버퍼 수 관계(주의사항) | `media/rockit/rockit/mpi/doc/Rockchip_Developer_Guide_MPI.pdf` 7.9 `VI_CHN_ATTR_S` (111쪽). 로컬 변환본 `docs/vendor/` |
| PTS는 단조 시계 | 같은 문서 6.9 `VIDEO_FRAME_INFO_S` |

LuckFox RKMPI 예제(github.com/LuckfoxTECH/luckfox_pico_rkmpi_example, SDK 안에는 없음): `example/luckfox_pico_rtsp_opencv/src/luckfox_mpi.cc`의 `vi_dev_init`(21행), `vi_chn_init`(67행), `venc_init`(91행)이 SDK 예제와 같은 구조다. 이 예제들은 bind가 아니라 `GetChnFrame`+OpenCV 경로(`u32Depth = 2`, 80행)라서 구조는 참고하지 않고, 호출 순서가 SDK 예제와 같다는 확인에만 썼다.

MPI 가이드의 Depth 주의사항(GetChnFrame을 쓰지 않으면 Depth 0 권장)과 rkipc의 메인 Depth 1은 다르다. 기본값은 요구대로 Depth 1로 두고 설정값으로 바꿀 수 있게 한다. Depth 0 조합은 시험 후보(미실시).

## 3. RTSP 서버

**결정: SDK의 librtsp(`rtsp_demo`)를 정적 링크해서 쓴다.**

- 위치: `media/out/lib/librtsp.a`, 헤더 `media/out/include/rtsp_demo.h` (라이브러리 소스는 SDK에 없음, 정적 라이브러리만 있음)
- rkipc가 쓰는 것과 같은 구현이라 지금 서버(mediamtx)와의 호환이 이미 확인되어 있음: 554, 인증 없음, TCP interleaved·UDP 모두 지원
- **오디오 트랙 없는 SDP**: 헤더 API상 오디오는 `rtsp_set_audio()`를 호출한 세션에만 선언된다. rkipc는 이 함수를 조건 없이 호출한다(`rtsp.c` 34·50행). streamer는 오디오가 꺼져 있으면 호출하지 않는다. 실제 SDP에 `m=audio`가 없는지는 1단계 보드 시험에서 확인한다(확인 필요 → 시험 항목)
- H.265 파라미터(VPS/SPS/PPS)는 `rtsp_set_video(..., NULL, 0)` 뒤 첫 키프레임에서 라이브러리가 뽑는다(rkipc와 같은 방식)
- 주의: librtsp는 `rtsp_do_event()`가 불려야 접속을 처리한다. rkipc는 프레임을 보낼 때만 부르므로 파이프라인이 멈추면 접속도 못 받는다(정지 때 554 대기열이 쌓이던 원인으로 보임, 추측). streamer는 별도 스레드에서 주기적으로 부른다
- librtsp에는 스레드 안전 보장이 없으므로 모든 호출을 mutex 하나로 묶는다
- 대안(채택 안 함): live555나 자체 구현은 크기·검증 부담이 큼. librtsp에서 문제가 확인되면 그때 다시 검토

## 4. 센서 모드

드라이버(`sysdrv/source/kernel/drivers/media/i2c/imx415.c`) 지원 모드 중 후보 둘:

| | A: 3864×2192@20, 10bit | B: 1944×1097@30, 12bit (2×2 binning) |
| --- | --- | --- |
| 드라이버 항목 | 1266행~ (594M 레지스터) | 1455행~ (`imx415_linear_12bit_1932x1096_594M_regs`) |
| ISP | unite 모드 필요(가상 ISP 최대 해상도 초과 → 프레임당 2회 읽기) | unite 불필요(추측: 폭이 ISP 한계 안. 확인 필요) |
| raw 버퍼 1개 | 약 10.2 MiB | 약 3.2 MiB (12bit 패킹 기준, 계산값) |
| fps | 표기 최대 20. 실제 센서 주기는 4.1절 참고 | 표기 최대 30 (실제 값 확인 필요) |
| 화질 | 4K에서 1080p로 축소 → 선명 | binning이라 해상력은 낮지만 저조도 감도는 유리 |
| IQ 파일 | 현재 json 그대로 | 현재 json은 4K 기준. 해상도별 보정(LSC 등)이 맞지 않을 수 있음 |
| 위험 | 지금까지 본 10fps 고정·정지가 이 모드에서 발생 | 미검증. 색·렌즈 보정, AE 동작 확인 필요 |

### 4.1 모드 A의 실제 프레임레이트 (1단계 보드 시험에서 확인, 확실)

드라이버는 이 모드를 20fps(VTS 2250)로 표기하지만, **센서는 실제로 그 1.5배 속도로 프레임을 낸다.** 근거:

| VTS | AE에 준 fps(표기) | 캡처한 프레임 간격(스트림 PTS) | 평균 |
| --- | --- | --- | --- |
| 2250 (rkipc, fps 20) | 20 | 33ms와 67ms가 섞임 / 100ms만 | 18.0·20.0fps / 10.0fps |
| 2500 (streamer, 표기 18) | 18 | 37ms와 74ms가 섞임 | 20.0 → 18.0fps로 변함 |
| **3750** (streamer, 표기 12) | 12 | **55~56ms만** | **18.00fps, 균일** |

- 간격이 항상 한 값의 배수다: VTS 2250이면 33.3ms(= 30fps), VTS 2500이면 37ms(= 27fps). 즉 센서 주기는 `VTS / 67500`초이고, 지금까지 본 "18fps", "20fps", "10fps"는 30fps로 들어오는 프레임 중 일부만 받는 패턴이었다(3장 중 2장, 2장 중 1장, 3장 중 1장 등). `/proc/rkcif-mipi-lvds`의 `rate`(직전 두 프레임 간격)와 `readout: 32 ms`도 같은 값을 가리킨다
- 4K raw를 unite 모드(2회 읽기)로 30fps 처리할 여유가 없어 CIF 버퍼가 모자랄 때마다 프레임이 빠지는 것으로 보인다(추측). 10fps 고정과 pingpong 메시지가 이 과정에서 나온다
- VTS를 3750으로 늘리면 센서가 실제 18fps로 내고, 파이프라인이 모든 프레임을 받는다(간격 균일, 손실 없음)
- 왜 센서가 표기의 1.5배로 도는지는 모른다(확인 필요: 모듈 발진기 주파수와 드라이버 레지스터 표의 전제가 다를 가능성). 실제 fps = 표기 fps × 1.5 관계는 VTS 세 값에서 일치
- 그래서 설정을 둘로 나눈다: `fps`(실제 목표, 인코더용)와 `ae_fps_x10`(rkaiq에 넘기는 표기 fps×10). 기본 18 / 120. 실제 20fps를 원하면 `fps=20`, `ae_fps_x10=133`
- 노출 영향: 프레임이 길어지면 AE가 쓸 수 있는 최대 노출 시간도 늘어난다(저조도에 유리, 움직임 번짐은 늘 수 있음)

**구현은 A.** 지금 보드 설정(부팅 때 `luckfox-config`가 unite를 1로 둠) 그대로 동작한다.

B 시험 방법(설계만, 구현하지 않음):
1. 조회: `v4l2-ctl -d /dev/v4l-subdev2 --list-subdev-mbus-codes`에 12bit 코드가 있는지(확인함: SGBRG12 있음), `media-ctl -p`로 현재 포맷 확인
2. rkipc를 멈춘 상태에서 `media-ctl --set-v4l2 '"m00_b_imx415 4-0037":0[fmt:SGBRG12_1X12/1944x1097]'`로 센서 포맷을 바꾸고 CIF·ISP 링크 포맷도 맞춘 뒤 streamer를 실행. rkaiq가 시작할 때 포맷을 다시 고르는지(IQ json의 해상도 목록에 따라) 확인 필요
3. unite는 부팅 때 정해지므로(`luckfox-config set_csi_unite`, 재부팅 필요) B를 제대로 보려면 unite 0으로 부팅해야 함 → 부팅 설정 변경이라 별도 승인 후 진행
4. 확인 항목: 입력 fps, 10fps 고정·정지 재현 여부, 색·주변부 밝기·선명도(4K 모드 캡처와 비교), CMA 사용량
5. IQ가 맞지 않으면 1944×1097용 IQ 조정이 필요(범위 밖)

## 5. 오디오

**하드웨어 확인(조회, 확실)**: 보드에 SoC 내장 코덱 사운드 카드가 있다 — `/proc/asound/cards`에 `rv1106-acodec`, 캡처 장치 `pcmC0D0c`. `S60micinit`이 부팅 때 마이크 게인을 설정한다. **마이크가 실제로 연결돼 있는지는 확인하지 않았다(확인 필요).**

마이크 확인 방법(실행하지 않음, 오디오를 켜기 전에 할 것):
1. rkipc·streamer의 오디오가 꺼진 상태에서 `arecord -D hw:0,0 -f S16_LE -r 8000 -c 2 -d 5 /tmp/mic.wav`
2. 조용할 때와 소리를 낼 때 각각 녹음해 VM으로 가져와 진폭(RMS, 최대값)을 비교. 두 경우가 구분되면 마이크가 연결된 것
3. 좌·우 채널 중 어느 쪽에 신호가 있는지 확인(`S60micinit`은 `ADC Mode`를 `SingadcL`로 둠)

오디오 경로(5단계, 기본 꺼짐):

```
AI dev0/chn0 (hw:0,0, 8000Hz, S16, 카드 2채널) ─bind─▶ AENC chn0 (G.711A, 8000Hz, mono)
   ─▶ audio 스레드: RK_MPI_AENC_GetStream ─▶ rtsp_tx_audio(/live/0, /live/1)
```

- 오디오가 켜졌을 때만 세션에 `rtsp_set_audio(G711A)`, `rtsp_set_audio_sample_rate(8000)`, `rtsp_set_audio_channels(1)`, `rtsp_sync_audio_ts`를 호출
- 켰는데 AI/AENC 초기화가 실패하면 오디오 트랙을 선언하지 않고 영상만 송출(빈 트랙을 만들지 않는 것이 원칙)
- VQE(잡음 억제)·AED 등은 넣지 않음
- 5단계는 코드 구조와 빌드까지만 하고, 마이크 확인 전에는 켜서 시험하지 않는다

## 6. 정지 감지와 복구

**감지 (4단계)**
- 1초마다 세 카운터를 본다: CIF 누적 프레임(`/proc/rkcif-mipi-lvds`의 `frame amount`), VENC0·VENC1 출력 프레임 수(스트림 스레드가 센 값)
- 시작(또는 재초기화) 후 `stall_grace_s`(기본 15초) 동안은 판정하지 않음
- CIF 또는 VENC0이 `stall_timeout_s`(기본 5초) 동안 늘지 않으면 정지로 판정. 판정 시 로그에 시각, 업타임, 세 카운터, CIF 드라이버 버퍼 수(`buf_cnt in drv`, `total buf_cnt`)를 남김
- 10fps 고정 감지: 최근 10초 평균 입력 fps가 설정 fps의 70% 미만이면 `degraded`로 표시하고 로그(복구 동작은 하지 않음)

**상태 파일** `/tmp/streamer.status` (1초마다 임시 파일에 쓴 뒤 rename)

```
state=running|starting|degraded|stalled|reinit|failed
uptime=<초, 단조 시계>  cif_frames=...  venc0_frames=...  venc1_frames=...
cif_fps=...  venc0_fps=...  venc1_fps=...  stalls=<누적>  reinits=<누적>  reinit_ok=<누적>
```

**heartbeat 연동 제안** (heartbeat는 이번에 수정하지 않음)
- 지금 heartbeat는 `/proc/*/comm`이 `rkipc`인 프로세스가 있는지만 본다(`heartbeat/heartbeat.c` 114·351행). 파이프라인이 멈춰도 true가 된다
- 제안: heartbeat가 상태 파일을 읽어 `state`가 `running` 또는 `degraded`이고 파일 수정 시각이 최근 5초 안이면 `rtsp_ok=true`. 파일이 없으면 지금처럼 프로세스 이름(`rkipc`)으로 판단(rkipc와 공존)
- heartbeat를 바꾸기 전에 streamer로 교체하면 `rtsp_ok=false`가 된다(프로세스 이름이 `streamer`). 임시로 실행 파일 이름을 `rkipc`로 두는 방법도 있으나 권하지 않음

**복구 전략** (`stall_action` 설정)

| 값 | 동작 |
| --- | --- |
| `log` | 감지·로그·상태 파일만 |
| `reinit` (기본) | 앱 내부에서 파이프라인 전체를 해제하고 다시 초기화. `reinit_max`(기본 3)회까지, 이후에는 `failed`로 두고 로그만 |

재초기화 순서: 스트림 스레드 정지 → OSD 해제 → unbind → VENC 정지·파괴 → VI 채널·dev 해제 → `RK_MPI_SYS_Exit` → rkaiq `stop`/`deinit` → (1초 대기) → rkaiq 초기화부터 다시. RTSP 서버와 세션은 유지한다(접속한 클라이언트는 영상이 다시 나올 때까지 대기).

- **복구되는지는 모른다(시험 항목).** 근거: rkipc에서는 정지 후 프로세스를 죽이고 다시 띄워도 CIF가 1프레임에서 멈췄고 재부팅만 복구했다. 프로세스 재시작으로 안 됐다면 앱 내부 재초기화도 안 될 가능성이 높다(추측). 다만 rkipc는 SIGKILL로 죽였기 때문에 드라이버 쪽 정리가 순서대로 되지 않았을 수 있어, 순서대로 해제하는 재초기화는 결과가 다를 수 있다
- 해제 함수가 멈춰 돌아오지 않을 수 있다(rkipc는 정지 상태에서 SIGTERM에 응답하지 않았음). 재초기화가 `reinit_timeout_s`(기본 30초) 안에 끝나지 않으면 상태를 `failed`로 쓰고 로그를 남긴다
- 앱이 스스로 재부팅하지는 않는다. `failed` 상태를 보고 재부팅할지는 바깥(감시 스크립트나 heartbeat/서버)이 정한다. 재부팅 루프 방지(시간당 횟수 상한)는 그쪽 설계에서 다룸
- 시험용: `SIGUSR1`을 보내면 정지가 아니어도 재초기화를 한 번 수행(재초기화 경로 자체가 동작하는지 확인용)
- 시험용: `SIGUSR2`를 보내면 감시가 프레임 카운터를 멈춘 것으로 취급한다(다음 재초기화가 끝날 때까지). 실제 정지 없이 "정지 감지 → 재초기화 요청 → 재초기화 → 복구 판정" 흐름을 확인하는 용도
- 종료 요청(`SIGTERM`) 뒤 15초가 지나도 해제가 끝나지 않으면 감시 스레드가 프로세스를 강제 종료한다(정지 상태의 rkipc가 SIGTERM에 응답하지 않던 문제 대비)

## 7. 빌드와 배포

**디렉토리** (`~/BoardServer/streamer/`)

```
streamer/
  Makefile
  streamer.conf.example   설정 예시(모든 키와 기본값)
  src/
    main.c      인자·신호·수명 주기
    config.c/h  설정(기본값, key=value 파일, 명령행)
    log.c/h     로그(시각 + 단계, stderr 또는 파일)
    isp.c/h     rkaiq 초기화·해제, raw 버퍼 수, AE 고정 fps
    video.c/h   MPI sys, VI dev/채널, VENC, bind, 스트림 스레드
    rtsp.c/h    librtsp 래퍼(mutex, 이벤트 스레드)
    osd.c/h     시각 OSD (3단계)
    watch.c/h   정지 감지, 상태 파일, 재초기화 요청 (4단계)
    audio.c/h   AI→AENC→RTSP (5단계, 기본 꺼짐)
```

**빌드** (VM): `make -C streamer` — SDK 경로는 `SDK ?= $(HOME)/luckfox-pico`
- 툴체인: `$(SDK)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc`
- 헤더·라이브러리: `$(SDK)/output/out/media_out/{include,lib}` (rockit, rockchip_mpp, rkaiq 동적 링크 / rtsp 정적 링크). SDK를 한 번 빌드한 상태여야 함
- 경고를 오류로 처리(`-Wall -Wextra -Werror`), SDK 헤더는 `-isystem`으로 포함
- 산출물 `streamer/build/streamer`는 `.gitignore` 대상(저장소에 넣지 않음)

**배포(시험)**: `/oem`과 부팅 스크립트를 바꾸지 않는 방식만 쓴다

```sh
adb push streamer/build/streamer /tmp/streamer
adb shell 'kill -TERM $(pidof rkipc)'            # rkipc 정지(재부팅하면 다시 뜸)
adb shell 'export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib; cd /tmp; \
  start-stop-daemon -S -b -m -p /tmp/streamer.pid -x /tmp/streamer -- -l /tmp/streamer.log'
```

- rkipc와 같은 보드 라이브러리(`/oem/usr/lib`의 librockit, librkaiq 등)와 IQ 파일(`/oem/usr/share/iqfiles`)을 쓴다
- 종료: `kill -TERM $(cat /tmp/streamer.pid)`. 재부팅하면 `/tmp`가 비워지고 rkipc로 돌아간다
- 정식 설치(`/oem` 배치, `RkLunch.sh`에서 rkipc 대신 실행, `install_board.sh` 반영)는 시험 결과를 본 뒤 결정(이 문서 범위 밖)

**설정** (우선순위: 명령행 > 설정 파일 > 기본값)

| 키 | 기본 | 뜻 |
| --- | --- | --- |
| `iq_dir` | `/oem/usr/share/iqfiles` | IQ json 디렉토리 |
| `rtsp_port` | 554 | |
| `fps` | 18 | 실제 목표 fps (VENC 프레임레이트) |
| `ae_fps_x10` | 120 | rkaiq AE 고정 fps×10, 센서 표기 기준. 실제 fps = 이 값/10 × 1.5 (4.1절). 0이면 고정 안 함 |
| `raw_buf_cnt` | 3 | CIF raw 버퍼 수 |
| `vi_buf_cnt` / `vi_depth` | 3 / 1 | 메인 VI 버퍼 수, Depth |
| `sub_enable` | 1 | 서브 채널 사용(2단계부터) |
| `sub_vi_buf_cnt` / `sub_vi_depth` | 2 / 0 | 서브 VI 버퍼 수, Depth |
| `main_width`/`main_height`/`main_kbps`/`main_gop` | 1920/1080/4096/18 | |
| `sub_width`/`sub_height`/`sub_kbps`/`sub_gop` | 960/540/1024/18 | |
| `osd_enable` | 1 | 시각 OSD (3단계부터) |
| `stall_timeout_s`/`stall_grace_s` | 5 / 15 | 정지 판정 시간, 시작 유예 |
| `stall_action` | `reinit` | `log` 또는 `reinit` |
| `reinit_max`/`reinit_timeout_s` | 3 / 30 | 재초기화 횟수 상한, 제한 시간 |
| `status_file` | `/tmp/streamer.status` | |
| `audio_enable` | 0 | 오디오(5단계, 마이크 확인 전에는 켜지 않음) |

## 8. 단계별 구현 계획

각 단계는 VM에서 경고 없이 빌드되어야 다음으로 넘어가고, 단계마다 커밋한다.

| 단계 | 내용 | 보드 확인 |
| --- | --- | --- |
| 1 | 영상 1채널: 설정·로그, rkaiq, VI chn0, VENC0, RTSP `/live/0`, 정상 종료 | ffprobe(TCP)로 코덱·해상도·fps, SDP에 오디오 없음 |
| 2 | 2채널: VI chn1, VENC1, `/live/1` | 두 스트림 동시 확인 |
| 3 | OSD: 내장 비트맵 글꼴(숫자·`-`·`:`)로 `YYYY-MM-DD HH:MM:SS`, 채널별 RGN | 최종 시험에서 캡처로 확인 |
| 4 | 정지 감지: 감시 스레드, 상태 파일, 재초기화(`SIGUSR1` 포함) | 최종 시험 |
| 5 | 오디오 구조: AI→AENC→RTSP, 기본 꺼짐 | 빌드만(켜지 않음) |
| 6 | 센서 모드 B | 설계만(4절), 구현 없음 |

**최종 시험**(전체 구현 후): ffprobe로 두 스트림과 SDP 확인 / 시작·종료 6회, 매회 3분, CIF 누적 프레임 1초 기록(정지·10fps 고정 횟수) / 정지 시 재초기화 복구 여부 / 10분 연속 송출에서 메모리·CPU를 rkipc와 비교 / 끝나면 재부팅해 rkipc 복귀 확인.

## 9. 알려진 위험

- 정지의 원인이 rkaiq/ISP 읽기 경로라면(추측) streamer도 같은 라이브러리·드라이버를 쓰므로 정지가 그대로 날 수 있다. streamer가 줄 수 있는 것은 감지·기록·재초기화 시도까지
- raw 버퍼 3개는 rkipc 시험(6회)에서 10fps 고정을 없앴지만 정지 1회, 18fps+pingpong 연속 2회가 있었다. fps 18은 rkipc(raw 2개) 시험에서 정지 3/6이었다. 기본값 조합(VI 3 / raw 3 / fps 18)은 이번에 처음 시험한다
- 보드의 `librkaiq.so`는 SDK의 것과 다른 파일이다(크기·md5 다름). SDK 헤더로 빌드한 rkipc가 보드에서 동작하므로 같은 방식으로 빌드하되, API 불일치 가능성은 남는다
- librtsp는 소스가 없어 오류가 나면 고칠 수 없다
