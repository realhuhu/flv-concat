# FLVConcat

[中文说明](README.md) · [Latest release](https://github.com/realhuhu/flv-concat/releases/latest) · [Report an issue](https://github.com/realhuhu/flv-concat/issues)

FLVConcat repairs live-recording FLV files and losslessly merges them into MP4. It reads raw FLV tag timestamps to handle backward jumps, 32-bit wraps, resent audio packets, duplicated publishing runs, and shared audio/video stalls.

No media is decoded or re-encoded. Processing speed is primarily limited by disk I/O.

## Highlights

- Repairs backward timestamp jumps and 32-bit wraps.
- Pairs audio and video runs on their shared recording clock.
- Removes non-increasing AAC packets resent inside a run.
- Detects duplicated publishing runs using both timestamps and encoded-content fingerprints.
- Preserves real gaps caused by shared audio/video stalls, preventing cumulative A/V drift.
- Preserves H.264/H.265 composition time and supports an additional presentation offset.
- Sorts multiple inputs by filename, with an option to keep command-line order.
- Expands `*.flv` and `part-?.flv` input filename patterns when the shell passes them through.
- Writes to a temporary output and only moves it into place after successful finalization.
- Supports drag-and-drop on Windows.

## Download

Download the Windows x64 or Linux x64 archive from [GitHub Releases](https://github.com/realhuhu/flv-concat/releases/latest). The Windows executable is statically linked and does not require a separate FFmpeg installation; the Linux build uses the distribution's FFmpeg shared libraries.

Official releases build a minimal FFmpeg 7.1.1 from a SHA-256-verified source archive, enabling only the FLV, MP4, H.264/H.265/AAC, and local-file components FLVConcat needs. The release workflow rejects executables larger than 4 MiB to catch accidental full-FFmpeg linkage.

Supported inputs are FLV files containing H.264/AVC or H.265/HEVC video and AAC audio. This includes legacy FLV `codec_id=12` HEVC recordings. Files in one merge must have identical resolution, codecs, sample rate, and channel count. H.264 compatibility compares NAL length, SPS, and PPS. H.265 compatibility compares NAL length, SPS profile/chroma/bit depth/dimensions, and parameter-set IDs; encoders may regenerate VPS/SPS/PPS reference-picture, VUI, and default-slice fields between segments, and FLVConcat injects the new parameter sets before every keyframe without re-encoding, keeping both sequential playback and random seeking decodable. Changes that cannot be made safe this way are rejected during initialization. AAC compatibility compares object type, sample rate, channel layout, and frame length. Differences only in optional avcC/hvcC/ASC extension bytes do not block a merge.

## Usage

~~~text
flvconcat [options] <input.flv|pattern> [more.flv ...]
~~~

Examples:

~~~powershell
flvconcat.exe "01.flv" "02.flv"
flvconcat.exe "recordings\*.flv"
flvconcat.exe -o "recording.mp4" "01.flv" "02.flv"
flvconcat.exe --keep-order --overwrite -o "recording.mp4" "part-b.flv" "part-a.flv"
~~~

Run \`flvconcat --help\` for every option. A plain integer in \`avoffset.txt\`, next to the executable or in the current directory, remains supported for compatibility; \`--av-offset\` takes precedence.

## How it works

FLVConcat performs two passes. The first pass indexes raw FLV media tags, unwraps timestamps, divides streams into runs, and computes lightweight packet fingerprints. It then pairs audio/video runs, removes verified duplicated runs, and plans a continuous output timeline. The second pass reads encoded H.264/H.265/AAC payloads by index, drops resent audio, preserves composition offsets and real recording gaps, and writes MP4 through libavformat. Sequence-header probing is codec-specific and lives under `src/codecs/`, so future formats can be added without changing the timeline pipeline.

See [docs/algorithm.md](docs/algorithm.md) for the detailed model.

## Building

You need CMake 3.20+, a C++17 compiler, and FFmpeg development libraries for libavformat, libavcodec, and libavutil. The repository includes a vcpkg manifest for convenient Windows development. Official compact binaries use the exact configuration in [scripts/build-minimal-ffmpeg.sh](scripts/build-minimal-ffmpeg.sh), shared by Windows CI and the release workflow. See the Chinese README above for ready-to-run commands.

## License

FLVConcat is licensed under [GPL-3.0-only](LICENSE). Official binaries statically link FFmpeg; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
