package com.airplay2.mirror;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Toast;

/**
 * AirPlay 镜像接收器主界面。
 *
 * 与原版一致：全屏视频画面（无任何操作条），启动即自动开启 AirPlay 服务器，
 * 服务器状态通过 Toast 短暂提示（便于确认是否成功）。
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "MainActivity";

    private SurfaceView surfaceView;
    private VideoSink videoSink;
    private AudioSink audioSink;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surface);
        surfaceView.getHolder().addCallback(this);

        videoSink = new VideoSink(null, new VideoSink.Listener() {
            @Override
            public void onVideoFrameCount(int count) {
                // 视频帧统计仅用于日志排查
            }

            @Override
            public void onVideoError(String msg) {
                Toast.makeText(MainActivity.this, "视频错误: " + msg, Toast.LENGTH_LONG).show();
            }
        });
        audioSink = new AudioSink(new AudioSink.Listener() {
            @Override
            public void onAudioError(String msg) {
                Toast.makeText(MainActivity.this, "音频错误: " + msg, Toast.LENGTH_LONG).show();
            }

            @Override
            public void onAudioPcmBytes(int bytes) {
            }

            @Override
            public void onAudioAacPackets(int packets) {
            }
        });
        NativeBridge.attach(this, videoSink, audioSink);

        // 启动即自动开启 AirPlay 服务器（与原版一致，无需手动操作）
        startServer();
    }

    @Override
    protected void onDestroy() {
        NativeBridge.stopServer();
        NativeBridge.detach();
        if (videoSink != null) videoSink.release();
        if (audioSink != null) audioSink.release();
        super.onDestroy();
    }

    // ---- Surface 生命周期：把 Surface 交给视频解码器 ----

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        videoSink.setSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        videoSink.setSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        videoSink.setSurface(null);
    }

    // ---- 服务器控制 ----

    private void startServer() {
        boolean ok = NativeBridge.startServer(this, "AirPlay Mirror", 7000);
        Toast.makeText(this, ok ? "AirPlay 服务器已启动" : "AirPlay 服务器启动失败", Toast.LENGTH_SHORT).show();
    }

    // ---- JNI 状态回调（NativeBridge 转发到主线程）----

    void onServerStatus(boolean ok, String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    void onSessionStatus(String message) {
        // 会话状态仅用于日志排查
    }
}
