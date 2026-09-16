LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= ir_daemon.c
LOCAL_MODULE := ir_daemon
LOCAL_CFLAGS := -std=c99
LOCAL_SHARED_LIBRARIES := libc

include $(BUILD_EXECUTABLE)
