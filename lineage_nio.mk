#
# Copyright (C) 2022 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from nio device
$(call inherit-product, device/motorola/nio/device.mk)

# Device identifier. This must come after all inclusions.
PRODUCT_NAME := lineage_nio
PRODUCT_DEVICE := nio
PRODUCT_MODEL := XT2125-4

PRODUCT_SYSTEM_NAME := nio_retcn

# Build info
PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="nio_retcn-user 12 S1RN32.55-16-13 fce21a-b9f4e9 release-keys" \
    BuildFingerprint=motorola/nio_retcn/nio:12/S1RN32.55-16-13/fce21a-b9f4e9:user/release-keys \
    DeviceProduct=nio_retcn \
    SystemName=nio_retcn
