CC ?= cc
CPPFLAGS ?= -Iplugin
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic

BUILD_DIR := build
TEST_BIN := $(BUILD_DIR)/test_mp4_probe
H264_TEST_BIN := $(BUILD_DIR)/test_h264_first
PROBE_BIN := $(BUILD_DIR)/nxprobe
H264BSD_SRC := $(wildcard plugin/h264bsd/*.c)

.PHONY: all test clean

all: test $(PROBE_BIN)

test: $(TEST_BIN) $(H264_TEST_BIN)
	$(TEST_BIN)
	$(H264_TEST_BIN)

$(TEST_BIN): tests/test_mp4_probe.c plugin/nx_mp4.c plugin/nx_mp4.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_mp4_probe.c plugin/nx_mp4.c -o $@

$(H264_TEST_BIN): tests/test_h264_first.c plugin/nx_h264.c plugin/nx_h264.h \
		plugin/nx_mp4.c plugin/nx_mp4.h $(H264BSD_SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) -Iplugin -Iplugin/h264bsd -std=c11 -O2 -w \
		tests/test_h264_first.c plugin/nx_h264.c plugin/nx_mp4.c \
		$(H264BSD_SRC) -o $@

$(PROBE_BIN): tools/nxprobe.c plugin/nx_mp4.c plugin/nx_mp4.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/nxprobe.c plugin/nx_mp4.c -o $@

clean:
	rm -f $(TEST_BIN) $(H264_TEST_BIN) $(PROBE_BIN)
