#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <ffmpeg-source-dir> <install-prefix>" >&2
  exit 2
fi

to_posix_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s\n' "$1"
  fi
}

source_dir="$(to_posix_path "$1")"
install_prefix="$(to_posix_path "$2")"

if [[ ! -x "$source_dir/configure" ]]; then
  echo "FFmpeg configure script not found: $source_dir/configure" >&2
  exit 2
fi

mkdir -p "$install_prefix"
cd "$source_dir"

if [[ "$(uname -s)" == Linux* ]]; then
  for tool in cc c++ ar make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "required build tool is unavailable: $tool" >&2
      exit 2
    fi
  done

  # Linux releases use static FFmpeg archives built without autodetected
  # external codecs or protocols. This keeps the release independent of the
  # distribution's libavformat/libavcodec package names and ABI versions.
  ./configure \
    --prefix="$install_prefix" \
    --arch=x86_64 \
    --target-os=linux \
    --enable-static \
    --disable-shared \
    --disable-everything \
    --disable-autodetect \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-swresample \
    --disable-swscale \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-network \
    --disable-iconv \
    --disable-x86asm \
    --enable-pic \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-demuxer=flv \
    --enable-muxer=mp4 \
    --enable-parser=h264 \
    --enable-parser=aac \
    --enable-protocol=file \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=aac_adtstoasc
else
  for tool in cl.exe lib.exe make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "required build tool is unavailable: $tool" >&2
      exit 2
    fi
  done

  ./configure \
    --prefix="$install_prefix" \
    --toolchain=msvc \
    --arch=x86_64 \
    --target-os=win32 \
    --enable-static \
    --disable-shared \
    --disable-everything \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-network \
    --disable-x86asm \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-demuxer=flv \
    --enable-muxer=mp4 \
    --enable-parser=h264 \
    --enable-parser=aac \
    --enable-protocol=file \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=aac_adtstoasc
fi

make -j"${NUMBER_OF_PROCESSORS:-2}"
make install
