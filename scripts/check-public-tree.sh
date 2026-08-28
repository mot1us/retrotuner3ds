#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

for forbidden_pattern in \
    '*.m3u' '*.m3u8' '*.3dsx' '*.cia' '*.elf' '*.map' '*.smdh' '*.zip' \
    ':(glob)**/telemetry.csv' ':(glob)**/telemetry-prev.csv' \
    ':(glob)**/crash_dump_*.dmp'; do
    tracked_files=$(git ls-files -- "$forbidden_pattern")
    if [ -n "$tracked_files" ]; then
        printf '%s\n' "Refusing public build: generated/private files are tracked:" >&2
        printf '%s\n' "$tracked_files" >&2
        exit 1
    fi
done

tracked_romfs_media=$(git ls-files -- romfs | awk '
    tolower($0) ~ /\.(mkv|mp4|m4v|mov|avi|webm|ts|m2ts|mpeg|mpg)$/ { print }
')
if [ -n "$tracked_romfs_media" ]; then
    printf '%s\n' "Refusing public build: media files under romfs would be embedded:" >&2
    printf '%s\n' "$tracked_romfs_media" >&2
    exit 1
fi

printf '%s\n' "Public-tree hygiene check passed"
