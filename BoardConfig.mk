#
# Copyright (C) 2022-2023 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

include device/motorola/sm8250-common/BoardConfigCommon.mk

DEVICE_PATH := device/motorola/nio

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := nio

# Display
TARGET_SCREEN_DENSITY := 480

# HIDL
DEVICE_MANIFEST_FILE += $(DEVICE_PATH)/manifest.xml

# Kernel
TARGET_KERNEL_CONFIG += vendor/ext_config/nio-default.config
BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD += nova_0flash_mmi.ko
BOOT_KERNEL_MODULES := $(BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD)

# Security patch level
VENDOR_SECURITY_PATCH := 2022-06-01

# SEPolicy
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor

# Touch
SOONG_CONFIG_NAMESPACES += MOTO_KONA_TOUCH
SOONG_CONFIG_MOTO_KONA_TOUCH := SINGLE_TAP_PATH
SOONG_CONFIG_MOTO_KONA_TOUCH_SINGLE_TAP_PATH := /sys/class/touchscreen/NVT-ts/single_click
TARGET_TAP_TO_WAKE_NODE := /sys/class/touchscreen/NVT-ts/double_click

# Inherit from the proprietary version
-include vendor/motorola/nio/BoardConfigVendor.mk
