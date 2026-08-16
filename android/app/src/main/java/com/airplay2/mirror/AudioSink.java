package com.airplay2.mirror;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.ArrayList;

/**
 * 音频渲染器：处理两条播放路径
 *  1. ALAC / PCM（音乐投送）：库解出 PCM16 后 onPcm() → AudioTrack 直接播放；
 *  2. AAC-ELD（屏幕镜像音频）：压缩帧经 onCompressedConfig/onCompressedAudio
 *     透传，这里用 MediaCodec("audio/mp4a-latm") 解码成 PCM 再喂 AudioTrack。
 *
 * 压缩帧格式 = RFC 3640 AU-headers（sizelength=13, indexdeltaLength=3）。
 */
public class AudioSink {
    private static final String TAG = "AudioSink";

    public interface Listener {
        void onAudioError(String msg);
        void onAudioPcmBytes(int bytes);
        void onAudioAacPackets(int packets);
    }

    private final Listener listener;

    // ---- 输出（两种路径共用）----
    private AudioTrack track;
    private int sampleRate = 44100;
    private int channels = 2;

    // ---- AAC-ELD 解码状态 ----
    private MediaCodec aacCodec;
    private boolean aacReady;
    private byte[] asc;                 // AudioSpecificConfig
    private final ArrayList<byte[]> aacBatch = new ArrayList<>();

    public AudioSink(Listener listener) {
        this.listener = listener;
    }

    /** 会话配置（format: 0=PCM16LE 4=ALAC 6=AAC_ELD） */
    public synchronized void onConfig(int sampleRate, int channels, int format) {
        this.sampleRate = sampleRate > 0 ? sampleRate : 44100;
        this.channels = channels > 0 ? channels : 2;
        Log.i(TAG, "onConfig " + this.sampleRate + "Hz " + this.channels + "ch fmt=" + format);
        ensureTrack();
    }

    /** PCM 数据（ALAC 路径） */
    public synchronized void onPcm(byte[] pcm, long tsUs) {
        if (pcm == null || pcm.length == 0) return;
        if (listener != null) listener.onAudioPcmBytes(pcm.length);
        writeToTrack(pcm);
    }

    /** AAC-ELD 压缩格式配置（含 config= AudioSpecificConfig） */
    public synchronized void onCompressedConfig(String codec, String fmtp,
                                                 int sampleRate, int channels) {
        this.sampleRate = sampleRate > 0 ? sampleRate : 44100;
        this.channels = channels > 0 ? channels : 2;
        ensureTrack();

        asc = null;
        if (fmtp != null) {
            for (String kv : fmtp.split(";")) {
                kv = kv.trim();
                if (kv.startsWith("config=")) {
                    asc = hexDecode(kv.substring("config=".length()));
                    break;
                }
            }
        }
        releaseAac();
        if (asc == null || asc.length == 0) {
            Log.w(TAG, "no config= in fmtp, AAC-ELD decoder disabled");
            return;
        }
        initAacDecoder();
    }

    /** AAC-ELD 压缩帧（RFC 3640 AU-headers + 载荷） */
    public synchronized void onCompressedAudio(byte[] data, long tsUs) {
        if (data == null || data.length == 0) return;
        if (!aacReady || aacCodec == null) return;
        if (listener != null) listener.onAudioAacPackets(1);

        ArrayList<byte[]> aus = parseAus(data);
        if (aus.isEmpty()) return;
        for (byte[] au : aus) {
            feedAac(au);
            drainAac();
        }
    }

    /** 会话停止 */
    public synchronized void release() {
        releaseAac();
        if (track != null) {
            try {
                track.stop();
                track.release();
            } catch (Exception ignored) {
            }
            track = null;
        }
        aacBatch.clear();
    }

    // ---- 内部 ----

    private void ensureTrack() {
        if (track != null) {
            if (track.getSampleRate() == sampleRate && track.getChannelCount() == channels) return;
            try {
                track.stop();
                track.release();
            } catch (Exception ignored) {
            }
            track = null;
        }
        int minBuf = AudioTrack.getMinBufferSize(sampleRate, channels == 1
                ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf < 4096) minBuf = 4096;
        try {
            track = new AudioTrack(AudioManager.STREAM_MUSIC, sampleRate,
                    channels == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, minBuf * 2,
                    AudioTrack.MODE_STREAM);
            track.play();
        } catch (Exception e) {
            Log.e(TAG, "AudioTrack create failed: " + e.getMessage());
            track = null;
        }
    }

    private void writeToTrack(byte[] pcm) {
        if (track == null) return;
        try {
            int off = 0;
            while (off < pcm.length) {
                int w = track.write(pcm, off, pcm.length - off);
                if (w <= 0) break;
                off += w;
            }
        } catch (Exception e) {
            Log.w(TAG, "AudioTrack.write: " + e.getMessage());
        }
    }

    private void initAacDecoder() {
        try {
            MediaFormat fmt = MediaFormat.createAudioFormat(
                    MediaFormat.MIMETYPE_AUDIO_AAC, sampleRate, channels);
            fmt.setByteBuffer("csd-0", ByteBuffer.wrap(asc));
            aacCodec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
            aacCodec.configure(fmt, null, null, 0);
            aacCodec.start();
            aacReady = true;
            Log.i(TAG, "AAC-ELD decoder started (" + sampleRate + "Hz " + channels + "ch)");
        } catch (Exception e) {
            Log.e(TAG, "AAC decoder init failed: " + e.getMessage());
            if (listener != null) listener.onAudioError("AAC init: " + e.getMessage());
            releaseAac();
        }
    }

    private void releaseAac() {
        if (aacCodec != null) {
            try {
                aacCodec.stop();
                aacCodec.release();
            } catch (Exception ignored) {
            }
            aacCodec = null;
        }
        aacReady = false;
        aacBatch.clear();
    }

    private void feedAac(byte[] au) {
        if (aacCodec == null || au == null || au.length == 0) return;
        try {
            int inIdx = aacCodec.dequeueInputBuffer(10000);
            if (inIdx < 0) return;
            ByteBuffer buf = aacCodec.getInputBuffer(inIdx);
            buf.clear();
            buf.put(au);
            aacCodec.queueInputBuffer(inIdx, 0, au.length, 0, 0);
        } catch (Exception e) {
            Log.w(TAG, "feedAac: " + e.getMessage());
        }
    }

    private void drainAac() {
        if (aacCodec == null) return;
        try {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            for (int guard = 0; guard < 16; guard++) {
                int outIdx = aacCodec.dequeueOutputBuffer(info, 1000);
                if (outIdx >= 0) {
                    ByteBuffer out = aacCodec.getOutputBuffer(outIdx);
                    byte[] pcm = new byte[info.size];
                    if (out != null) {
                        out.position(info.offset);
                        out.get(pcm);
                    }
                    aacCodec.releaseOutputBuffer(outIdx, false);
                    if (pcm.length > 0) {
                        if (listener != null) listener.onAudioPcmBytes(pcm.length);
                        writeToTrack(pcm);
                    }
                } else {
                    break;
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "drainAac: " + e.getMessage());
        }
    }

    /**
     * RFC 3640 AU-headers 解析（与 macOS demo ParseBatchToAus 一致）：
     *   [0:2] = AU-header 长度（bit）
     *   payload = 2 + ceil(hdrBits/8)
     *   header 里每个 AU 用 sizelength(13) 位记录大小，第一个 AU 后跟
     *   indexlength(3) 位 AU-index，其余后跟 indexdeltaLength(3) 位 delta。
     */
    private ArrayList<byte[]> parseAus(byte[] raw) {
        ArrayList<byte[]> out = new ArrayList<>();
        if (raw.length < 2) return out;
        int hdrBits = ((raw[0] & 0xFF) << 8) | (raw[1] & 0xFF);
        int payloadOff = 2 + (hdrBits + 7) / 8;
        if (payloadOff > raw.length) payloadOff = raw.length;

        if (hdrBits == 0) {
            if (raw.length > 2) {
                byte[] au = new byte[raw.length - 2];
                System.arraycopy(raw, 2, au, 0, au.length);
                out.add(au);
            }
            return out;
        }

        final int sizelength = 13, indexlength = 3, indexdelta = 3;
        BitReader br = new BitReader(raw, 2);
        int auOff = payloadOff;
        int idx = 0;
        while (auOff < raw.length) {
            if (br.pos + sizelength > hdrBits) break;
            int auSize = br.read(sizelength);
            int skip = (idx == 0) ? indexlength : indexdelta;
            br.skip(skip);
            if (auSize <= 0 || auOff + auSize > raw.length) break;
            byte[] au = new byte[auSize];
            System.arraycopy(raw, auOff, au, 0, auSize);
            out.add(au);
            auOff += auSize;
            idx++;
        }
        if (out.isEmpty() && payloadOff < raw.length) {
            byte[] au = new byte[raw.length - payloadOff];
            System.arraycopy(raw, payloadOff, au, 0, au.length);
            out.add(au);
        }
        return out;
    }

    private static class BitReader {
        final byte[] data;
        int pos = 0;

        BitReader(byte[] d, int startByte) {
            data = d;
            pos = startByte * 8;
        }

        int read(int n) {
            int v = 0;
            for (int i = 0; i < n; i++) {
                int byteIdx = pos >> 3;
                if (byteIdx >= data.length) break;
                v = (v << 1) | ((data[byteIdx] >> (7 - (pos & 7))) & 1);
                pos++;
            }
            return v;
        }

        void skip(int n) { pos += n; }
    }

    private static byte[] hexDecode(String hex) {
        String s = hex.trim();
        if ((s.length() & 1) != 0) return new byte[0];
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            int hi = Character.digit(s.charAt(i * 2), 16);
            int lo = Character.digit(s.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) return new byte[0];
            out[i] = (byte) ((hi << 4) | lo);
        }
        return out;
    }
}
