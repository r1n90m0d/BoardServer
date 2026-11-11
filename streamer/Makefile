# streamer 빌드 (VM에서 실행). SDK를 한 번 빌드해 둔 상태여야 한다.
#   make            -> build/streamer
#   make clean
SDK    ?= $(HOME)/luckfox-pico
CROSS  ?= $(SDK)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-
MEDIA  ?= $(SDK)/output/out/media_out

CC     := $(CROSS)gcc
STRIP  := $(CROSS)strip
BUILD  := build
TARGET := $(BUILD)/streamer

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# SDK 헤더는 -isystem 으로 넣어 우리 코드의 경고만 오류로 처리한다.
INCS := -isystem $(MEDIA)/include \
        -isystem $(MEDIA)/include/rkaiq \
        -isystem $(MEDIA)/include/rkaiq/uAPI2 \
        -isystem $(MEDIA)/include/rkaiq/common \
        -isystem $(MEDIA)/include/rkaiq/algos \
        -isystem $(MEDIA)/include/rkaiq/xcore \
        -isystem $(MEDIA)/include/rkaiq/iq_parser \
        -isystem $(MEDIA)/include/rkaiq/iq_parser_v2

CFLAGS  := -O2 -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -MMD -MP $(INCS)
LDFLAGS := -L$(MEDIA)/lib -Wl,-rpath-link,$(MEDIA)/lib
# librtsp 만 정적 링크, 나머지는 보드의 /oem/usr/lib 것을 쓴다.
LIBS    := -Wl,-Bstatic -lrtsp -Wl,-Bdynamic -lrockit -lrockchip_mpp -lrkaiq -lpthread -lm

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD   $@"
	@$(CC) $(LDFLAGS) -o $@.unstripped $(OBJS) $(LIBS)
	@$(STRIP) -o $@ $@.unstripped

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

.PHONY: all clean
