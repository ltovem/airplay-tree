package com.airplay2.mirror;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;

/**
 * JNI 桥接：暴露给 mirror_jni.so 的静态方法 + 供 UI 调用的 native 方法。
 */
public final class NativeBridge {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private static VideoSink videoSink;
    private static AudioSink audioSink;
    private static MainActivity activity;

    static {
        System.loadLibrary("mirror_jni");
    }

    private NativeBridge() {}

    // ---- UI 调用 ----

    public static boolean startServer(Context ctx, String deviceName, int port) {
        return nativeStartServer(deviceName, port, 5000, 5030, deviceId(ctx));
    }

    public static void stopServer() {
        nativeStopServer();
    }

    public static native boolean nativeStartServer(String deviceName, int port,
                                                   int rtpMin, int rtpMax,
                                                   String deviceId);
    public static native void nativeStopServer();

    /**
     * 用 Android ID 生成稳定的 MAC 格式 device_id（AA:BB:CC:DD:EE:FF）。
     * mDNS TXT 的 deviceid/pi 字段必须是它，iOS 才把设备列入投屏列表；
     * 为空会直接隐藏设备（这是之前 iPhone 搜不到设备的根因）。
     */
    private static String deviceId(Context ctx) {
        String aid = Settings.Secure.getString(ctx.getContentResolver(),
                Settings.Secure.ANDROID_ID);
        if (aid == null) aid = "0000000000000000";
        if (aid.length() < 12) aid = String.format("%016x", aid.hashCode() & 0xFFFFFFFFL);
        String hex = aid.substring(0, 12).toUpperCase();
        StringBuilder sb = new StringBuilder(17);
        for (int i = 0; i < 12; i += 2) {
            if (i > 0) sb.append(':');
            sb.append(hex.charAt(i)).append(hex.charAt(i + 1));
        }
        return sb.toString();
    }

    // ---- 状态接线（MainActivity 生命周期）----

    public static void attach(MainActivity act, VideoSink vs, AudioSink as) {
        activity = act;
        videoSink = vs;
        audioSink = as;
    }

    public static void detach() {
        activity = null;
        videoSink = null;
        audioSink = null;
    }

    // ============================================================
    // 以下方法由 mirror_jni.cpp 通过 JNI 调用（静态回调）
    // ============================================================

    static void nativeOnVideoConfig(int codec, int width, int height, byte[] extra) {
        if (videoSink != null) videoSink.onConfig(codec, width, height, extra);
    }

    static void nativeOnVideoFrame(int codec, long ptsUs, boolean isKey, byte[] annexB) {
        if (videoSink != null) videoSink.onFrame(codec, ptsUs, isKey, annexB);
    }

    static void nativeOnAudioConfig(int sampleRate, int channels, int format) {
        if (audioSink != null) audioSink.onConfig(sampleRate, channels, format);
    }

    static void nativeOnPcm(byte[] pcm, long tsUs) {
        if (audioSink != null) audioSink.onPcm(pcm, tsUs);
    }

    static void nativeOnCompressedConfig(String codec, String fmtp,
                                         int sampleRate, int channels) {
        if (audioSink != null) {
            audioSink.onCompressedConfig(codec, fmtp, sampleRate, channels);
        }
    }

    static void nativeOnCompressedAudio(byte[] data, long tsUs) {
        if (audioSink != null) audioSink.onCompressedAudio(data, tsUs);
    }

    static void nativeOnServerStarted(final boolean ok, final String message) {
        MAIN.post(() -> {
            if (activity != null) activity.onServerStatus(ok, message);
        });
    }

    static void nativeOnServerStopped() {
        MAIN.post(() -> {
            if (activity != null) activity.onServerStatus(false, "AirPlay 已停止");
        });
    }

    static void nativeOnSessionStatus(final String message) {
        MAIN.post(() -> {
            if (activity != null) activity.onSessionStatus(message);
        });
    }
}
