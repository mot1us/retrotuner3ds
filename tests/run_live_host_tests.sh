#!/bin/sh
set -eu

mkdir -p tests/bin
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/playlist.c \
  tests/test_playlist.c \
  -o tests/bin/test_playlist_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_playlist_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/hls.c \
  tests/test_hls_parser.c \
  -o tests/bin/test_hls_parser_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_hls_parser_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/hls.c \
  source/miniiptv/hls_prefetch.c \
  tests/test_hls_prefetch.c \
  -o tests/bin/test_hls_prefetch_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_hls_prefetch_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/h264_annexb.c \
  tests/test_h264_annexb.c \
  -o tests/bin/test_h264_annexb_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_h264_annexb_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/network.c \
  tests/test_network.c \
  -lcurl \
  -o tests/bin/test_network_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_network_san

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iinclude \
  source/miniiptv/buffer_shadow.c \
  tests/test_buffer_shadow.c \
  -o tests/bin/test_buffer_shadow_san

ASAN_OPTIONS=detect_leaks=0 tests/bin/test_buffer_shadow_san
