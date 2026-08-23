#!/bin/sh
set -eu

mkdir -p tests/bin
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/hls.c \
  source/miniiptv/hls_prefetch.c \
  tests/test_hls_prefetch.c \
  -o tests/bin/test_hls_prefetch_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_hls_prefetch_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/h264_annexb.c \
  tests/test_h264_annexb.c \
  -o tests/bin/test_h264_annexb_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_h264_annexb_san
