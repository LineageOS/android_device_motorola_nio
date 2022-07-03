/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/properties.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include "vendor_init.h"

using android::base::GetProperty;

namespace {
void PropertyOverride(const std::string& name, const std::string& value, bool add = true) {
    auto pi = const_cast<prop_info*>(__system_property_find(name.c_str()));

    if (pi != nullptr) {
        __system_property_update(pi, value.c_str(), value.size());
    } else if (add) {
        __system_property_add(name.c_str(), name.size(), value.c_str(), value.size());
    }
}
};  // anonymous namespace

void vendor_load_properties() {
    std::string value = GetProperty("ro.boot.carrier", "");
    if (!value.empty()) PropertyOverride("ro.carrier", value);
}
