package com.airplay2.mirror;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * 视频渲染器：把库回调里的 Annex-B NAL 帧喂给 MediaCodec 硬解码，
 * 直接渲染到 SurfaceView 的 Surface 上。
 *
 * 关键点（参考 macOS demo 的 MacVideoRenderer）：
 *  1. iOS 镜像视频 = H.264/H.265，SPS/PPS 由 TCP Data Push 的 type=0x01
 *     包带内下发，因此每个 Annex-B 帧都要解析并把最新参数集缓存下来；
 *  2. MediaCodec 输入统一转成 AVCC（4 字节长度前缀）格式，并带 csd-0；
 *  3. 丢包时 has_loss=true：跳过非关键帧，等下一个 IDR 再恢复显示；
 *  4. 时间戳直接用库算好的 ptsUs（微秒）。
 */
public class VideoSink {
    private static final String TAG = "VideoSink";

    public interface Listener {
        void onVideoFrameCount(int count);
        void onVideoError(String msg);
    }

    private Surface surface;
    private final Listener listener;

    // 参数集缓存（带内下发的最新 SPS/PPS/VPS）
    private byte[] sps, pps, vps;
    private int codecType = -1; // 0=H264 1=H265
    private int initType = -1;  // 当前解码器初始化的 codec 类型

    private MediaCodec mediaCodec;
    private boolean configured;

    // 丢包恢复
    private boolean waitForKey = false;

    // 统计
    private int frameCount;

    public VideoSink(Surface surface, Listener listener) {
        this.surface = surface;
        this.listener = listener;
    }

    /** Surface 变化（Activity 生命周期）：解码器必须重建 */
    public void setSurface(Surface s) {
        if (surface == s) return;
        release();
        surface = s;
    }

    /** 会话配置（可能带 SDP 里的参数集）。codec=-1 表示会话结束 */
    public void onConfig(int codec, int width, int height, byte[] extra) {
        if (codec < 0) { release(); return; }
        Log.i(TAG, "onConfig codec=" + codec + " " + width + "x" + height);
        codecType = codec;
        if (extra != null && extra.length > 0) {
            parseParameterSets(extra, codec);
        }
        maybeInitCodec();
    }

    /** 一帧 Annex-B 到达 */
    public void onFrame(int codec, long ptsUs, boolean isKey, byte[] annexB) {
        if (codec < 0) { release(); return; }
        if (annexB == null || annexB.length == 0) return;
        if (surface == null) return;

        codecType = codec;
        frameCount++;
        if (listener != null && frameCount % 60 == 0) {
            listener.onVideoFrameCount(frameCount);
        }

        // 1) 解析帧内 NAL，缓存参数集
        List<Nal> nals = splitAnnexB(annexB);
        if (nals.isEmpty()) return;

        List<Nal> slices = new ArrayList<>();
        for (Nal n : nals) {
            int type = (codec == 1) ? ((n.data[0] >> 1) & 0x3F) : (n.data[0] & 0x1F);
            if (codec == 1) {
                if (type == 32) vps = n.data;
                else if (type == 33) sps = n.data;
                else if (type == 34) pps = n.data;
                else slices.add(n);
            } else {
                if (type == 7) sps = n.data;
                else if (type == 8) pps = n.data;
                else slices.add(n);
            }
        }
        // 只送 slice，参数集由 csd-0 携带（避免重复喂参数集导致解码器重置）
        if (slices.isEmpty()) return;

        // 2) 丢包：等下一个关键帧
        if (waitForKey && !isKey) return;

        // 3) 初始化解码器（首次或编解码类型变化时重建）
        if (!configured || initType != codecType) {
            if (!maybeInitCodec()) return;
        }
        if (!configured || mediaCodec == null) return;

        // 4) 转 AVCC 并送入解码器
        byte[] avcc = toAvcc(slices);
        if (avcc == null || avcc.length == 0) return;
        try {
            int inIdx = mediaCodec.dequeueInputBuffer(10000);
            if (inIdx >= 0) {
                ByteBuffer buf = mediaCodec.getInputBuffer(inIdx);
                buf.clear();
                buf.put(avcc);
                mediaCodec.queueInputBuffer(inIdx, 0, avcc.length,
                        ptsUs > 0 ? ptsUs : frameCount * 16666L, 0);
            }
        } catch (Exception e) {
            if (listener != null) listener.onVideoError("queueInputBuffer: " + e.getMessage());
        }
        if (isKey) waitForKey = false;
    }

    /** 会话停止：释放解码器（保留参数集缓存，供重建使用） */
    public void release() {
        try {
            if (mediaCodec != null) {
                mediaCodec.stop();
                mediaCodec.release();
            }
        } catch (Exception ignored) {
        }
        mediaCodec = null;
        configured = false;
        initType = -1;
        waitForKey = false;
    }

    // ---- 内部 ----

    private boolean maybeInitCodec() {
        // 参数集必须齐（H.265 还需要 VPS）
        if (sps == null || pps == null) return false;
        if (codecType == 1 && vps == null) return false;
        if (surface == null) return false;

        release();

        String mime = (codecType == 1) ? MediaFormat.MIMETYPE_VIDEO_HEVC
                                       : MediaFormat.MIMETYPE_VIDEO_AVC;
        MediaFormat fmt = MediaFormat.createVideoFormat(mime, 1280, 720);
        // 用带内参数集构造 csd-0（AVCC 头），MediaCodec 必须用它初始化
        if (codecType == 1) {
            fmt.setByteBuffer("csd-0", ByteBuffer.wrap(buildAvccHeader(new byte[][]{vps, sps, pps})));
        } else {
            fmt.setByteBuffer("csd-0", ByteBuffer.wrap(buildAvccHeader(new byte[][]{sps, pps})));
        }
        try {
            mediaCodec = MediaCodec.createDecoderByType(mime);
            mediaCodec.configure(fmt, surface, null, 0);
            mediaCodec.start();
            configured = true;
            initType = codecType;
            Log.i(TAG, "MediaCodec " + mime + " started");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "init codec failed: " + e.getMessage(), e);
            if (listener != null) listener.onVideoError("init codec: " + e.getMessage());
            mediaCodec = null;
            configured = false;
            initType = -1;
            return false;
        }
    }

    private void parseParameterSets(byte[] data, int codec) {
        List<Nal> nals = splitAnnexB(data);
        for (Nal n : nals) {
            int type = (codec == 1) ? ((n.data[0] >> 1) & 0x3F) : (n.data[0] & 0x1F);
            if (codec == 1) {
                if (type == 32) vps = n.data;
                else if (type == 33) sps = n.data;
                else if (type == 34) pps = n.data;
            } else {
                if (type == 7) sps = n.data;
                else if (type == 8) pps = n.data;
            }
        }
    }

    private static class Nal {
        byte[] data;
        Nal(byte[] d) { data = d; }
    }

    /** 把 Annex-B（起始码分隔）拆成 NAL 列表 */
    private static List<Nal> splitAnnexB(byte[] data) {
        List<Nal> out = new ArrayList<>();
        int start = -1;
        int i = 0;
        while (i + 3 < data.length) {
            // 找起始码 00 00 01 或 00 00 00 01
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                if (start >= 0) out.add(new Nal(copyOfRange(data, start, i)));
                start = i + 3;
                i += 3;
            } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                if (start >= 0) out.add(new Nal(copyOfRange(data, start, i)));
                start = i + 4;
                i += 4;
            } else {
                i++;
            }
        }
        if (start >= 0 && start < data.length) out.add(new Nal(copyOfRange(data, start, data.length)));
        return out;
    }

    /** AVCC 头：01 profile compat level FF E1 [spsLen][sps] 01 [ppsLen][pps] */
    private static byte[] buildAvccHeader(byte[][] paramSets) {
        byte[] hdr = new byte[6];
        hdr[0] = 1;
        hdr[1] = paramSets[0][1];   // profile
        hdr[2] = paramSets[0][2];   // compatibility
        hdr[3] = paramSets[0][3];   // level
        hdr[4] = (byte) 0xFF;
        hdr[5] = (byte) 0xE1;
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(128);
        out.writeBytes(hdr);
        out.writeBytes(new byte[]{(byte) (paramSets[0].length >> 8), (byte) paramSets[0].length});
        out.writeBytes(paramSets[0]);
        out.writeBytes(new byte[]{1});
        out.writeBytes(new byte[]{(byte) (paramSets[1].length >> 8), (byte) paramSets[1].length});
        out.writeBytes(paramSets[1]);
        // H.265 第三参数集（VPS）用 4 字节长度前缀
        if (paramSets.length > 2) {
            int vlen = paramSets[2].length;
            out.writeBytes(new byte[]{(byte) (vlen >> 24), (byte) (vlen >> 16), (byte) (vlen >> 8), (byte) vlen});
            out.writeBytes(paramSets[2]);
        }
        return out.toByteArray();
    }

    /** NAL 列表转 AVCC（4 字节长度前缀） */
    private static byte[] toAvcc(List<Nal> nals) {
        int total = 0;
        for (Nal n : nals) total += 4 + n.data.length;
        byte[] out = new byte[total];
        int off = 0;
        for (Nal n : nals) {
            int l = n.data.length;
            out[off++] = (byte) (l >> 24);
            out[off++] = (byte) (l >> 16);
            out[off++] = (byte) (l >> 8);
            out[off++] = (byte) l;
            System.arraycopy(n.data, 0, out, off, l);
            off += l;
        }
        return out;
    }

    private static byte[] copyOfRange(byte[] src, int from, int to) {
        int n = to - from;
        byte[] d = new byte[n];
        System.arraycopy(src, from, d, 0, n);
        return d;
    }
}
