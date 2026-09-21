DEVICE_PATH := device/zte/b760h
VENDOR_PATH := vendor/zte/b760h

# Inherit the proprietary configuration.
$(call inherit-product, $(VENDOR_PATH)/b760h-vendor.mk)

# Dalvik/HWUI
$(call inherit-product, frameworks/native/build/tablet-10in-xhdpi-2048-dalvik-heap.mk)
$(call inherit-product-if-exists, frameworks/native/build/phone-xxhdpi-2048-hwui-memory.mk)

# Gapps
$(call inherit-product-if-exists, vendor/gapps/vendor.mk)

# Boot animation
TARGET_BOOTANIMATION_MULTITHREAD_DECODE := true
TARGET_SCREEN_WIDTH := 1920
TARGET_SCREEN_HEIGHT := 1080

# Permissions/features
PERM_PATH := frameworks/native/data/etc
PERM_DEST := system/etc/permissions

PRODUCT_COPY_FILES += \
    $(PERM_PATH)/android.hardware.bluetooth.xml:$(PERM_DEST)/android.hardware.bluetooth.xml \
    $(PERM_PATH)/android.hardware.bluetooth_le.xml:$(PERM_DEST)/android.hardware.bluetooth_le.xml \
    $(PERM_PATH)/android.hardware.usb.host.xml:$(PERM_DEST)/android.hardware.usb.host.xml \
    $(PERM_PATH)/android.hardware.wifi.xml:$(PERM_DEST)/android.hardware.wifi.xml \
    $(PERM_PATH)/android.hardware.wifi.direct.xml:$(PERM_DEST)/android.hardware.wifi.direct.xml \
    $(PERM_PATH)/android.hardware.ethernet.xml:$(PERM_DEST)/android.hardware.ethernet.xml

# Init
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,${DEVICE_PATH}/configs/init,root)

# WiFi
PRODUCT_PACKAGES += \
    libwpa_client \
    hostapd \
    wpa_supplicant \
    wpa_supplicant.conf \
    wificond

# Bluetooth
PRODUCT_PACKAGES += \
     libbt-vendor

# Audio
PRODUCT_PACKAGES += \
    audio.primary.mt8127 \
    audio.a2dp.default \
    audio_policy.default \
    audio_policy.stub \
    audio.r_submix.default \
    audio.usb.default \
    audio.primary.default \
    libaudio-resampler

# Root
PRODUCT_PACKAGES += \
    su

# Remote
PRODUCT_PACKAGES += \
    ir_daemon

# Shims/stubs
PRODUCT_PACKAGES += \
    libshim_ui \
    libshim_gui \
    libshim_utils \
    libshim_audio\
    libshim_omx \
    libshim_log