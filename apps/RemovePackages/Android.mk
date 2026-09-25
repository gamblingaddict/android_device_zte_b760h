LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := RemovePackages
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_TAGS := optional
LOCAL_OVERRIDES_PACKAGES := \
	LockClock \
	DeskClock \
	messaging \
	Dialer \
	Contacts \
	ContactsProvider \
	Trebuchet \
	AudioFX \
	Calculator \
	Calendar \
	CalendarProvider \
	Camera2 \
	Eleven \
	ThemeChooser \
	Gallery2 \
	Recorder \
	ExactCalculator \
	Jelly \
	PhotoTable

LOCAL_UNINSTALLABLE_MODULE := true
LOCAL_CERTIFICATE := PRESIGNED
LOCAL_SRC_FILES := /dev/null
include $(BUILD_PREBUILT)