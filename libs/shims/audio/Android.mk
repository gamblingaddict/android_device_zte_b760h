LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := audio.cpp
LOCAL_MODULE := libshim_audio
include $(BUILD_SHARED_LIBRARY)
