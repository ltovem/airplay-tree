/*!
 * @file mirror_jni.cpp
 * @brief AirPlay 镜像接收器 JNI 桥接层（Android）
 *
 * 职责：
 *   1. 持有 AirPlayServer 单例，把 Java 的 start/stop 调用转发给库；
 *   2. 实现 IAudioRenderer / IVideoRenderer，把库工作线程上的
 *      PCM/压缩音频/视频帧回调转发给 Java 静态方法（MediaCodec/AudioTrack
 *      在 Java 侧完成硬解码与播放）。
 *
 * 线程安全：
 *   - 回调来自库的内部线程（RTSP / RTP / 播放线程），转发时必须
 *     AttachCurrentThread 拿到本线程的 JNIEnv（用 thread_local 缓存）。
 *   - 所有 Java 静态回调方法都通过 JNI_OnLoad 里缓存的 jmethodID 调用，
 *     不缓存 Java 对象引用（避免泄漏 / 生命周期问题）。
 */
#include <jni.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>

#include "airplay2/airplay_server.h"
#include "airplay2/airplay_config.h"

using namespace airplay2;

namespace {

JavaVM* g_vm = nullptr;

// ---- 缓存 Java 静态回调方法 ID（JNI_OnLoad 里初始化）----
jclass    g_bridge_cls   = nullptr;
jmethodID m_on_video_cfg = nullptr;
jmethodID m_on_video_frm = nullptr;
jmethodID m_on_audio_cfg = nullptr;
jmethodID m_on_pcm       = nullptr;
jmethodID m_on_ccfg      = nullptr;
jmethodID m_on_caudio    = nullptr;
jmethodID m_on_started   = nullptr;
jmethodID m_on_stopped   = nullptr;
jmethodID m_on_status    = nullptr;

/*! 取当前线程的 JNIEnv；库回调线程需要 Attach 才能调用 Java */
JNIEnv* GetEnv() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    return env;
}

/*! 把 C++ vector 转成 Java byte[] */
jbyteArray ToByteArray(JNIEnv* env, const uint8_t* data, size_t len) {
    jbyteArray arr = env->NewByteArray((jsize)len);
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)len, (const jbyte*)data);
    return arr;
}

/*!
 * @brief 视频渲染回调实现：帧直接转发给 Java MediaCodec。
 */
class JniVideoRenderer : public IVideoRenderer {
public:
    void on_config(const VideoConfig& cfg) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_video_cfg) return;
        jbyteArray extra = ToByteArray(env, cfg.codec_extra.data(), cfg.codec_extra.size());
        env->CallStaticVoidMethod(g_bridge_cls, m_on_video_cfg,
                                  (jint)cfg.codec, (jint)cfg.width, (jint)cfg.height, extra);
        if (extra) env->DeleteLocalRef(extra);
    }

    void on_frame(const VideoFrame& frame) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_video_frm) return;
        jbyteArray annex = ToByteArray(env, frame.annex_b.data(), frame.annex_b.size());
        env->CallStaticVoidMethod(g_bridge_cls, m_on_video_frm,
                                  (jint)frame.codec, (jlong)frame.pts_us,
                                  (jboolean)frame.is_key, annex);
        if (annex) env->DeleteLocalRef(annex);
    }

    void on_stop() override {
        JNIEnv* env = GetEnv();
        if (env && m_on_video_frm) {
            env->CallStaticVoidMethod(g_bridge_cls, m_on_video_frm,
                                      (jint)-1, (jlong)0, (jboolean)false, nullptr);
        }
    }
};

/*!
 * @brief 音频渲染回调实现。
 */
class JniAudioRenderer : public IAudioRenderer {
public:
    Status on_config(const AudioConfig& config) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_audio_cfg) return Status::OK;
        env->CallStaticVoidMethod(g_bridge_cls, m_on_audio_cfg,
                                  (jint)config.sample_rate, (jint)config.channels,
                                  (jint)config.format);
        return Status::OK;
    }

    Status on_pcm(const uint8_t* pcm_data, size_t num_bytes,
                  uint64_t timestamp_us) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_pcm || !pcm_data || num_bytes == 0) return Status::OK;
        jbyteArray arr = ToByteArray(env, pcm_data, num_bytes);
        env->CallStaticVoidMethod(g_bridge_cls, m_on_pcm, arr, (jlong)timestamp_us);
        if (arr) env->DeleteLocalRef(arr);
        return Status::OK;
    }

    void on_compressed_config(const std::string& codec, const std::string& fmtp,
                              const AudioConfig& cfg) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_ccfg) return;
        jstring jcodec = env->NewStringUTF(codec.c_str());
        jstring jfmtp  = env->NewStringUTF(fmtp.c_str());
        env->CallStaticVoidMethod(g_bridge_cls, m_on_ccfg, jcodec, jfmtp,
                                  (jint)cfg.sample_rate, (jint)cfg.channels);
        if (jcodec) env->DeleteLocalRef(jcodec);
        if (jfmtp)  env->DeleteLocalRef(jfmtp);
    }

    Status on_compressed_audio(const uint8_t* data, size_t len,
                               uint64_t timestamp_us) override {
        JNIEnv* env = GetEnv();
        if (!env || !m_on_caudio || !data || len == 0) return Status::OK;
        jbyteArray arr = ToByteArray(env, data, len);
        env->CallStaticVoidMethod(g_bridge_cls, m_on_caudio, arr, (jlong)timestamp_us);
        if (arr) env->DeleteLocalRef(arr);
        return Status::OK;
    }

    void on_play()  override {}
    void on_pause() override {}
    void on_stop()  override {}
    void on_flush() override {}
};

// ---- 服务器单例（与 Java start/stop 生命周期一致）----
std::mutex       g_srv_mu;
AirPlayServer*   g_server = nullptr;
JniAudioRenderer g_audio_renderer;
JniVideoRenderer g_video_renderer;

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass local = env->FindClass("com/airplay2/mirror/NativeBridge");
    if (!local) return JNI_ERR;
    g_bridge_cls = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    m_on_video_cfg = env->GetStaticMethodID(g_bridge_cls, "nativeOnVideoConfig",
                                            "(III[B)V");
    m_on_video_frm = env->GetStaticMethodID(g_bridge_cls, "nativeOnVideoFrame",
                                            "(IJZ[B)V");
    m_on_audio_cfg = env->GetStaticMethodID(g_bridge_cls, "nativeOnAudioConfig",
                                            "(III)V");
    m_on_pcm       = env->GetStaticMethodID(g_bridge_cls, "nativeOnPcm",
                                            "([BJ)V");
    m_on_ccfg      = env->GetStaticMethodID(g_bridge_cls, "nativeOnCompressedConfig",
                                            "(Ljava/lang/String;Ljava/lang/String;II)V");
    m_on_caudio    = env->GetStaticMethodID(g_bridge_cls, "nativeOnCompressedAudio",
                                            "([BJ)V");
    m_on_started   = env->GetStaticMethodID(g_bridge_cls, "nativeOnServerStarted",
                                            "(ZLjava/lang/String;)V");
    m_on_stopped   = env->GetStaticMethodID(g_bridge_cls, "nativeOnServerStopped",
                                            "()V");
    m_on_status    = env->GetStaticMethodID(g_bridge_cls, "nativeOnSessionStatus",
                                            "(Ljava/lang/String;)V");
    return JNI_VERSION_1_6;
}

/*! Java: nativeStartServer(deviceName, port, rtpMin, rtpMax, deviceId) -> boolean */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_airplay2_mirror_NativeBridge_nativeStartServer(
    JNIEnv* env, jclass, jstring device_name, jint port, jint rtp_min, jint rtp_max,
    jstring device_id) {
    std::lock_guard<std::mutex> lk(g_srv_mu);
    if (g_server) return JNI_TRUE;  // 已在运行

    const char* name_utf = device_name ? env->GetStringUTFChars(device_name, nullptr) : nullptr;
    std::string name = name_utf ? name_utf : "AirPlay Mirror";
    if (name_utf) env->ReleaseStringUTFChars(device_name, name_utf);

    // deviceid / pi 字段（TXT 里必需的非空值）：iOS 依赖它识别设备，
    // 为空会直接隐藏该 AirPlay 设备。由 Java 侧用 Android ID 生成稳定值。
    std::string dev_id;
    if (device_id) {
        const char* id_utf = env->GetStringUTFChars(device_id, nullptr);
        if (id_utf) { dev_id = id_utf; env->ReleaseStringUTFChars(device_id, id_utf); }
    }
    if (dev_id.empty()) dev_id = "00:00:00:00:00:00"; // 兜底，正常不会走到

    if (AirPlayServer::global_init() != Status::OK) return JNI_FALSE;

    DeviceInfo dev;
    // 与原版一致的宣告参数（原版能被 iPhone 发现）：
    //   设备名 "AirPlay Mirror"、型号 AppleTV6,2
    dev.name = name;
    dev.model = "AppleTV6,2";
    dev.device_id = dev_id;
    dev.supports_audio = true;
    dev.supports_video = true;
    dev.supports_photo = true;
    dev.requires_encryption = false; // 无 MFi 证书：回退 legacy PIN 配对

    ServerConfig cfg;
    cfg.device = dev;
    cfg.control_port = (uint16_t)(port > 0 ? port : 7000);
    cfg.rtp_port_min = (uint16_t)(rtp_min > 0 ? rtp_min : 5000);
    cfg.rtp_port_max = (uint16_t)(rtp_max > rtp_min ? rtp_max : cfg.rtp_port_min + 20);
    cfg.publish_mdns = true;
    cfg.enable_logging = true;
    cfg.log_level = 3; // debug：打印每个 HTTP 请求，便于排查连接会话

    ServerCallbacks cb;
    cb.on_started = []() {
        JNIEnv* e = GetEnv();
        if (e && m_on_started) {
            jstring ok = e->NewStringUTF("AirPlay 已启动，请在 iPhone 上选择本设备");
            e->CallStaticVoidMethod(g_bridge_cls, m_on_started, (jboolean)JNI_TRUE, ok);
            if (ok) e->DeleteLocalRef(ok);
        }
    };
    cb.on_stopped = []() {
        JNIEnv* e = GetEnv();
        if (e && m_on_stopped) e->CallStaticVoidMethod(g_bridge_cls, m_on_stopped);
    };

    g_server = new AirPlayServer(cfg, cb, &g_audio_renderer, &g_video_renderer);
    Status st = g_server->start();
    if (st != Status::OK) {
        delete g_server;
        g_server = nullptr;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/*! Java: nativeStopServer() */
extern "C" JNIEXPORT void JNICALL
Java_com_airplay2_mirror_NativeBridge_nativeStopServer(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_srv_mu);
    if (!g_server) return;
    g_server->stop();
    delete g_server;
    g_server = nullptr;
    AirPlayServer::global_cleanup();
}
