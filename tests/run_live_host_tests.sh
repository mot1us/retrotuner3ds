#!/bin/sh
set -eu

host_cc=${CC:-cc}
host_c_standard=${RETROTUNER_HOST_C_STANDARD:-c99}
test_bin_dir=$(mktemp -d "${TMPDIR:-/tmp}/retrotuner-host-tests.XXXXXX")
# Test executables stay isolated in the temporary directory. The telemetry
# fixture still writes short-lived CSV files beneath this ignored path.
mkdir -p tests/bin

cleanup() {
  rm -rf -- "$test_bin_dir"
}
trap cleanup EXIT HUP INT TERM

# LeakSanitizer is reliable on the Linux CI runner. Apple's sanitizer runtime
# does not consistently support it, so macOS keeps the previous behavior unless
# the caller supplies ASAN_OPTIONS explicitly.
if [ -z "${ASAN_OPTIONS+x}" ]; then
  case "$(uname -s)" in
    Linux) ASAN_OPTIONS=detect_leaks=1 ;;
    *) ASAN_OPTIONS=detect_leaks=0 ;;
  esac
fi
export ASAN_OPTIONS

compile_and_run() {
  test_name=$1
  shift
  "$host_cc" -std="$host_c_standard" -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer "$@" -Iinclude -o "$test_bin_dir/$test_name"
  "$test_bin_dir/$test_name"
}

compile_and_run test_playlist \
  source/miniiptv/playlist.c tests/test_playlist.c

compile_and_run test_hls_parser \
  source/miniiptv/hls.c tests/test_hls_parser.c

compile_and_run test_channel_scan \
  source/miniiptv/hls.c source/miniiptv/channel_scan.c \
  tests/test_channel_scan.c

compile_and_run test_ts_mux \
  source/miniiptv/ts_mux.c tests/test_ts_mux.c

compile_and_run test_h264_annexb \
  source/miniiptv/h264_annexb.c tests/test_h264_annexb.c

compile_and_run test_network \
  source/miniiptv/network.c tests/test_network.c -lcurl

compile_and_run test_buffer_shadow \
  source/miniiptv/buffer_shadow.c tests/test_buffer_shadow.c

compile_and_run test_telemetry_log \
  source/miniiptv/telemetry_log.c tests/test_telemetry_log.c

compile_and_run test_speaker -Itests/speaker_stubs \
  source/system/util/speaker.c tests/test_speaker.c

compile_and_run test_theme \
  source/miniiptv/theme.c tests/test_theme.c -lm

compile_and_run test_theme_ui -Itests/theme_stubs \
  source/miniiptv/theme.c source/miniiptv/theme_store.c \
  source/miniiptv/theme_ui.c tests/test_theme_ui.c

# Includes the real storage source under deterministic stdio fault wrappers.
compile_and_run test_theme_store \
  source/miniiptv/theme.c tests/test_theme_store.c
