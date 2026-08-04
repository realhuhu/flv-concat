# FLVConcat

[English](README_EN.md) · [最新版本](https://github.com/realhuhu/flv-concat/releases/latest) · [问题反馈](https://github.com/realhuhu/flv-concat/issues)

FLVConcat 是面向直播、监控和录播文件的 FLV 修复合并工具。它直接读取 FLV tag 的原始时间戳，自动处理时间戳回退、32 位回绕、音频重发、整段重复推送和音画共同卡顿，再无损重封装为 MP4。

处理过程不解码、不转码，视频和音频数据原样写入 MP4，因此速度主要取决于磁盘读写。

## 主要能力

- 修复 FLV 时间戳回退与 32 位回绕。
- 将同一次录制中的音视频 run 按共享时钟重新配对。
- 删除 run 内时间戳不递增的 AAC 重发包。
- 通过“时间戳 + 编码内容指纹”识别整段重复推送，避免误删有效内容。
- 保留音画共同卡顿产生的真实空隙，避免后半段音频逐渐提前。
- 保留 H.264 composition time（PTS - DTS）并支持额外音画偏移。
- 多个 FLV 按文件名排序后快速合并，也可以保留命令行顺序。
- 输出使用临时文件；全部完成后才替换目标，失败不会留下半成品。
- Windows 支持把一个或多个 FLV 直接拖到 flvconcat.exe 上。

## 下载

从 [GitHub Releases](https://github.com/realhuhu/flv-concat/releases/latest) 下载 Windows x64 压缩包，解压后直接使用。官方包静态链接所需运行库，不需要另外安装 FFmpeg。

当前支持输入：

- FLV 容器
- H.264/AVC 视频
- AAC 音频
- 多文件必须具有相同的分辨率、编码、采样率、声道数和编解码配置

其他编码组合会明确拒绝，不会尝试有损转码。

## 使用方法

最简单的方式是在资源管理器中选中一个或多个 FLV，拖到 flvconcat.exe 上。单文件默认输出 \`原文件名_fixed.mp4\`，多文件默认输出 \`首文件名_merged.mp4\`。

也可以在 PowerShell 中运行：

~~~powershell
flvconcat.exe "01.flv" "02.flv"
flvconcat.exe -o "recording.mp4" "01.flv" "02.flv"
flvconcat.exe --keep-order --overwrite -o "recording.mp4" "part-b.flv" "part-a.flv"
~~~

常用参数：

| 参数 | 说明 |
| --- | --- |
| \`-o, --output <file>\` | 指定输出 MP4 |
| \`--av-offset <ms>\` | 给视频 PTS 增加偏移；正值表示画面更晚 |
| \`--run-gap <ms>\` | 判定新 run 的时间戳回退阈值，默认 2000 |
| \`--duplicate-threshold <0..1>\` | 重复 run 内容匹配比例，默认 0.90 |
| \`--keep-order\` | 保留命令行输入顺序，默认按文件名排序 |
| \`-f, --overwrite\` | 成功完成后替换已有输出 |
| \`--no-faststart\` | 不把 MP4 元数据移动到文件开头 |

为了兼容原始工具，也可以在 EXE 同目录或当前目录创建 \`avoffset.txt\`，写入一个毫秒整数。命令行 \`--av-offset\` 的优先级更高。

## 它解决的不是普通 concat

常规 FFmpeg concat 假定每段媒体时间戳基本正确。录播 FLV 常见的问题是设备或推流端在同一文件中重新从旧时间戳开始推送，甚至重新发送之前的一整段内容。直接拼接可能产生 non-monotonous DTS、音画逐渐错位或重复画面。

FLVConcat 使用两遍处理：

1. 第一遍直接扫描 FLV 字节，建立包索引，解开时间戳回绕，划分音频/视频 run，并计算轻量内容指纹。
2. 按时间范围配对音视频 run，识别重复 run，规划连续输出时间轴。
3. 第二遍按索引读取 H.264/AAC 包，过滤音频重发，保留原始 composition time 和真实空隙。
4. 通过 libavformat 写入 MP4，全程不重新编码。

详细设计见 [docs/algorithm.md](docs/algorithm.md)。

## 从源码构建

需要 CMake 3.20+、支持 C++17 的编译器，以及 FFmpeg 的 \`libavformat\`、\`libavcodec\`、\`libavutil\` 开发库。

Windows 推荐使用 vcpkg：

~~~powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
~~~

Ubuntu/Debian：

~~~bash
sudo apt-get install cmake g++ libavformat-dev libavcodec-dev libavutil-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
~~~

## 限制与安全

- 这是容器修复和无损重封装工具，不能恢复录制时已经丢失的音视频数据。
- 共同卡顿会表现为音频静默和最后一帧保持，这是为了维持卡顿后的同步关系。
- 请先保留原始 FLV。除非显式使用 \`--overwrite\`，程序不会替换已有输出。
- 解析不可信媒体文件仍可能触发 FFmpeg 中的缺陷；安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。

## 许可证

FLVConcat 使用 [GPL-3.0-only](LICENSE) 发布。官方二进制静态链接 FFmpeg；FFmpeg 的许可证和来源说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
