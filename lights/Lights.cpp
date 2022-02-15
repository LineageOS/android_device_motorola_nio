/*
 * Copyright (C) 2019 The Android Open Source Project
 * Copyright (C) 2020-2022 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.lights-service.motokona"

#include "Lights.h"
#include <android-base/file.h>
#include <android-base/logging.h>

/* clang-format off */
#define PPCAT_NX(A, B) A/B
#define PPCAT(A, B) PPCAT_NX(A, B)
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

#define CHARGING_ATTR(x) STRINGIFY(PPCAT(/sys/class/leds/charging, x))
/* clang-format on */

namespace aidl {
namespace android {
namespace hardware {
namespace light {

namespace {

// Write value to path and close file.
template <typename T>
inline bool WriteToFile(const std::string& path, T content) {
    return ::android::base::WriteStringToFile(std::to_string(content), path);
}

uint32_t RgbaToBrightness(uint32_t color) {
    // Extract brightness from AARRGGBB.
    uint32_t alpha = (color >> 24) & 0xFF;

    // Retrieve each of the RGB colors
    uint32_t red = (color >> 16) & 0xFF;
    uint32_t green = (color >> 8) & 0xFF;
    uint32_t blue = color & 0xFF;

    // Scale RGB colors if a brightness has been applied by the user
    if (alpha != 0xFF && alpha != 0) {
        red = red * alpha / 0xFF;
        green = green * alpha / 0xFF;
        blue = blue * alpha / 0xFF;
    }

    return (77 * red + 150 * green + 29 * blue) >> 8;
}

inline bool IsLit(uint32_t color) {
    return color & 0x00ffffff;
}

void ApplyNotificationState(const HwLightState& state) {
    auto brightness = RgbaToBrightness(state.color);

    // Turn off the leds (initially)
    WriteToFile(CHARGING_ATTR(breath), 0);
    if (state.flashMode == FlashMode::TIMED && state.flashOnMs > 0 && state.flashOffMs > 0) {
        WriteToFile(CHARGING_ATTR(delay_on), state.flashOnMs);
        WriteToFile(CHARGING_ATTR(delay_off), state.flashOffMs);
        WriteToFile(CHARGING_ATTR(breath), 1);
    } else {
        WriteToFile(CHARGING_ATTR(brightness), brightness);
    }
}

}  // anonymous namespace

ndk::ScopedAStatus Lights::setLightState(int id, const HwLightState& state) {
    static_assert(kAvailableLights.size() == std::tuple_size_v<decltype(notif_states_)>);

    if (id == static_cast<int32_t>(LightType::BACKLIGHT)) {
        // Stub backlight handling
        return ndk::ScopedAStatus::ok();
    }

    // Update saved state first
    bool found = false;
    for (size_t i = 0; i < notif_states_.size(); ++i) {
        if (kAvailableLights[i].id == id) {
            notif_states_[i] = state;
            LOG(DEBUG) << __func__ << ": updating id=" << id
                       << ", type=" << toString(kAvailableLights[i].type);
            found = true;
            break;
        }
    }
    if (!found) {
        LOG(ERROR) << "Light not supported";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    // Lit up in order or fallback to battery light if others are dim
    for (size_t i = 0; i < notif_states_.size(); ++i) {
        auto&& cur_state = notif_states_[i];
        auto&& cur_light = kAvailableLights[i];
        if (IsLit(cur_state.color) || cur_light.type == LightType::BATTERY) {
            LOG(DEBUG) << __func__ << ": applying id=" << cur_light.id
                       << ", type=" << toString(cur_light.type);
            ApplyNotificationState(cur_state);
            break;
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Lights::getLights(std::vector<HwLight>* lights) {
    lights->insert(lights->end(), kAvailableLights.begin(), kAvailableLights.end());
    // We don't handle backlight but still need to report as supported.
    lights->push_back({static_cast<int32_t>(LightType::BACKLIGHT), 0, LightType::BACKLIGHT});
    return ndk::ScopedAStatus::ok();
}

}  // namespace light
}  // namespace hardware
}  // namespace android
}  // namespace aidl
