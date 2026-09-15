# Inherit some common AOSP stuff.
$(call inherit-product, device/google/atv/products/atv_base.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/cm/config/common_full_tv.mk)

# Inherit device configuration
$(call inherit-product, device/zte/b760h/device.mk)

# Product Name
PRODUCT_NAME		:= cm_b760h

# Device identifier. This must come after all inclusions.
PRODUCT_DEVICE          := b760h
PRODUCT_BRAND           := ZTE
PRODUCT_MANUFACTURER	:= ZTE
PRODUCT_MODEL           := B760H
