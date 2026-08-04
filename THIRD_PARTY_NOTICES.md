# Third-party notices

## FFmpeg

FLVConcat uses FFmpeg libraries for stream probing and MP4 muxing.

- Project: https://ffmpeg.org/
- Source: https://github.com/FFmpeg/FFmpeg
- License: LGPL-2.1-or-later by default; the exact effective license depends on build configuration.

Official FLVConcat release binaries build FFmpeg through the repository's vcpkg manifest with only the \`avcodec\` and \`avformat\` features and without GPL or nonfree external codec features. FLVConcat itself is GPL-3.0-only, which permits redistribution of the combined executable under GPL-compatible terms.

The corresponding FLVConcat source, build workflow, pinned vcpkg baseline, and FFmpeg port configuration are contained in this repository. FFmpeg source for the pinned port can be obtained by following vcpkg's port metadata.
