CC = /home/taeseok/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
SYSROOT = /home/taeseok/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot

CFLAGS = --sysroot=$(SYSROOT) -Wall -Wextra -O2
LDFLAGS = --sysroot=$(SYSROOT)
LDLIBS = -lcurl

TARGET = heartbeat
SRC = heartbeat.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
