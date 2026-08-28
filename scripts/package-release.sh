#!/bin/sh
set -eu

release_version=${1-}
case "$release_version" in
    ''|*[!A-Za-z0-9._-]*)
        printf '%s\n' "Usage: $0 VERSION" >&2
        exit 2
        ;;
esac

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

for release_tool in git tar make zip unzip; do
    if ! command -v "$release_tool" >/dev/null 2>&1; then
        printf '%s\n' "Required release tool not found: $release_tool" >&2
        exit 1
    fi
done
if ! command -v sha256sum >/dev/null 2>&1 &&
   ! command -v shasum >/dev/null 2>&1; then
    printf '%s\n' "Required SHA-256 utility not found (sha256sum or shasum)." >&2
    exit 1
fi
if [ -z "${DEVKITARM-}" ] || [ -z "${DEVKITPRO-}" ]; then
    printf '%s\n' "DEVKITARM and DEVKITPRO must be configured." >&2
    exit 1
fi

if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    printf '%s\n' "Refusing release build: commit or remove working-tree changes first." >&2
    exit 1
fi

"$project_root/scripts/check-public-tree.sh"

release_commit=$(git rev-parse HEAD)
embedded_version=$(sed -n 's/^#define RETROTUNER_VERSION "\([^"]*\)"$/\1/p' \
    include/miniiptv/version.h)
if [ "$release_version" != "$embedded_version" ]; then
    printf '%s\n' \
        "Release version '$release_version' does not match embedded version '$embedded_version'." >&2
    exit 1
fi

release_tag="v$release_version"
release_tag_commit=
release_tag_type=
if release_tag_commit=$(git rev-parse -q --verify "refs/tags/$release_tag^{commit}" 2>/dev/null); then
    release_tag_type=$(git cat-file -t "refs/tags/$release_tag")
    if [ "$release_tag_commit" != "$release_commit" ]; then
        printf '%s\n' \
            "Tag '$release_tag' points at a different commit; refusing an ambiguous package." >&2
        exit 1
    fi
fi

require_tag=${RETROTUNER_REQUIRE_TAG:-0}
third_party_source_archive=${RETROTUNER_THIRD_PARTY_SOURCE_ARCHIVE:-}
third_party_source_name=
case "$require_tag" in
    0) ;;
    1)
        if [ -z "$release_tag_commit" ] || [ "$release_tag_type" != tag ]; then
            printf '%s\n' \
                "Public releases require an annotated '$release_tag' tag at HEAD." >&2
            exit 1
        fi
        if [ -z "$third_party_source_archive" ] ||
           [ ! -f "$third_party_source_archive" ]; then
            printf '%s\n' \
                "Public releases require RETROTUNER_THIRD_PARTY_SOURCE_ARCHIVE." \
                "Provide an audited archive containing the corresponding source for bundled third-party libraries." >&2
            exit 1
        fi
        ;;
    *)
        printf '%s\n' "RETROTUNER_REQUIRE_TAG must be 0 or 1." >&2
        exit 2
        ;;
esac
if [ -n "$third_party_source_archive" ]; then
    third_party_source_name=$(basename -- "$third_party_source_archive")
    case "$third_party_source_name" in
        ''|*[!A-Za-z0-9._-]*)
            printf '%s\n' "Third-party source archive has an unsafe filename." >&2
            exit 1
            ;;
    esac
fi

source_base=${RETROTUNER_SOURCE_URL_BASE:-https://github.com/mot1us/retrotuner3ds}
source_base=${source_base%/}
source_url="$source_base/tree/$release_commit"
if [ -n "$release_tag_commit" ]; then
    package_provenance="Tagged source: $release_tag"
else
    package_provenance="Untagged local test package"
fi
release_name="RetroTuner3DS-$release_version"
source_archive_name="$release_name-project-source.zip"

release_tmp_dir=$(mktemp -d "/tmp/retrotuner3ds-release.XXXXXX")
case "$release_tmp_dir" in
    /tmp/retrotuner3ds-release.*) ;;
    *) printf '%s\n' "Unexpected temporary path: $release_tmp_dir" >&2; exit 1 ;;
esac

cleanup_release_tmp() {
    case "$release_tmp_dir" in
        /tmp/retrotuner3ds-release.*) rm -rf -- "$release_tmp_dir" ;;
    esac
}
trap cleanup_release_tmp EXIT HUP INT TERM

release_source="$release_tmp_dir/source"
mkdir -p "$release_source"
git archive --format=tar "$release_commit" | tar -xf - -C "$release_source"
git archive --format=zip --prefix="$release_name-project-source/" \
    -o "$release_tmp_dir/$source_archive_name" "$release_commit"

(
    cd "$release_source"
    ./tests/run_live_host_tests.sh
    make 3dsx -j"${RETROTUNER_BUILD_JOBS:-4}"
)

release_dist="$project_root/dist"
release_stage="$release_tmp_dir/$release_name"
release_zip="$release_dist/$release_name.zip"
release_zip_tmp="$release_tmp_dir/$release_name.zip"

if [ -e "$release_dist/$release_name" ] || [ -e "$release_zip" ] ||
   [ -e "$release_zip.sha256" ]; then
    printf '%s\n' "Refusing to overwrite an existing release: $release_name" >&2
    exit 1
fi

mkdir -p "$release_stage/3ds/retrotuner3ds"
cp "$release_source/retrotuner3ds.3dsx" \
   "$release_stage/3ds/retrotuner3ds/retrotuner3ds.3dsx"
cp "$release_source/LICENSE" "$release_stage/LICENSE"
cp "$release_source/THIRD_PARTY_NOTICES.md" "$release_stage/THIRD_PARTY_NOTICES.md"
cp -R "$release_source/LICENSES" "$release_stage/LICENSES"
cp "$release_tmp_dir/$source_archive_name" "$release_stage/$source_archive_name"
if [ -n "$third_party_source_name" ]; then
    cp "$third_party_source_archive" \
       "$release_stage/$third_party_source_name"
    third_party_source_notice="Third-party source archive: $third_party_source_name"
else
    third_party_source_notice="Third-party source is not bundled in this local test package."
fi

printf '%s\n' \
    "RetroTuner3DS $release_version" \
    "" \
    "Copy the included 3ds folder to the root of your SD card." \
    "Then add your own playlist at:" \
    "  sd:/3ds/retrotuner3ds/channels.m3u" \
    "" \
    "The latest hardware telemetry is written to:" \
    "  sd:/3ds/retrotuner3ds/telemetry.csv" \
    "" \
    "No playlist or channel URLs are included." \
    "Tested on a New Nintendo 3DS. Other models are untested." \
    "" \
    "$package_provenance" \
    "RetroTuner3DS project source snapshot: $source_archive_name" \
    "$third_party_source_notice" \
    "Project source mirror: $source_url" \
    > "$release_stage/README.txt"
printf '%s\n' "$release_commit" > "$release_stage/SOURCE_COMMIT.txt"
printf '%s\n' "$source_url" > "$release_stage/SOURCE_URL.txt"

packaged_playlists=$(find "$release_stage" -type f \( -name '*.m3u' -o -name '*.m3u8' \) -print)
if [ -n "$packaged_playlists" ]; then
    printf '%s\n' "Refusing release: a playlist entered the package:" >&2
    printf '%s\n' "$packaged_playlists" >&2
    exit 1
fi

write_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$@"
    else
        printf '%s\n' "No SHA-256 utility found." >&2
        return 1
    fi
}

(
    cd "$release_stage"
    write_sha256 3ds/retrotuner3ds/retrotuner3ds.3dsx \
        "$source_archive_name" > SHA256SUMS.txt
    if [ -n "$third_party_source_name" ]; then
        write_sha256 "$third_party_source_name" >> SHA256SUMS.txt
    fi
)
(
    cd "$release_tmp_dir"
    zip -X -qr "$release_name.zip" "$release_name" \
        -x '*/.DS_Store' '*/__MACOSX/*'
    unzip -tq "$release_name.zip"
    write_sha256 "$release_name.zip" > "$release_name.zip.sha256"
)

mkdir -p "$release_dist"
mv "$release_stage" "$release_dist/$release_name"
mv "$release_zip_tmp" "$release_zip"
mv "$release_zip_tmp.sha256" "$release_zip.sha256"

printf '%s\n' "Release package created:" "$release_zip" "$release_zip.sha256"
