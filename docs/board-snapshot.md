# 보드 런타임 스냅샷

- 새 스냅샷은 이 파일 맨 위에 추가함(최신이 위)
- 각 출력은 adb 조회 결과 원본. 터미널 색상 코드(ANSI)만 제거했고, 줄을 생략한 곳은 그렇게 표시함
- 조회 명령만 사용했고 보드 상태는 바꾸지 않았음
- WireGuard 개인키, `wg1.conf` 전문은 기록하지 않음 (`Address`/`Endpoint`/`AllowedIPs`/`ListenPort` 줄만 grep)
- 공개 저장소이므로 서버 공인 IP, WireGuard 공개키, wlan0 MAC은 `<redacted>`로 가림 (실제 값은 보드에서 다시 조회)

---

## 2026-11-10 17:19 KST (08:19 UTC) — 보드 1 (CAM-E099)

- adb 장치: `63d0e45945a2af66` (usb:3-2, adb가 보고한 model 문자열은 `Nexus_4`. `uname`으로 보면 luckfox 보드이고, 이 문자열이 어디서 오는지는 확인 안 함)
- 조회 시점 업타임 약 50분

### 조회 시 메모 (원본 출력은 아래)

- 보드 시계: VM과 같은 초(08:19:24 UTC)로 맞음
- **wg1 주소가 `123.123.123.123/24`** — I2 문서의 보드 1 VPN IP `10.0.1.2`와 다름. `wg1.conf`는 2026-11-09 06:08(보드 시각)에 수정됨
- **WireGuard 핸드셰이크 기록 없음**: `latest-handshakes` 값 0, `transfer 0 B received`
- heartbeat 실행 중(pid 541). 마지막 heartbeat 로그는 `transport error: Timeout was reached` (로그는 결과가 바뀔 때만 남으므로 이후에도 계속 타임아웃인 것으로 판단)
- 카메라 센서: dmesg에 `imx415 4-0037` 드라이버 메시지가 반복해서 찍힘, iqfiles에 `imx415_...json`이 있음 → **IMX415**
- rkipc 현재 설정은 I2 목표와 다름(아래 "rkipc 영상 설정" 참고): video.0은 1920×1080이지만 15fps, video.1은 2560×1440 15fps이고 `enable_venc_1 = 0`

### uname -a
```
$ adb shell uname -a
Linux luckfox 5.10.160 #1 Thu Aug 20 10:31:50 +09 2026 armv7l GNU/Linux
```

### date / 업타임
```
$ date   (VM)
VM 시각: 2026-11-10 17:19:24 KST
$ adb shell date
Tue Nov 10 08:19:24 UTC 2026
$ adb shell date -u
Tue Nov 10 08:19:24 UTC 2026
$ adb shell uptime
 08:19:24 up 50 min,  0 users,  load average: 10.21, 10.25, 9.94
$ adb shell cat /proc/uptime
3022.66 2417.96
```

### /proc/cmdline
```
$ adb shell cat /proc/cmdline
user_debug=31 storagemedia=emmc androidboot.storagemedia=emmc androidboot.mode=normal  rootwait earlycon=uart8250,mmio32,0xff4c0000 console=ttyFIQ0 root=/dev/mmcblk0p7 snd_soc_core.prealloc_buffer_size_kbytes=16 coherent_pool=0 blkdevparts=mmcblk0:32K(env),512K@32K(idblock),256K(uboot),32M(boot),512M(oem),256M(userdata),6G(rootfs) rootfstype=ext4 rk_dma_heap_cma=100M androidboot.fwver=uboot-08/22/2026
```

### wlan0 MAC
```
$ adb shell cat /sys/class/net/wlan0/address
<redacted>
```

### ip addr
```
$ adb shell ip addr
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host 
       valid_lft forever preferred_lft forever
2: ip6_vti0@NONE: <NOARP> mtu 1364 qdisc noop qlen 1000
    link/tunnel6 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00 brd 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00
3: ip6tnl0@NONE: <NOARP> mtu 1452 qdisc noop qlen 1000
    link/tunnel6 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00 brd 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00
4: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq qlen 1000
    link/ether <redacted> brd ff:ff:ff:ff:ff:ff
    inet 192.168.0.9/24 brd 192.168.0.255 scope global dynamic wlan0
       valid_lft 4198sec preferred_lft 3298sec
    inet6 fe80::57e:cdf7:4f6d:f115/64 scope link 
       valid_lft forever preferred_lft forever
5: usb0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast qlen 1000
    link/ether <redacted> brd ff:ff:ff:ff:ff:ff
    inet 172.32.0.93/16 brd 172.32.255.255 scope global usb0
       valid_lft forever preferred_lft forever
    inet 169.254.172.245/16 brd 169.254.255.255 scope global noprefixroute usb0
       valid_lft forever preferred_lft forever
    inet6 fe80::1881:1b:d0ad:7bd4/64 scope link 
       valid_lft forever preferred_lft forever
6: wg1: <POINTOPOINT,NOARP,UP,LOWER_UP> mtu 1420 qdisc noqueue qlen 1000
    link/[65534] 
    inet 123.123.123.123/24 scope global wg1
       valid_lft forever preferred_lft forever
```

### ip route
```
$ adb shell ip route
default via 192.168.0.1 dev wlan0  src 192.168.0.9  metric 3004 
10.0.0.100 dev wg1 scope link 
123.123.123.0/24 dev wg1 scope link  src 123.123.123.123 
169.254.0.0/16 dev usb0 scope link  src 169.254.172.245  metric 1005 
172.32.0.0/16 dev usb0 scope link  src 172.32.0.93 
192.168.0.0/24 dev wlan0 scope link  src 192.168.0.9  metric 3004 
```

### wg show
```
$ adb shell wg show
interface: wg1
  public key: <redacted>
  private key: (hidden)
  listening port: 51820

peer: <redacted>
  endpoint: <redacted>:51820
  allowed ips: 10.0.0.100/32
  transfer: 0 B received, 80.36 KiB sent
  persistent keepalive: every 25 seconds
$ adb shell wg show wg1 latest-handshakes
<redacted>	0
```

### WireGuard 설정 (키 제외)
```
$ adb shell ls -la /etc/wireguard/
total 32
drwx------    3 1000     1000          4096 Nov  4 06:01 .
drwxr-xr-x   18 1000     1000          4096 Jan  1  2021 ..
-rw-------    1 root     root            45 Nov  3 10:13 privatekey
-rw-------    1 root     root            45 Nov  3 10:13 publickey
-rw-------    1 root     root           267 Nov  9 06:08 wg1.conf
-rw-------    1 root     root           270 Nov  4 03:40 wg1.conf.bak-20261104
-rw-------    1 root     root           270 Nov  4 05:38 wg1.conf.bak-final
drwx------    2 root     root          4096 Nov  3 10:31 wg100
$ adb shell 'grep -h -i -E "^[[:space:]]*(Address|Endpoint|AllowedIPs|ListenPort)" /etc/wireguard/*.conf'
Address = 123.123.123.123/24
ListenPort = 51820
Endpoint = <redacted>:51820
AllowedIPs = 10.0.0.100/32
$ adb shell cat /var/log/wg1-boot.log
/usr/bin/wg-quick: line 47: stat: command not found
/usr/bin/wg-quick: line 47: stat: command not found
/usr/bin/wg-quick: line 47: ((: ( &  & 0007) == 0: syntax error: operand expected (error token is "&  & 0007) == 0")
Warning: `/etc/wireguard/wg1.conf' is world accessible
[#] ip link add wg1 type wireguard
[#] wg setconf wg1 /dev/fd/63
[#] ip -4 address add 123.123.123.123/24 dev wg1
ip: RTNETLINK answers: Network is unreachable
[#] ip link set mtu 1420 up dev wg1
[#] ip -4 route add 10.0.0.100/32 dev wg1
```

### 프로세스 (heartbeat / rkipc / dhcpcd / ntpd)
```
$ adb shell pidof heartbeat
541
$ adb shell 'ps -ef | grep -E "heartbeat|rkipc|dhcpcd|ntpd" | grep -v grep'
dhcpcd    223    1  0 07:29 ?        00:00:00 dhcpcd: [manager] [ip4] [ip6]
root      228  223  0 07:29 ?        00:00:00 dhcpcd: [privileged proxy]
dhcpcd    229  223  0 07:29 ?        00:00:00 dhcpcd: [network proxy]
dhcpcd    230  223  0 07:29 ?        00:00:00 dhcpcd: [control proxy]
root      269    1  0 07:29 ?        00:00:00 /usr/sbin/ntpd -g -p /var/run/ntpd.pid
dhcpcd    319  228  0 07:52 ?        00:00:00 dhcpcd: [BPF BOOTP] usb0
dhcpcd    346  228  0 07:52 ?        00:00:00 dhcpcd: [BPF ARP] usb0 169.254.172.245
root      538    1  0 07:29 ?        00:00:00 sh -c '/usr/bin/heartbeat' 2>&1 | logger -t 'heartbeat'
root      541  538  0 07:29 ?        00:00:04 /usr/bin/heartbeat
root      542  538  0 07:29 ?        00:00:00 logger -t heartbeat
root      676    1 10 07:29 ?        00:05:05 rkipc -a /oem/usr/share/iqfiles
dhcpcd    703  228  0 07:29 ?        00:00:00 dhcpcd: [BPF ARP] wlan0 192.168.0.9
```

### heartbeat 설치 파일
```
$ adb shell ls -la /usr/bin/heartbeat /etc/init.d/S60heartbeat
-rwxr-xr-x    1 root     root          1433 Nov  5 08:35 /etc/init.d/S60heartbeat
-rwxr-xr-x    1 root     root         13060 Nov  3 13:55 /usr/bin/heartbeat
$ adb shell md5sum /usr/bin/heartbeat /etc/init.d/S60heartbeat
892233420ebd883fa37a65fb628de124  /usr/bin/heartbeat
99c9bf9d26c5a53be1add53589db3a03  /etc/init.d/S60heartbeat
$ md5sum heartbeat/heartbeat heartbeat/S60heartbeat   (VM, 저장소)
892233420ebd883fa37a65fb628de124  heartbeat/heartbeat
3ceab478aec96f82d6bb18e7c6e47fdc  heartbeat/S60heartbeat
```

### 보드 S60heartbeat의 HEARTBEAT_URL
```
$ adb shell grep -n HEARTBEAT /etc/init.d/S60heartbeat
3:export HEARTBEAT_URL=http://10.0.0.100:3000/v1/devices/heartbeat
$ diff heartbeat/S60heartbeat <보드 /etc/init.d/S60heartbeat>
2a3
> export HEARTBEAT_URL=http://10.0.0.100:3000/v1/devices/heartbeat
```

### heartbeat 로그 (마지막 기록)
```
$ adb shell 'grep -h -i heartbeat /var/log/messages.0 /var/log/messages | tail -6'
Jan  1 12:00:06 luckfox user.notice heartbeat: heartbeat: url=http://10.0.0.100:3000/v1/devices/heartbeat iface=wg1 mac=<redacted> period=5s
Jan  1 12:00:06 luckfox user.notice heartbeat: heartbeat: status rtsp_ok=false wg_ok=true
Jan  1 12:00:08 luckfox user.notice heartbeat: heartbeat: transport error: Timeout was reached, retry next cycle
Jan  1 12:00:11 luckfox user.notice heartbeat: heartbeat: status rtsp_ok=true wg_ok=true
```

### /etc/init.d/
```
$ adb shell ls /etc/init.d/
S01seedrng               S40bluetoothd            S91smb
S01syslogd               S40network               S99_auto_reboot
S02klogd                 S41dhcpcd                S99hciinit
S02sysctl                S49ntp                   S99luckfoxconfigload
S10udev                  S50sshd                  S99luckfoxcustomoverlay
S20linkmount             S50telnet                S99python
S20urandom               S50usbdevice             S99rtcinit
S21appinit               S50wireguard             S99usb0config
S30dbus                  S60heartbeat             rcK
S35iptables              S60micinit               rcS
```

### rkipc factory ini 심링크
```
$ adb shell readlink -f /tmp/rkipc-factory-config.ini
/oem/usr/share/rkipc-800w.ini
$ adb shell ls -la /tmp/rkipc-factory-config.ini
lrwxrwxrwx    1 root     root            29 Jan  1  2021 /tmp/rkipc-factory-config.ini -> /oem/usr/share/rkipc-800w.ini
```

### rkipc [network.ntp] enable
```
$ adb shell 'grep -n -A3 "^\[network.ntp\]" /oem/usr/share/rkipc-800w.ini'
539:[network.ntp]
540-enable = 0
541-refresh_time_s = 60
542-ntp_server = 119.28.183.184
$ adb shell 'grep -n -A3 "^\[network.ntp\]" /userdata/rkipc.ini'
539:[network.ntp]
540-enable = 0
541-refresh_time_s = 60
542-ntp_server = 119.28.183.184
$ adb shell 'diff /oem/usr/share/rkipc-800w.ini /userdata/rkipc.ini && echo SAME'
SAME
```

### rkipc 영상 설정 (/userdata/rkipc.ini, 일부 키만 grep)
```
== [video.0]
enable_refer_buffer_share = 1
stream_type = mainStream
video_type = compositeStream
width = 1920
height = 1080
rc_mode = CBR
src_frame_rate_den = 1
src_frame_rate_num = 15
dst_frame_rate_den = 1
dst_frame_rate_num = 15
max_rate = 4096
output_data_type = H.265
gop = 50
gop_mode = normalP
enable_motion_deblur = 1
enable_motion_static_switch = 0
enable_debreath_effect = 0
qbias_enable = 1
== [video.1]
enable_refer_buffer_share = 1
stream_type = subStream
video_type = compositeStream
width = 2560
height = 1440
rc_mode = CBR
src_frame_rate_den = 1
src_frame_rate_num = 15
dst_frame_rate_den = 1
dst_frame_rate_num = 15
max_rate = 8192
output_data_type = H.265
gop = 50
gop_mode = normalP
enable_motion_deblur = 1
enable_motion_static_switch = 0
enable_debreath_effect = 0
== [video.source]
enable_aiq = 1
enable_vo = 0
enable_ivs = 1
enable_jpeg = 0
enable_venc_0 = 1
enable_venc_1 = 0
enable_venc_2 = 0
enable_npu = 0
enable_rtsp = 1
enable_rtmp = 0
```

### 열린 TCP 포트
```
$ adb shell netstat -tln
Active Internet connections (only servers)
Proto Recv-Q Send-Q Local Address           Foreign Address         State       
tcp        0      0 127.0.0.1:3893          0.0.0.0:*               LISTEN      
tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN      
tcp        0      0 0.0.0.0:445             0.0.0.0:*               LISTEN      
tcp        0      0 0.0.0.0:554             0.0.0.0:*               LISTEN      
tcp        0      0 0.0.0.0:139             0.0.0.0:*               LISTEN      
tcp        0      0 127.0.0.1:5037          0.0.0.0:*               LISTEN      
tcp        0      0 0.0.0.0:5555            0.0.0.0:*               LISTEN      
tcp        0      0 :::22                   :::*                    LISTEN      
tcp        0      0 :::23                   :::*                    LISTEN      
tcp        0      0 :::445                  :::*                    LISTEN      
tcp        0      0 :::139                  :::*                    LISTEN      
```

### 카메라 센서
```
$ adb shell 'dmesg | grep -i -E "imx415|sc3336"'
[ 2703.016315] imx415 4-0037: set analog gain 0x96
[ 2707.416143] imx415 4-0037: set analog gain 0x97
[ 2748.659401] imx415 4-0037: set analog gain 0x98
[ 2762.392491] imx415 4-0037: set analog gain 0x99
[ 2762.525668] imx415 4-0037: set analog gain 0x9a
... (같은 형식의 imx415 4-0037 gain/exposure 메시지 약 45줄 생략. sc3336 매칭은 0건.
     dmesg 링버퍼가 이미 넘쳐서 부팅 시 probe 메시지는 남아 있지 않음)
[ 3033.095677] imx415 4-0037: set analog gain 0x9e
$ adb shell ls -la /oem/usr/share/iqfiles/
total 596
drwxrwxr-x    2 1000     1000          4096 Aug 22 01:37 .
drwxrwxr-x    5 1000     1000          4096 Nov  4 10:27 ..
-rw-rw-r--    1 1000     1000        451241 Jul 12 03:21 imx415_CMK-OT2022-PX1_IR0147-36IRC-8M-F20.json
-rwxrwxr-x    1 1000     1000        143486 Jul 12 03:21 mia1321_MIA1321_30IRC-F16.json
```

### dhcpcd denyinterfaces
```
$ adb shell grep -n denyinterfaces /etc/dhcpcd.conf
49:denyinterfaces wg*
```

### 메모리
```
$ adb shell free
               total        used        free      shared  buff/cache   available
Mem:          149424       30024       62376        1088       57024      112144
Swap:              0           0           0
$ adb shell 'cat /proc/meminfo | head -5'
MemTotal:         149424 kB
MemFree:           62236 kB
MemAvailable:     112012 kB
Buffers:            9192 kB
Cached:            42312 kB
```

### 디스크
```
$ adb shell df -h
Filesystem                Size      Used Available Use% Mounted on
/dev/root                 5.8G    326.3M      5.3G   6% /
devtmpfs                 72.8M         0     72.8M   0% /dev
tmpfs                    73.0M         0     73.0M   0% /dev/shm
tmpfs                    73.0M    488.0K     72.5M   1% /tmp
tmpfs                    73.0M    600.0K     72.4M   1% /run
/dev/block/by-name/userdata
                        237.9M    126.0K    228.5M   0% /userdata
/dev/block/by-name/oem
                        487.6M     19.0M    452.7M   4% /oem
```
