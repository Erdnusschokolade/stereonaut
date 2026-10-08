#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 INPUT.mkv OUTPUT_DIRECTORY DURATION_SECONDS" >&2
    exit 2
fi

input=$1
output_directory=$2
duration_seconds=$3
elementary_stream="$output_directory/realvideo-001.hevc"
reference="$output_directory/selected-reference.i420"
manifest="$output_directory/manifest.txt"
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

[[ -f "$input" ]] || { echo "input is not a regular file: $input" >&2; exit 2; }
[[ "$duration_seconds" =~ ^[1-9][0-9]*$ ]] || {
    echo "duration must be a positive integer" >&2
    exit 2
}
mkdir -p "$output_directory"

codec=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name -of default=nw=1:nk=1 "$input")
profile=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 "$input")
pixel_format=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=pix_fmt -of default=nw=1:nk=1 "$input")
if [[ "$codec" != hevc || "$profile" != Main || "$pixel_format" != yuv420p ]]; then
    echo "input must be HEVC Main 8-bit yuv420p; got $codec/$profile/$pixel_format" >&2
    exit 2
fi

# Stream copy only: convert container framing to Annex B and insert AUD NALs.
ffmpeg -nostdin -hide_banner -loglevel warning -y -i "$input" \
    -map 0:v:0 -t "$duration_seconds" -an -sn -dn -c:v copy \
    -bsf:v 'hevc_mp4toannexb,hevc_metadata=aud=insert' \
    -f hevc "$elementary_stream"

width=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width -of default=nw=1:nk=1 "$elementary_stream")
height=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=height -of default=nw=1:nk=1 "$elementary_stream")
rate=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=r_frame_rate -of default=nw=1:nk=1 "$elementary_stream")
inspection="$output_directory/annexb-inspection.json"
"$script_directory/inspect-annexb.py" "$elementary_stream" > "$inspection"
frame_count=$(python3 -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["access_units"])' \
    "$inspection")
[[ "$frame_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "could not determine elementary-stream frame count" >&2
    exit 2
}
middle_frame=$((frame_count / 2))
last_frame=$((frame_count - 1))

# The source container is indexed. Accurate input seeking decodes from the
# preceding keyframe and avoids decoding the complete 8K excerpt three times.
rate_numerator=${rate%/*}
rate_denominator=${rate#*/}
: > "$reference"
for selected_frame in 0 "$middle_frame" "$last_frame"; do
    timestamp=$(awk -v n="$selected_frame" -v num="$rate_numerator" \
        -v den="$rate_denominator" 'BEGIN { printf "%.9f", n * den / num }')
    selected_path=$(printf '%s/reference-frame-%08d.i420' \
        "$output_directory" "$selected_frame")
    ffmpeg -nostdin -hide_banner -loglevel warning -y -ss "$timestamp" \
        -i "$input" -map 0:v:0 -frames:v 1 -pix_fmt yuv420p \
        -f rawvideo "$selected_path"
    cat "$selected_path" >> "$reference"
done

stream_sha256=$(sha256sum "$elementary_stream" | awk '{print $1}')
reference_sha256=$(sha256sum "$reference" | awk '{print $1}')
stream_bytes=$(stat -c %s "$elementary_stream")
reference_bytes=$(stat -c %s "$reference")
{
    printf 'source=%s\n' "$input"
    printf 'preparation=stream-copy+hevc_mp4toannexb+AUD-insertion\n'
    printf 'codec=hevc\nprofile=Main\npixel_format=yuv420p\n'
    printf 'width=%s\nvisible_height=%s\nframe_rate=%s\n' \
        "$width" "$height" "$rate"
    printf 'frames=%s\nselected_frames=0,%s,%s\n' \
        "$frame_count" "$middle_frame" "$last_frame"
    printf 'elementary_stream_bytes=%s\n' "$stream_bytes"
    printf 'elementary_stream_sha256=%s\n' "$stream_sha256"
    printf 'reference_bytes=%s\nreference_sha256=%s\n' \
        "$reference_bytes" "$reference_sha256"
} > "$manifest"

cat "$manifest"
