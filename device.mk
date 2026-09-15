DEVICE_PATH := device/zte/b760h
VENDOR_PATH := vendor/zte/b760h

# Inherit the proprietary configuration.
$(call inherit-product, $(VENDOR_PATH)/b760h-vendor.mk)

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

# Root
PRODUCT_PACKAGES += \
    su


