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
VENDOR_SECURITY_PATCH := 2023-08-01

# SEPolicy
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor

# Inherit from the proprietary version
-include vendor/motorola/nio/BoardConfigVendor.mk
