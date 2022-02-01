/*
 * Copyright (C) 2022 The LineageOS Project
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

#define LOG_TAG "TouchscreenGestureService"

#include <android-base/file.h>
#include <android-base/logging.h>

#include "TouchscreenGesture.h"

namespace {
struct GestureInfo {
    int32_t keycode;
    const char* name;
    const char* path;
};

constexpr GestureInfo kGestureNodes[] = {
        {250, "Single Tap", "/sys/class/touchscreen/NVT-ts/single_click"},
};
}  // anonymous namespace

namespace vendor {
namespace lineage {
namespace touch {
namespace V1_0 {
namespace implementation {

using ::android::hardware::Void;

Return<void> TouchscreenGesture::getSupportedGestures(getSupportedGestures_cb resultCb) {
    std::vector<Gesture> gestures;
    for (int32_t i = 0; i < std::size(kGestureNodes); ++i) {
        gestures.push_back({i, kGestureNodes[i].name, kGestureNodes[i].keycode});
    }
    resultCb(gestures);
    return Void();
}

Return<bool> TouchscreenGesture::setGestureEnabled(
        const ::vendor::lineage::touch::V1_0::Gesture& gesture, bool enabled) {
    if (gesture.id >= std::size(kGestureNodes)) {
        return false;
    }

    if (!android::base::WriteStringToFile(std::to_string(enabled),
                                          kGestureNodes[gesture.id].path)) {
        LOG(ERROR) << "Wrote file " << kGestureNodes[gesture.id].path << " failed";
        return false;
    }

    return true;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace touch
}  // namespace lineage
}  // namespace vendor
