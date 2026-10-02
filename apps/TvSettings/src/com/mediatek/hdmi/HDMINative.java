package com.mediatek.hdmi;

import android.util.Log;

public class HDMINative {
    private static final String TAG = "HDMINative";
    private static boolean sLoaded = false;

    static {
        try {
            System.loadLibrary("mtkhdmi_jni");
            sLoaded = true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load mtkhdmi_jni", e);
        }
    }

    private native boolean enableAudioNative(boolean z);
    private native boolean enableCecNative(boolean z);
    private native boolean enableHdcpNative(boolean z);
    private native boolean enableHdmiIpoNative(boolean z);
    private native boolean enableHdmiNative(boolean z);
    private native boolean enableVideoNative(boolean z);
    private native int getCapabilitiesNative();
    private native int[] getCecAddrNative();
    private native int[] getCecCmdNative();
    private native int[] getCecTxSTsNative();
    private native int getDisplayTypeNative();
    private native int[] getEdidNative();
    private native int getHdcpStatusNative();
    private native boolean hdmiPortraitEnableNative(boolean z);
    private native boolean hdmiPowerEnableNative(boolean z);
    private native boolean isHdmiForceAwakeNative();
    private native boolean needSwDrmProtectNative();
    private native boolean notifyOtgStateNative(int i);
    private native boolean resetHdmiFlashNative(int i);
    private native boolean set3DVideoNative(int i);
    private native boolean setAudioConfigNative(int i);
    private native boolean setCecAddrNative(byte b, byte[] bArr, int i, char c);
    private native boolean setCecCmdNative(byte b, byte b2, short s, byte[] bArr, int i, byte b3);
    private native boolean setDeepColorNative(int i, int i2);
    private native boolean setHdcpKeyNative(byte[] bArr);
    private native boolean setHdmiDrmKeyNative();
    private native boolean setUserMuteAudioNative(int i);
    private native boolean setVideoConfigNative(int i);

    public boolean setVideoConfig(int resolution) {
        return sLoaded && setVideoConfigNative(resolution);
    }
}