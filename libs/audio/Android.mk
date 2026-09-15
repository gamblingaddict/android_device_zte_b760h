LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= audio.cpp

LOCAL_MODULE := audio.primary.mt8127
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_C_INCLUDES += frameworks/av/media/mtp/ system/core/include/ frameworks/rs/server/ frameworks/av/include/ hardware/libhardware/include/
#LOCAL_LDFLAGS += -L$(LOCAL_PATH)/../../../../vendor/wiko/rainbow/lib/ -laudio.primary.default
LOCAL_SHARED_LIBRARIES := libdl libcutils liblog libutils libbinder

include $(BUILD_SHARED_LIBRARY)
