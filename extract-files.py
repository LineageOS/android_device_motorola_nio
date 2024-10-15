#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)
namespace_imports = [
    'vendor/motorola/sm8250-common',
]
blob_fixups: blob_fixups_user_type = {
    ('vendor/lib/libmot_chi_desktop_helper.so', 'vendor/lib64/libmot_chi_desktop_helper.so'): blob_fixup()
        .add_needed('libgui_shim_vendor.so'),
    ('vendor/lib64/com.qti.feature2.gs.so', 'vendor/lib64/com.qti.feature2.rt.so',
     'vendor/lib64/hw/camera.qcom.so', 'vendor/lib64/hw/com.qti.chi.override.so'): blob_fixup()
        .binary_regex_replace(b'camera.mot.is.coming.cts', b'vendor.camera.coming.cts'),
    'vendor/lib64/vendor.qti.hardware.camera.postproc@1.0-service-impl.so': blob_fixup()
        .sig_replace('13 0A 00 94', '1F 20 03 D5'),
}
module = ExtractUtilsModule(
    'nio',
    'motorola',
    namespace_imports=namespace_imports,
    blob_fixups=blob_fixups,
)
if __name__ == '__main__':
    utils = ExtractUtils.device_with_common(
        module, 'sm8250-common', module.vendor
    )
    utils.run()
