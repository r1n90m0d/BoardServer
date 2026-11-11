#ifndef STREAMER_OSD_H
#define STREAMER_OSD_H

#include "config.h"

/*
 * 날짜·시각 OSD ("YYYY-MM-DD HH:MM:SS", localtime 기준이라 /etc/TZ 를 따른다).
 * 채널마다 RGN overlay 를 하나씩 만들어 VENC 에 붙이고 1초마다 비트맵을 바꾼다.
 * video_start 뒤에 osd_start, video_stop 앞에 osd_stop 을 불러야 한다.
 */
int osd_start(const struct config *cfg);
void osd_stop(void);

#endif
