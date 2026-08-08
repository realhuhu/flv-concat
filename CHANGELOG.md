# Changelog

All notable changes follow [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and semantic versioning.

## [1.1.0] - 2026-08-08

### Fixed

- Accept H.265 segments whose encoder regenerated VPS/SPS/PPS reference-picture,
  VUI, or default-slice fields while keeping the same NAL length, decoded format,
  and parameter-set IDs, matching the original repair tool's merge behavior.
- Add a regression test for the supplied 20260808 来聂卫平杯棋牌大师赛 FLVs.

## [1.0.3] - 2026-08-08

### Added

- Add H.265/HEVC support for legacy FLV `codec_id=12` recordings.
- Parse H.264 and H.265 sequence headers directly, including SPS dimensions and
  semantic VPS/SPS/PPS compatibility checks.
- Split codec-specific parsing into `src/codecs/` so future formats can be
  registered without changing the timestamp and muxing pipeline.

### Fixed

- Avoid FFmpeg's unsupported-codec path for old HEVC FLV files by probing hvcC
  and AAC sequence headers directly from FLV tags.

## [1.0.2] - 2026-08-08

### Fixed

- Compare H.264 configuration semantically through NAL length, SPS, and PPS instead of hashing the whole AVC configuration record.
- Compare AAC AudioSpecificConfig semantics and ignore optional metadata-only extensions.
- Accept compatible FLV recordings that differ only in malformed or optional avcC/ASC extension bytes, while still rejecting real decoder-configuration changes.

## [1.0.1] - 2026-08-04

### Fixed

- Build official Windows releases against a reproducible, SHA-256-verified minimal FFmpeg instead of the general-purpose vcpkg FFmpeg port.
- Reject release executables larger than 4 MiB to prevent accidental package-size regressions.

## [1.0.0] - 2026-08-04

### Added

- Raw two-pass FLV scanner for H.264/AAC recordings.
- Timestamp reset and 32-bit wrap handling.
- Audio/video run pairing on a shared clock.
- Resent AAC packet filtering.
- Timestamp-and-content duplicate run detection.
- Lossless MP4 muxing with composition-time preservation.
- Multi-file CLI, drag-and-drop support, atomic output, and optional A/V offset.
- Unit tests, cross-platform CI, CodeQL, and tag-driven Windows releases.
