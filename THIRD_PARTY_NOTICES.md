# Third-party notices

## FFmpeg

FLVConcat uses FFmpeg libraries for stream probing and MP4 muxing.

- Project: https://ffmpeg.org/
- Source: https://github.com/FFmpeg/FFmpeg
- License: LGPL-2.1-or-later by default; the exact effective license depends on build configuration.

Official FLVConcat release binaries statically link FFmpeg 7.1.1 built from the unmodified official source archive at https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.gz (SHA-256: \`9a6e57a446b671012612aaeb9df5126794d5ac8f2015ca220934f99a6a4e0601\`). The release build disables all components by default and enables only the FLV demuxer, MP4 muxer, H.264/AAC parsers, local file protocol, and required bitstream filters. GPL, nonfree, network, programs, and external codec features are not enabled. FLVConcat itself is GPL-3.0-only, which permits redistribution of the combined executable under GPL-compatible terms.

The exact FFmpeg configure flags are recorded in \`scripts/build-minimal-ffmpeg.sh\`, and the reproducible download, checksum verification, build, and packaging workflow is contained in this repository. The vcpkg manifest remains available as a convenient general-purpose development setup, but it is not used for official release binaries.
