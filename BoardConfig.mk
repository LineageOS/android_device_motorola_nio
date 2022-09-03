#
# Copyright (C) 2022 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

include device/motorola/sm8250-common/BoardConfigCommon.mk

DEVICE_PATH := device/motorola/nio

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := nio

# Kernel
TARGET_KERNEL_CONFIG := vendor/lineageos_nio_defconfig

# Inherit from the proprietary version
-include vendor/motorola/nio/BoardConfigVendor.mk
