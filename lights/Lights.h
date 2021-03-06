/*
 * Copyright (C) 2020 The Android Open Source Project
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

#pragma once

#include <aidl/android/hardware/light/BnLights.h>
#include <array>

namespace aidl {
namespace android {
namespace hardware {
namespace light {

// Keep sorted in the order of priority.
constexpr std::array kAvailableLights = {
        // id, ordinal, type
        HwLight{static_cast<int32_t>(LightType::NOTIFICATIONS), 0, LightType::NOTIFICATIONS},
        HwLight{static_cast<int32_t>(LightType::BATTERY), 0, LightType::BATTERY},
};

class Lights : public BnLights {
  public:
    ndk::ScopedAStatus setLightState(int id, const HwLightState& state) override;
    ndk::ScopedAStatus getLights(std::vector<HwLight>* types) override;

  private:
    std::array<HwLightState, kAvailableLights.size()> notif_states_;
};

}  // namespace light
}  // namespace hardware
}  // namespace android
}  // namespace aidl
