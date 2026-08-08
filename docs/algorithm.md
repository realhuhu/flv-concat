# 时间轴修复模型

## 术语

- packet：一个 FLV 音频或视频 media tag。
- run：同一条流中时间戳连续推进的一段包；向后跳变超过阈值时开启新的 run。
- DTS：解码时间戳。FLV tag timestamp 作为视频 DTS 和音频时间戳。
- CTS：AVC video packet 中的 composition time；视频 PTS = DTS + CTS。

## 扫描与回绕

扫描器不使用 FFmpeg 解复用，因为解复用器可能在应用看到时间戳前已经进行溢出修正。它直接读取 11 字节 FLV tag header 和 H.264/H.265/AAC media header。媒体探测还会直接读取视频 avcC/hvcC 和音频 ASC sequence header；这使旧式 FLV `codec_id=12` 的 HEVC 不依赖 FFmpeg 的 FLV 解码器注册。

编码格式处理器位于 `src/codecs/`，由注册表把 FLV codec id 映射到 H.264、H.265 或其他格式。处理器负责 sequence header、SPS/VPS/PPS 尺寸解析和语义兼容检查；扫描器、时间轴和封装器只处理统一的包模型。

对每条流分别维护最近时间戳和 32 位回绕基数。接近 \`2^32\` 边界的低值恢复被识别为回绕；其他超过 \`run-gap\` 的向后跳变被视为新 run。小范围乱序保留在原 run 中，后续只对音频执行重发过滤。

## 音视频 run 配对

视频 run 与尚未使用的音频 run 按时间范围的 Jaccard 重叠度贪心配对，默认最低值为 0.30。配对后的两条流共享同一个零点，因此音频相对视频的真实晚开始或早结束会被保留。

## 重复推送检测

候选视频 run 必须满足：

1. 时间范围被此前保留的 run 包含（允许 500 ms 边界误差）。
2. 包数量不多于此前 run。
3. 至少 90% 的视频包同时具有相同原始时间戳和相同编码内容指纹。

只有时间戳相同但编码内容不同的 run 不会被删除。被确认重复的视频 run 及其配对音频 run 一起丢弃。

内容指纹使用 payload 大小和编码 payload 开头的采样字节计算 FNV-1a。它用于降低误判概率，不作为密码学完整性校验。

## 输出时间轴

每个保留的 run 对从当前输出游标开始。包在 run 对内使用：

\`output = pair_base + (raw_timestamp - pair_zero)\`

视频保留 FLV 中的 CTS，音频 PTS 与 DTS 相同。AAC 包的标称持续时间按每包 1024 个采样计算；视频使用下一帧 DTS 差，异常值回退到 50 ms。run 对的跨度包含最后一个包的标称持续时间，因此相邻 run 不共享相同 DTS。

音频包若在同一 run 内不大于此前已写时间戳，会被视为设备重发并删除。音视频共同向前跳变造成的空隙不会压缩：MP4 中会留下音频空隙与较长的视频 sample duration，从而保证空隙后的同步关系。

兼容的 H.265 分段如果使用了不同的 VPS/SPS/PPS，封装器会在该分段首个关键帧前追加对应的长度前缀参数集；视频 VCL 和音频 payload 仍保持原样。若 profile、色度、位深、尺寸、NAL 长度或参数集身份无法安全共存，则在输出初始化前拒绝合并。
