/*!
 * @file video_renderer.h
 * @brief AirPlay 视频 / 屏幕镜像渲染器回调接口
 *
 * AirPlay 视频流有两种模式：
 *   1. **Data Push**（RTP 推送，屏幕镜像常用）：
 *      发送端把 H.264 / H.265 切片打包成 RTP 包（视频用 UDP data 端口；
 *      控制走 RTSP /action），库做 NAL 重组和丢包检测，通过
 *      IVideoRenderer 回调送出一帧完整的 Annex-B NAL 单元。
 *
 *   2. **URL Pull**（拉流播放，常见于 iTunes / iOS Music App 投片）：
 *      RTSP POST /play body 里携带 playURL，客户端把这个 URL 交给本地
 *      HTTP(S)/HLS/DASH 播放器拉流。库通过 on_url 回调把 playURL 通知给
 *      上层应用，不做解析。外部需要自行处理 HLS 清单拉取 / 解码 / DRM。
 *
 * 典型实现：
 *   - iOS/macOS 里把 NAL 挂到 VTDecompressionSessionCreate / AVSampleBufferDisplayLayer
 *   - Android 送 MediaCodec (configure H.264/H.265, surface mode)
 *   - Linux 用 VA-API / VDPAU / FFmpeg avcodec_send_packet + EGL
 *
 * 低延时建议：
 *   - 视频 buffer_depth_ms 控制在 60~120ms（音画同步由外部做）
 *   - 收到 SPS/PPS/VPS 立刻送解码器（关键帧前的 codec config 只来一次）
 *   - 丢 P 帧（不关键）直接等下一帧 I 帧，不要请求重传（AirPlay 视频没有重传）
 */
#ifndef AIRPLAY2_VIDEO_RENDERER_H
#define AIRPLAY2_VIDEO_RENDERER_H

#include "airplay_config.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace airplay2 {

/*!
 * @brief 视频编解码器类型（AirPlay 发送端目前只送这两种）
 */
enum class VideoCodec : uint8_t {
    H264_AVC    = 0,  ///< H.264 / AVC (Annex-B byte-stream NAL, 默认)
    H265_HEVC   = 1,  ///< H.265 / HEVC (含 VPS/SPS/PPS)
    MJPEG       = 2,  ///< 偶见快速同步 / 低分辨率场景
    UNKNOWN     = 0xFF
};

/*!
 * @brief 视频尺寸 + 色彩空间
 */
struct VideoConfig {
    VideoCodec  codec       = VideoCodec::H264_AVC;
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    uint32_t    fps_num     = 0;  ///< 帧率分子（0 表示未知）
    uint32_t    fps_den     = 0;  ///< 帧率分母
    uint8_t     bit_depth   = 8;  ///< 8/10/12 位
    bool        full_range  = false; ///< JPEG full range vs video range
    std::vector<uint8_t> codec_extra; ///< SPS/PPS/VPS (一次性设置)
};

/*!
 * @brief 一帧完整的 NAL 单元
 */
struct VideoFrame {
    VideoCodec  codec       = VideoCodec::H264_AVC;
    uint64_t    pts_us      = 0;    ///< 显示时间戳（微秒，RTP timestamp 换算）
    uint64_t    dts_us      = 0;    ///< 解码时间戳（若无则 = pts）
    bool        is_key      = false;///< 关键帧（H.264 IDR / H.265 CRA）
    bool        has_loss    = false;///< 本帧之前有丢包（通知解码器丢弃到下一关键帧）
    /// Annex-B 字节流：start_code (0x00000001) + NAL + start_code + NAL + ...
    /// 调用方可直接喂 ffmpeg av_parser_parse2 / VideoToolbox / MediaCodec
    std::vector<uint8_t> annex_b;
    uint32_t    width       = 0;    ///< 宽（若 codec_extra 里有则自动填充）
    uint32_t    height      = 0;    ///< 高
};

/*!
 * @brief 视频播放控制命令（来自 /action play/rate/scrub/stop）
 *
 * 与音频不同，视频有"位置跳转"(scrub)，需要上层解码管线能 seek。
 */
struct VideoPlaybackCmd {
    enum Type {
        PLAY,           ///< 开始或恢复播放
        PAUSE,          ///< 暂停在当前帧
        STOP,           ///< 完全停止（释放解码器）
        SEEK            ///< 跳到指定位置
    };
    Type        type = PLAY;
    double      rate = 1.0;     ///< PLAY 时的播放速率（1.0 = 正常速度）
    double      start_pos_sec = 0.0; ///< PLAY / SEEK 时的起始位置（秒）
    std::string content_url;    ///< URL Pull 模式才非空（http:// 或 /playQueue index 语法）
};

/*!
 * @brief AirPlay 视频渲染回调接口
 *
 * 所有回调都在库内部"视频工作线程"上被调用。不要在回调里执行
 * 阻塞操作（> 10ms），否则会造成丢帧或低延时恶化。
 */
class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;

    /// 新会话开始时或 SPS/PPS/VPS 变更时调用；内部应 (re)configure 解码器
    virtual void on_config(const VideoConfig& cfg) = 0;

    /// URL Pull 模式（播放线上视频 / HLS）：客户端请求播放某个 URL
    /// 返回 true 表示上层已接受并开始拉流；false 表示库可以拒绝这条流
    virtual bool on_url(const VideoPlaybackCmd& cmd) { (void)cmd; return true; }

    /// Data Push 模式：一帧完整 NAL 到达。若 frame.is_key=false 且
    /// frame.has_loss=true，实现端应丢弃直到下一个关键帧。
    virtual void on_frame(const VideoFrame& frame) = 0;

    /// 播放控制命令（PLAY/PAUSE/STOP/SEEK）
    virtual void on_playback(const VideoPlaybackCmd& cmd) { (void)cmd; }

    /// 播放结束回调
    virtual void on_stop() {}
};

} // namespace airplay2

#endif // AIRPLAY2_VIDEO_RENDERER_H
