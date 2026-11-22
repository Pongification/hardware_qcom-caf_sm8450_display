/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2017 The Android Open Source Project
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

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cutils/properties.h>

#include "QtiComposerClient.h"

namespace vendor {
namespace qti {
namespace hardware {
namespace display {
namespace composer {
namespace V3_1 {
namespace implementation {

namespace {
constexpr char kLtpoNode[] = "/sys/panel_feature/skip_frame_mode";
constexpr int kLtpoIndexNoSkip = 16;
constexpr int kLtpoIndexNone = -1;
constexpr int kLtpoIndex60 = 7;
constexpr int kLtpoIndex30 = 8;
constexpr int kLtpoIndex24 = 9;
constexpr int kLtpoIndexAuto10 = 1;  // panel-managed idle at 120 Hz, down to 10 Hz
constexpr int kLtpoIndexAuto1 = 2;   // panel-managed idle at 120 Hz, down to 1 Hz
constexpr int kLtpoIndex1Dim = 2;   // doze / AOD
constexpr int kLtpoMaxPendingPresents = 30;
constexpr uint32_t kLtpoConfig30 = 0xFFFC;
constexpr uint32_t kLtpoConfig24 = 0xFFFD;
constexpr uint32_t kLtpoEmulatedConfigs[] = {
    kLtpoConfig30,
    kLtpoConfig24,
};
}  // namespace

ComposerHandleImporter mHandleImporter;

BufferCacheEntry::BufferCacheEntry() : mHandle(nullptr) {}

BufferCacheEntry::BufferCacheEntry(BufferCacheEntry&& other) {
  mHandle = other.mHandle;
  other.mHandle = nullptr;
}

BufferCacheEntry& BufferCacheEntry::operator=(buffer_handle_t handle) {
  clear();
  mHandle = handle;
  return *this;
}

BufferCacheEntry::~BufferCacheEntry() {
  clear();
}

void BufferCacheEntry::clear() {
  if (mHandle) {
    mHandleImporter.freeBuffer(mHandle);
  }
}

QtiComposerClient::QtiComposerClient() : mWriter(kWriterInitialSize), mReader(*this) {
  hwc_session_ = HWCSession::GetInstance();
  mHandleImporter.initialize();

  char ltpo[PROPERTY_VALUE_MAX] = {};
  property_get("vendor.display.ltpo", ltpo, "1");
  char panel[PROPERTY_VALUE_MAX] = {};
  property_get("ro.vendor.display.panel.type", panel, "");
  mLtpo_supported_ =
      (strcmp(ltpo, "0") != 0) && (strcmp(panel, "LTPO_AMOLED") == 0);
  mLtpoAllow90_ = property_get_bool("vendor.display.ltpo.allow_90", true);
  mLtpoIndexAuto_ = property_get_bool("vendor.display.ltpo.idle_1hz", false) ? kLtpoIndexAuto1
                                                                               : kLtpoIndexAuto10;
  ALOGI("LTPO: panel feature %s (panel type '%s', 90Hz %s, idle %s)",
        mLtpo_supported_ ? "enabled" : "disabled", panel,
        mLtpoAllow90_ ? "allowed" : "hidden", mLtpoIndexAuto_ == kLtpoIndexAuto1 ? "1Hz" : "10Hz");
}

bool QtiComposerClient::LtpoIsPrimary(uint64_t display) const {
  return display == HWC_DISPLAY_PRIMARY;
}

bool QtiComposerClient::LtpoIsSyntheticConfig(uint32_t config) const {
  return config == kLtpoConfig30 || config == kLtpoConfig24;
}

int32_t QtiComposerClient::LtpoSyntheticPeriodNs(uint32_t config) const {
  switch (config) {
    case kLtpoConfig30:
      return 33333333;
    case kLtpoConfig24:
      return 41666667;
    default:
      return 0;
  }
}

int QtiComposerClient::LtpoFpsForPeriodNs(int64_t period_ns) const {
  if (period_ns <= 0) return 0;
  return static_cast<int>(1000000000LL / period_ns + 0.5);
}

int32_t QtiComposerClient::LtpoAttr(uint64_t display, uint32_t config,
                                    composer_V2_4::IComposerClient::Attribute attribute) {
  const int32_t synthetic_period_ns = LtpoSyntheticPeriodNs(config);
  if (synthetic_period_ns > 0) {
    if (attribute == composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD) {
      return synthetic_period_ns;
    }
    const uint32_t real = LtpoMaxFpsConfig(display);
    if (real == UINT32_MAX) return 0;
    config = real;
  }
  int32_t value = 0;
  if (hwc_session_->GetDisplayAttribute(display, config, attribute, &value) !=
      HWC2_ERROR_NONE) {
    return 0;
  }
  return value;
}

int QtiComposerClient::LtpoFpsForConfig(uint64_t display, uint32_t config) {
  const int32_t synthetic_period_ns = LtpoSyntheticPeriodNs(config);
  if (synthetic_period_ns > 0) return LtpoFpsForPeriodNs(synthetic_period_ns);
  const int32_t period_ns =
      LtpoAttr(display, config, composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD);
  return LtpoFpsForPeriodNs(period_ns);
}

int QtiComposerClient::LtpoIndexForFps(int fps) const {
  if (fps >= 80) return mLtpoIndexAuto_;
  if (fps >= 45) return kLtpoIndex60;
  if (fps >= 26) return kLtpoIndex30;
  if (fps >= 20) return kLtpoIndex24;
  return -1;
}

int QtiComposerClient::LtpoWrite(int index) const {
  if (index < 0) return -EINVAL;
  int fd = open(kLtpoNode, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    const int error = errno;
    ALOGW("LTPO: cannot open %s: %s", kLtpoNode, strerror(error));
    return -error;
  }
  char buf[8];
  const int len = snprintf(buf, sizeof(buf), "%d", index);
  const ssize_t written = write(fd, buf, len);
  const int error = written < 0 ? errno : EIO;
  close(fd);
  if (written != len) {
    ALOGW("LTPO: write index %d failed: %s", index, strerror(error));
    return -error;
  }
  return 0;
}

int QtiComposerClient::LtpoAppliedFps(uint64_t display) {
  VsyncPeriodNanos period_ns = 0;
  if (hwc_session_->GetDisplayVsyncPeriod(display, &period_ns) != HWC2_ERROR_NONE) {
    return 0;
  }
  return LtpoFpsForPeriodNs(period_ns);
}

uint32_t QtiComposerClient::LtpoVirtualConfig(uint64_t display) {
  {
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    auto it = mVirtualConfig.find(display);
    if (it != mVirtualConfig.end()) return it->second;
  }

  uint32_t config = 0;
  hwc_session_->GetActiveConfig(display, &config);
  const int32_t period_ns =
      LtpoAttr(display, config, composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD);
  std::lock_guard<std::mutex> lock(mLtpo_mutex_);
  auto it = mVirtualConfig.find(display);
  if (it != mVirtualConfig.end()) return it->second;
  mVirtualConfig[display] = config;
  mVirtualPeriodNs[display] = period_ns;
  return config;
}

int32_t QtiComposerClient::LtpoCachedPeriodNs(uint64_t display) {
  std::lock_guard<std::mutex> lock(mLtpo_mutex_);
  if (mPendingLtpo.count(display)) return 0;
  auto it = mVirtualPeriodNs.find(display);
  return it != mVirtualPeriodNs.end() ? it->second : 0;
}

void QtiComposerClient::LtpoSetActive(uint64_t display, uint32_t config, int32_t period_ns,
                                      int index) {
  std::lock_guard<std::mutex> lock(mLtpo_mutex_);
  mPendingLtpo.erase(display);
  mVirtualConfig[display] = config;
  mVirtualPeriodNs[display] = period_ns;
  mLtpoActiveIndex_ = index;
}

bool QtiComposerClient::LtpoScreenOn() {
  std::lock_guard<std::mutex> lock(mLtpo_mutex_);
  return mLtpoScreenOn_;
}

void QtiComposerClient::LtpoQueuePending(uint64_t display, const PendingLtpo &pending) {
  std::lock_guard<std::mutex> lock(mLtpo_mutex_);
  mPendingLtpo[display] = pending;
  mVirtualConfig[display] = pending.virtual_config;
}

void QtiComposerClient::LtpoOnPresent(uint64_t display) {
  if (!LtpoAvailable() || !LtpoIsPrimary(display)) return;
  {
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    if (!mPendingLtpo.count(display)) return;
  }
  LtpoProcessPending(display);
}

void QtiComposerClient::LtpoProcessPending(uint64_t display) {
  std::lock_guard<std::mutex> io_lock(mLtpo_io_mutex_);

  PendingLtpo pending = {};
  {
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    auto it = mPendingLtpo.find(display);
    if (it == mPendingLtpo.end()) return;
    pending = it->second;
  }

  auto rollback = [&](const char *why, int error) {
    ALOGE("LTPO: giving up on index %d (%s: %s)", pending.index, why, strerror(-error));
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    mPendingLtpo.erase(display);
    mVirtualConfig[display] = pending.previous_config;
    mVirtualPeriodNs[display] = pending.previous_period_ns;
  };
  auto store = [&]() {
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    mPendingLtpo[display] = pending;
  };

  pending.presents++;
  if (LtpoAppliedFps(display) != pending.real_fps) {
    pending.settled = 0;
    if (pending.presents > kLtpoMaxPendingPresents) {
      rollback("native mode never applied", -ETIMEDOUT);
    } else {
      store();
    }
    return;
  }

  if (++pending.settled < 2) {
    store();
    return;
  }

  if (!LtpoScreenOn()) {
    LtpoSetActive(display, pending.virtual_config, pending.virtual_period_ns, pending.index);
    return;
  }

  const int error = LtpoWrite(pending.index);
  if (error == -EINVAL && pending.presents <= kLtpoMaxPendingPresents) {
    store();
    return;
  }
  if (error) {
    rollback("write failed", error);
    return;
  }
  LtpoSetActive(display, pending.virtual_config, pending.virtual_period_ns, pending.index);
}

uint32_t QtiComposerClient::LtpoMaxFpsConfig(uint64_t display) {
  uint32_t count = 0;
  if (hwc_session_->GetDisplayConfigs(display, &count, nullptr) != HWC2_ERROR_NONE ||
      count == 0) {
    return UINT32_MAX;
  }
  std::vector<hwc2_config_t> configs(count);
  if (hwc_session_->GetDisplayConfigs(display, &count, configs.data()) !=
      HWC2_ERROR_NONE) {
    return UINT32_MAX;
  }
  uint32_t best = UINT32_MAX;
  int best_fps = 0;
  for (auto config : configs) {
    const int fps = LtpoFpsForConfig(display, config);
    if (fps > best_fps) {
      best_fps = fps;
      best = config;
    }
  }
  return best_fps >= 100 ? best : UINT32_MAX;
}

int32_t QtiComposerClient::LtpoSetPanelConfig(
    uint64_t display, uint32_t config,
    const VsyncPeriodChangeConstraints *vsync_period_change_constraints,
    VsyncPeriodChangeTimeline *timeline) {
  std::lock_guard<std::mutex> io_lock(mLtpo_io_mutex_);

  const int fps = LtpoFpsForConfig(display, config);
  if (fps <= 0) {
    return HWC2_ERROR_BAD_CONFIG;
  }
  if (!mLtpoAllow90_ && !LtpoIsSyntheticConfig(config) && fps > 60 && fps < 100) {
    return HWC2_ERROR_BAD_CONFIG;
  }
  const bool emulated = LtpoIsSyntheticConfig(config) || fps < 80;
  const uint32_t real = emulated ? LtpoMaxFpsConfig(display) : config;
  if (real == UINT32_MAX) {
    return HWC2_ERROR_BAD_CONFIG;
  }

  uint32_t current = 0;
  auto error = hwc_session_->GetActiveConfig(display, &current);
  if (error != HWC2_ERROR_NONE) {
    return error;
  }

  const uint32_t previous = LtpoVirtualConfig(display);
  const int32_t previous_period_ns =
      LtpoAttr(display, previous, composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD);
  const int32_t period_ns =
      LtpoAttr(display, config, composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD);
  const int real_fps = LtpoFpsForConfig(display, real);
  const bool native_low = !emulated && fps < 100;
  const int index = native_low ? kLtpoIndexNone : LtpoIndexForFps(fps);

  // Drop any skip state before landing on a real timing that rejects it.
  if (index == kLtpoIndexNone && LtpoScreenOn() && LtpoAppliedFps(display) >= 100) {
    LtpoWrite(kLtpoIndexNoSkip);
  }

  if (current != real) {
    error = vsync_period_change_constraints
        ? hwc_session_->SetActiveConfigWithConstraints(
              display, real, vsync_period_change_constraints, timeline)
        : hwc_session_->SetActiveConfig(display, real);
    if (error != HWC2_ERROR_NONE) {
      return error;
    }
  }

  if (index == kLtpoIndexNone || !LtpoScreenOn()) {
    LtpoSetActive(display, config, period_ns, index);
    return HWC2_ERROR_NONE;
  }

  // The kernel rejects skip_frame_mode at 60/90 Hz; defer until 120 Hz is applied.
  if (current != real || LtpoAppliedFps(display) != real_fps) {
    LtpoQueuePending(display, {real_fps, index, config, period_ns, previous,
                               previous_period_ns, 0, 0});
    return HWC2_ERROR_NONE;
  }

  const int write_error = LtpoWrite(index);
  if (write_error) {
    return HWC2_ERROR_NO_RESOURCES;
  }
  LtpoSetActive(display, config, period_ns, index);
  return HWC2_ERROR_NONE;
}

void QtiComposerClient::LtpoOnPowerMode(uint64_t display, int32_t mode) {
  if (!LtpoAvailable() || !LtpoIsPrimary(display)) return;
  std::lock_guard<std::mutex> io_lock(mLtpo_io_mutex_);

  using PowerMode = composer_V2_1::IComposerClient::PowerMode;
  const bool on = mode == static_cast<int32_t>(PowerMode::ON);
  {
    std::lock_guard<std::mutex> lock(mLtpo_mutex_);
    mPendingLtpo.erase(display);
    mLtpoScreenOn_ = on;
  }

  if (mode == static_cast<int32_t>(PowerMode::DOZE) ||
      mode == static_cast<int32_t>(PowerMode::DOZE_SUSPEND)) {
    if (LtpoAppliedFps(display) >= 100) {
      LtpoWrite(kLtpoIndex1Dim);
    }
  } else if (on) {
    uint32_t current = 0;
    if (hwc_session_->GetActiveConfig(display, &current) == HWC2_ERROR_NONE) {
      const int32_t period_ns =
          LtpoAttr(display, current,
                   composer_V2_4::IComposerClient::Attribute::VSYNC_PERIOD);
      if (LtpoAppliedFps(display) >= 100) {
        if (LtpoWrite(mLtpoIndexAuto_) == 0) {
          LtpoSetActive(display, current, period_ns, mLtpoIndexAuto_);
        }
      } else {
        LtpoSetActive(display, current, period_ns, kLtpoIndexNone);
      }
    }
  }
}

QtiComposerClient::~QtiComposerClient() {
  // We want to call hwc2_close here (and move hwc2_open to the
  // constructor), with the assumption that hwc2_close would
  //
  //  - clean up all resources owned by the client
  //  - make sure all displays are blank (since there is no layer)
  //
  // But since SF used to crash at this point, different hwcomposer2
  // implementations behave differently on hwc2_close.  Our only portable
  // choice really is to abort().  But that is not an option anymore
  // because we might also have VTS or VR as clients that can come and go.
  //
  // Below we manually clean all resources (layers and virtual
  // displays), and perform a presentDisplay afterwards.
  ALOGW("destroying composer client");

  enableCallback(false);

  // no need to grab the mutex as any in-flight hwbinder call would have
  // kept the client alive
  for (const auto& dpy : mDisplayData) {
    ALOGW("destroying client resources for display %" PRIu64, dpy.first);

    for (const auto& ly : dpy.second.Layers) {
      hwc_session_->DestroyLayer(dpy.first, ly.first);
    }

    if (dpy.second.IsVirtual) {
      destroyVirtualDisplay(dpy.first);
    } else {
      ALOGW("performing a final presentDisplay");

      mReader.validateDisplay();

      hwc_session_->AcceptDisplayChanges(dpy.first);

      shared_ptr<Fence> presentFence = nullptr;
      std::vector<Layer> releasedLayers;
      std::vector<shared_ptr<Fence>> releaseFences;
      mReader.presentDisplay(dpy.first, &presentFence, releasedLayers, releaseFences);
    }
  }

  mDisplayData.clear();

  mHandleImporter.cleanup();

  ALOGW("removed composer client");
}

void QtiComposerClient::onHotplug(hwc2_callback_data_t callbackData, hwc2_display_t display,
                                    int32_t connected) {
  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  auto connect = static_cast<composer_V2_4::IComposerCallback::Connection>(connected);
  if (connect == composer_V2_4::IComposerCallback::Connection::CONNECTED) {
    std::lock_guard<std::mutex> lock_d(client->mDisplayDataMutex);
    client->mDisplayData.emplace(display, DisplayData(false));
  }

  auto ret = client->callback_->onHotplug(display, connect);
  ALOGW_IF(!ret.isOk(), "failed to send onHotplug: %s. SF likely unavailable.",
           ret.description().c_str());

  if (connect == composer_V2_4::IComposerCallback::Connection::DISCONNECTED) {
    {
      std::lock_guard<std::mutex> lock(client->mLtpo_mutex_);
      client->mVirtualConfig.erase(display);
      client->mVirtualPeriodNs.erase(display);
      client->mPendingLtpo.erase(display);
    }
    // Trigger refresh to make sure disconnect event received/updated properly by SurfaceFlinger.
    client->hwc_session_->Refresh(HWC_DISPLAY_PRIMARY);
    // Wait for sufficient time to ensure sufficient resources are available to process connection.
    uint32_t vsync_period;
    client->hwc_session_->GetVsyncPeriod(HWC_DISPLAY_PRIMARY, &vsync_period);
    usleep(vsync_period * 2 / 1000);

    // Wait for the input command message queue to process before destroying the local display data.
    std::lock_guard<std::mutex> lock(client->mCommandMutex);
    std::lock_guard<std::mutex> lock_d(client->mDisplayDataMutex);
    client->mDisplayData.erase(display);
  }
}

void QtiComposerClient::onRefresh(hwc2_callback_data_t callbackData, hwc2_display_t display) {
  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  auto ret = client->callback_->onRefresh(display);
  ALOGW_IF(!ret.isOk(), "failed to send onRefresh: %s. SF likely unavailable.",
           ret.description().c_str());
}

void QtiComposerClient::onVsync(hwc2_callback_data_t callbackData, hwc2_display_t display,
                                int64_t timestamp) {
  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  auto ret = client->callback_->onVsync(display, timestamp);
  ALOGW_IF(!ret.isOk(), "failed to send onVsync: %s. SF likely unavailable.",
           ret.description().c_str());
}

void QtiComposerClient::onVsync_2_4(hwc2_callback_data_t callbackData, hwc2_display_t display,
                                    int64_t timestamp, VsyncPeriodNanos vsyncPeriodNanos) {
  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  if (client && client->LtpoAvailable() && client->LtpoIsPrimary(display)) {
    const int32_t period_ns = client->LtpoCachedPeriodNs(display);
    if (period_ns > 0) vsyncPeriodNanos = period_ns;
  }
  auto ret = client->callback24_->onVsync_2_4(display, timestamp, vsyncPeriodNanos);
  ALOGW_IF(!ret.isOk(), "failed to send onVsync_2_4: %s. SF likely unavailable.",
           ret.description().c_str());
}

void QtiComposerClient::onVsyncPeriodTimingChanged(hwc2_callback_data_t callbackData,
      hwc2_display_t display, hwc_vsync_period_change_timeline_t *updatedTimeline) {
   VsyncPeriodChangeTimeline timeline =
                                   {updatedTimeline->newVsyncAppliedTimeNanos,
                                    static_cast<bool>(updatedTimeline->refreshRequired),
                                    updatedTimeline->refreshTimeNanos};

  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  auto ret = client->callback24_->onVsyncPeriodTimingChanged(display, timeline);
  ALOGW_IF(!ret.isOk(), "failed to send onVsyncPeriodTimingChanged: %s. SF likely unavailable.",
          ret.description().c_str());
}

void QtiComposerClient::onSeamlessPossible(hwc2_callback_data_t callbackData,
                                           hwc2_display_t display) {
  auto client = reinterpret_cast<QtiComposerClient*>(callbackData);
  auto ret = client->callback24_->onSeamlessPossible(display);
  ALOGW_IF(!ret.isOk(), "failed to send onSeamlessPossible: %s. SF likely unavailable.",
           ret.description().c_str());
}

// convert fenceFd to or from hidl_handle
// Handle would still own original fence. Hence create a Fence object on duped fd.
Error QtiComposerClient::getFence(const hidl_handle& fenceHandle, shared_ptr<Fence>* outFence,
                                  const string& name) {
  auto handle = fenceHandle.getNativeHandle();
  if (handle && handle->numFds > 1) {
    QTI_LOGE("Invalid fence handle with %d fds", handle->numFds);
    return Error::BAD_PARAMETER;
  }

  int fenceFd = (handle && handle->numFds == 1) ? handle->data[0] : -1;
  *outFence = Fence::Create(dup(fenceFd), name);

  return Error::NONE;
}

// Handle would own fence hereafter. Hence provide a dupped fd.
hidl_handle QtiComposerClient::getFenceHandle(const shared_ptr<Fence>& fence,
                                              char* handleStorage) {
  native_handle_t* handle = nullptr;
  if (fence) {
    handle = native_handle_init(handleStorage, 1, 0);
    if (handle) {
      handle->data[0] = Fence::Dup(fence);
    }
  }

  return hidl_handle(handle);
}

Error QtiComposerClient::getDisplayReadbackBuffer(Display display,
                                                  const native_handle_t* rawHandle,
                                                  const native_handle_t** outHandle) {
  // TODO(user): revisit for caching and freeBuffer in success case.
  if (!mHandleImporter.importBuffer(rawHandle)) {
    QTI_LOGE("ImportBuffer failed: ");
    return Error::NO_RESOURCES;
  }

  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  auto iter = mDisplayData.find(display);
  if (iter == mDisplayData.end()) {
    mHandleImporter.freeBuffer(rawHandle);
    return Error::BAD_DISPLAY;
  }

  *outHandle = rawHandle;
  return Error::NONE;
}

void QtiComposerClient::getCapabilities() {
  uint32_t count = 0;
  hwc_session_->GetCapabilities(&count, nullptr);

  std::vector<int32_t> composer_caps(count);
  hwc_session_->GetCapabilities(&count, composer_caps.data());
  composer_caps.resize(count);

  mCapabilities.reserve(count);
  for (auto cap : composer_caps) {
    mCapabilities.insert(static_cast<hwc2_capability_t>(cap));
  }
}

void QtiComposerClient::enableCallback(bool enable) {
  if (enable) {
    hwc_session_->RegisterCallback(HWC2_CALLBACK_HOTPLUG, this,
                                   reinterpret_cast<hwc2_function_pointer_t>(onHotplug));
    hwc_session_->RegisterCallback(HWC2_CALLBACK_REFRESH, this,
                                   reinterpret_cast<hwc2_function_pointer_t>(onRefresh));
    if (!mUseCallback24_) {
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC, this,
                                     reinterpret_cast<hwc2_function_pointer_t>(onVsync));
    } else {
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC_2_4, this,
                                     reinterpret_cast<hwc2_function_pointer_t>(onVsync_2_4));
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC_PERIOD_TIMING_CHANGED, this,
                             reinterpret_cast<hwc2_function_pointer_t>(onVsyncPeriodTimingChanged));
      hwc_session_->RegisterCallback(HWC2_CALLBACK_SEAMLESS_POSSIBLE, this,
                                     reinterpret_cast<hwc2_function_pointer_t>(onSeamlessPossible));
    }
  } else {
    hwc_session_->RegisterCallback(HWC2_CALLBACK_HOTPLUG, this, nullptr);
    hwc_session_->RegisterCallback(HWC2_CALLBACK_REFRESH, this, nullptr);
    if (!mUseCallback24_) {
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC, this, nullptr);
    } else {
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC_2_4, this, nullptr);
      hwc_session_->RegisterCallback(HWC2_CALLBACK_VSYNC_PERIOD_TIMING_CHANGED, this, nullptr);
      hwc_session_->RegisterCallback(HWC2_CALLBACK_SEAMLESS_POSSIBLE, this, nullptr);
    }
  }
}

// Methods from ::android::hardware::graphics::composer::V2_1::IComposerClient follow.
Return<void> QtiComposerClient::registerCallback(
                                            const sp<composer_V2_1::IComposerCallback>& callback) {
  callback_ = callback;
  enableCallback(callback != nullptr);
  return Void();
}

Return<uint32_t> QtiComposerClient::getMaxVirtualDisplayCount() {
  return hwc_session_->GetMaxVirtualDisplayCount();
}

Return<void> QtiComposerClient::createVirtualDisplay(uint32_t width, uint32_t height,
                                                     common_V1_0::PixelFormat formatHint,
                                                     uint32_t outputBufferSlotCount,
                                                     createVirtualDisplay_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t createVirtualDisplay_2_2
  int32_t format = static_cast<int32_t>(formatHint);
  uint64_t display;
  auto error = hwc_session_->CreateVirtualDisplay(width, height, &format, &display);

  if (static_cast<Error>(error) == Error::NONE) {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);

    auto dpy = mDisplayData.emplace(static_cast<Display>(display), DisplayData(true)).first;
    dpy->second.OutputBuffers.resize(outputBufferSlotCount);
  }

  _hidl_cb(static_cast<Error>(error), display, static_cast<common_V1_0::PixelFormat>(format));
  return Void();
}

Return<composer_V2_1::Error> QtiComposerClient::destroyVirtualDisplay(uint64_t display) {
  auto error = hwc_session_->DestroyVirtualDisplay(display);
  if (static_cast<Error>(error) == Error::NONE) {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);

    mDisplayData.erase(display);
  }

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::createLayer(uint64_t display, uint32_t bufferSlotCount,
                                            createLayer_cb _hidl_cb) {
  composer_V2_1::Layer layer = 0;
  auto error = hwc_session_->CreateLayer(display, &layer);
  Error err = static_cast<Error>(error);
  if (err == Error::NONE) {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);
    auto dpy = mDisplayData.find(display);
    // The display entry may have already been removed by onHotplug.
    if (dpy != mDisplayData.end()) {
      auto ly = dpy->second.Layers.emplace(layer, LayerBuffers()).first;
      ly->second.Buffers.resize(bufferSlotCount);
    } else {
      err = Error::BAD_DISPLAY;
      // Note: We do not destroy the layer on this error as the hotplug
      // disconnect invalidates the display id. The implementation should
      // ensure all layers for the display are destroyed.
    }
  }

  _hidl_cb(err, layer);
  return Void();
}

Return<Error> QtiComposerClient::destroyLayer(uint64_t display, uint64_t layer) {
  auto error = hwc_session_->DestroyLayer(display, layer);
  Error err = static_cast<Error>(error);
  if (err == Error::NONE) {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);

    auto dpy = mDisplayData.find(display);
    // The display entry may have already been removed by onHotplug.
    if (dpy != mDisplayData.end()) {
      dpy->second.Layers.erase(layer);
    }
  }

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getActiveConfig(uint64_t display, getActiveConfig_cb _hidl_cb) {
  uint32_t config = 0;
  auto error = hwc_session_->GetActiveConfig(display, &config);

  if (error == HWC2_ERROR_NONE && LtpoAvailable() && LtpoIsPrimary(display)) {
    config = LtpoVirtualConfig(display);
  }

  _hidl_cb(static_cast<Error>(error), config);

  return Void();
}

Return<Error> QtiComposerClient::getClientTargetSupport(uint64_t display, uint32_t width,
                                                        uint32_t height,
                                                        common_V1_0::PixelFormat format,
                                                        common_V1_0::Dataspace dataspace) {
  auto error = hwc_session_->GetClientTargetSupport(display, width, height,
                                                    static_cast<int32_t>(format),
                                                    static_cast<int32_t>(dataspace));

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getColorModes(uint64_t display, getColorModes_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getColorModes_2_3
  hidl_vec<common_V1_0::ColorMode> modes;
  uint32_t count = 0;

  auto error = hwc_session_->GetColorModes(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), modes);
    return Void();
  }

  modes.resize(count);
  error = hwc_session_->GetColorModes(display, &count,
              reinterpret_cast<std::underlying_type<common_V1_0::ColorMode>::type*>(modes.data()));

  _hidl_cb(static_cast<Error>(error), modes);
  return Void();
}

Return<void> QtiComposerClient::getDisplayAttribute(uint64_t display, uint32_t config,
                                               composer_V2_1::IComposerClient::Attribute attribute,
                                               getDisplayAttribute_cb _hidl_cb) {
  if (LtpoAvailable() && LtpoIsPrimary(display) && LtpoIsSyntheticConfig(config)) {
    _hidl_cb(Error::NONE,
             LtpoAttr(display, config,
                      static_cast<composer_V2_4::IComposerClient::Attribute>(attribute)));
    return Void();
  }
  int32_t value = 0;
  auto error = hwc_session_->GetDisplayAttribute(
      display, config, static_cast<composer_V2_4::IComposerClient::Attribute>(attribute), &value);

  _hidl_cb(static_cast<Error>(error), value);
  return Void();
}

Return<void> QtiComposerClient::getDisplayConfigs(uint64_t display,
                                                  getDisplayConfigs_cb _hidl_cb) {
  hidl_vec<uint32_t> configs;
  uint32_t count = 0;

  auto error = hwc_session_->GetDisplayConfigs(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), configs);
    return Void();
  }

  configs.resize(count);
  error = hwc_session_->GetDisplayConfigs(display, &count, configs.data());

  if (error == HWC2_ERROR_NONE && LtpoAvailable() && LtpoIsPrimary(display)) {
    size_t out = 0;
    for (size_t i = 0; i < configs.size(); i++) {
      const int fps = LtpoFpsForConfig(display, configs[i]);
      if (!mLtpoAllow90_ && fps > 60 && fps < 100) continue;
      configs[out++] = configs[i];
    }
    if (LtpoMaxFpsConfig(display) != UINT32_MAX) {
      configs.resize(out + sizeof(kLtpoEmulatedConfigs) / sizeof(kLtpoEmulatedConfigs[0]));
      for (uint32_t config : kLtpoEmulatedConfigs) {
        configs[out++] = config;
      }
    } else {
      configs.resize(out);
    }
  }

  _hidl_cb(static_cast<Error>(error), configs);
  return Void();
}

Return<void> QtiComposerClient::getDisplayName(uint64_t display, getDisplayName_cb _hidl_cb) {
  uint32_t count = 0;
  hidl_string name_reply;
  std::vector<char> name;

  auto error = hwc_session_->GetDisplayName(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), name_reply);
    return Void();
  }

  name.resize(count + 1);
  error = hwc_session_->GetDisplayName(display, &count, name.data());
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), name_reply);
    return Void();
  }

  name.resize(count + 1);
  name[count] = '\0';
  name_reply.setToExternal(name.data(), count);

  _hidl_cb(static_cast<Error>(error), name_reply);
  return Void();
}

Return<void> QtiComposerClient::getDisplayType(uint64_t display, getDisplayType_cb _hidl_cb) {
  int32_t hwc_type;
  auto error = hwc_session_->GetDisplayType(display, &hwc_type);

  _hidl_cb(static_cast<Error>(error), static_cast<IComposerClient::DisplayType>(hwc_type));
  return Void();
}

Return<void> QtiComposerClient::getDozeSupport(uint64_t display, getDozeSupport_cb _hidl_cb) {
  int32_t hwc_support = 0;

  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    _hidl_cb(Error::BAD_DISPLAY, hwc_support);
    return Void();
  }

  auto error = hwc_session_->GetDozeSupport(display, &hwc_support);

  _hidl_cb(static_cast<Error>(error), hwc_support);
  return Void();
}

Return<void> QtiComposerClient::getHdrCapabilities(uint64_t display,
                                                   getHdrCapabilities_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getHdrCapabilities_2_3
  uint32_t count = 0;
  hidl_vec<common_V1_0::Hdr> types;
  float max_lumi = 0.0f;
  float max_avg_lumi = 0.0f;
  float min_lumi = 0.0f;

  auto error = hwc_session_->GetHdrCapabilities(display, &count, nullptr, &max_lumi,
                                                &max_avg_lumi, &min_lumi);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), types, max_lumi, max_avg_lumi, min_lumi);
    return Void();
  }

  types.resize(count);
  error = hwc_session_->GetHdrCapabilities(display, &count,
           reinterpret_cast<std::underlying_type<common_V1_2::Hdr>::type*>(types.data()),
           &max_lumi, &max_avg_lumi, &min_lumi);

  _hidl_cb(static_cast<Error>(error), types, max_lumi, max_avg_lumi, min_lumi);
  return Void();
}

Return<Error> QtiComposerClient::setClientTargetSlotCount(uint64_t display,
                                                          uint32_t clientTargetSlotCount) {
  std::lock_guard<std::mutex> lock(mDisplayDataMutex);

  auto dpy = mDisplayData.find(display);
  if (dpy == mDisplayData.end()) {
    return Error::BAD_DISPLAY;
  }
  dpy->second.ClientTargets.resize(clientTargetSlotCount);

  return Error::NONE;
}

Return<Error> QtiComposerClient::setActiveConfig(uint64_t display, uint32_t config) {
  if (LtpoAvailable() && LtpoIsPrimary(display)) {
    return static_cast<Error>(LtpoSetPanelConfig(display, config, nullptr, nullptr));
  }

  auto error = hwc_session_->SetActiveConfig(display, config);

  return static_cast<Error>(error);
}

Return<Error> QtiComposerClient::setColorMode(uint64_t display, common_V1_0::ColorMode mode) {
  auto error = hwc_session_->SetColorMode(display, static_cast<int32_t>(mode));

  return static_cast<Error>(error);
}

Return<Error> QtiComposerClient::setPowerMode(uint64_t display,
                                              composer_V2_1::IComposerClient::PowerMode mode) {
  // TODO(user): Implement combinedly w.r.t setPowerMode_2_2
  auto error = hwc_session_->SetPowerMode(display, static_cast<int32_t>(mode));

  if (error == HWC2_ERROR_NONE) {
    LtpoOnPowerMode(display, static_cast<int32_t>(mode));
  }

  return static_cast<Error>(error);
}

Return<Error> QtiComposerClient::setVsyncEnabled(uint64_t display,
                                                 composer_V2_1::IComposerClient::Vsync enabled) {
  auto error = hwc_session_->SetVsyncEnabled(display, static_cast<int32_t>(enabled));

  return static_cast<Error>(error);
}

Return<Error> QtiComposerClient::setInputCommandQueue(
                                                    const MQDescriptorSync<uint32_t>& descriptor) {
  std::lock_guard<std::mutex> lock(mCommandMutex);
  return mReader.setMQDescriptor(descriptor) ? Error::NONE : Error::NO_RESOURCES;
}

Return<void> QtiComposerClient::getOutputCommandQueue(getOutputCommandQueue_cb _hidl_cb) {
  // no locking as we require this function to be called inside
  // executeCommands_cb

  auto outDescriptor = mWriter.getMQDescriptor();
  if (outDescriptor) {
    _hidl_cb(Error::NONE, *outDescriptor);
  } else {
    _hidl_cb(Error::NO_RESOURCES, MQDescriptorSync<uint32_t>());
  }

  return Void();
}

Return<void> QtiComposerClient::executeCommands(uint32_t inLength,
                                                const hidl_vec<hidl_handle>& inHandles,
                                                executeCommands_cb _hidl_cb) {
  std::lock_guard<std::mutex> lock(mCommandMutex);

  std::lock_guard<std::mutex> hwc_lock(hwc_session_->command_seq_mutex_);

  bool outChanged = false;
  uint32_t outLength = 0;
  hidl_vec<hidl_handle> outHandles;

  if (!mReader.readQueue(inLength, inHandles)) {
    _hidl_cb(Error::BAD_PARAMETER, outChanged, outLength, outHandles);
    return Void();
  }

  Error err = mReader.parse();
  if (err == Error::NONE &&
      !mWriter.writeQueue(outChanged, outLength, outHandles)) {
    err = Error::NO_RESOURCES;
  }

  _hidl_cb(Error::NONE, outChanged, outLength, outHandles);

  mReader.reset();
  mWriter.reset();

  return Void();
}


// Methods from ::android::hardware::graphics::composer::V2_2::IComposerClient follow.
Return<void> QtiComposerClient::getPerFrameMetadataKeys(uint64_t display,
                                                        getPerFrameMetadataKeys_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getPerFrameMetadataKeys_2_3
  std::vector<PerFrameMetadataKey_V2> keys;
  uint32_t count = 0;

  auto error = hwc_session_->GetPerFrameMetadataKeys(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), keys);
    return Void();
  }

  keys.resize(count);
  error = hwc_session_->GetPerFrameMetadataKeys(display, &count,
               reinterpret_cast<std::underlying_type<PerFrameMetadataKey_V2>::type*>(keys.data()));

  _hidl_cb(static_cast<Error>(error), keys);
  return Void();
}

Return<void> QtiComposerClient::getReadbackBufferAttributes(uint64_t display,
                                                         getReadbackBufferAttributes_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getReadbackBufferAttributes_2_3
  int32_t format = 0;
  int32_t dataspace = 0;

  auto error = hwc_session_->GetReadbackBufferAttributes(display, &format, &dataspace);

  if (error != HWC2_ERROR_NONE) {
    format = 0;
    dataspace = 0;
  }

  _hidl_cb(static_cast<Error>(error), static_cast<common_V1_1::PixelFormat>(format),
           static_cast<common_V1_1::Dataspace>(dataspace));
  return Void();
}

Return<void> QtiComposerClient::getReadbackBufferFence(uint64_t display,
                                                       getReadbackBufferFence_cb _hidl_cb) {
  shared_ptr<Fence> fence = nullptr;
  auto error = hwc_session_->GetReadbackBufferFence(display, &fence);
  if (static_cast<Error>(error) != Error::NONE) {
    _hidl_cb(static_cast<Error>(error), nullptr);
    return Void();
  }

  NATIVE_HANDLE_DECLARE_STORAGE(fenceStorage, 1, 0);

  _hidl_cb(static_cast<Error>(error), getFenceHandle(fence, fenceStorage));
  return Void();
}

Return<Error> QtiComposerClient::setReadbackBuffer(uint64_t display, const hidl_handle& buffer,
                                                   const hidl_handle& releaseFence) {
  shared_ptr<Fence> fence = nullptr;
  Error error = getFence(releaseFence, &fence, "read_back");
  if (error != Error::NONE) {
    return error;
  }

  {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);
    if (mDisplayData.find(display) == mDisplayData.end()) {
      return Error::BAD_DISPLAY;
    }
  }

  const native_handle_t* readbackBuffer;
  getDisplayReadbackBuffer(display, buffer.getNativeHandle(), &readbackBuffer);
  if (error != Error::NONE) {
    return error;
  }

  auto err = hwc_session_->SetReadbackBuffer(display, readbackBuffer, fence);
  return static_cast<Error>(err);
}

Return<void> QtiComposerClient::createVirtualDisplay_2_2(uint32_t width, uint32_t height,
                                                         common_V1_1::PixelFormat formatHint,
                                                         uint32_t outputBufferSlotCount,
                                                         createVirtualDisplay_2_2_cb _hidl_cb) {
  int32_t format = static_cast<int32_t>(formatHint);
  uint64_t display;
  auto error = hwc_session_->CreateVirtualDisplay(width, height, &format, &display);

  if (static_cast<Error>(error) == Error::NONE) {
    std::lock_guard<std::mutex> lock(mDisplayDataMutex);

    auto dpy = mDisplayData.emplace(static_cast<Display>(display), DisplayData(true)).first;
    dpy->second.OutputBuffers.resize(outputBufferSlotCount);
  }

  _hidl_cb(static_cast<Error>(error), display, static_cast<common_V1_1::PixelFormat>(format));
  return Void();
}

Return<Error> QtiComposerClient::getClientTargetSupport_2_2(uint64_t display, uint32_t width,
                                                            uint32_t height,
                                                            common_V1_1::PixelFormat format,
                                                            common_V1_1::Dataspace dataspace) {
  auto error = hwc_session_->GetClientTargetSupport(display, width, height,
                                                    static_cast<int32_t>(format),
                                                    static_cast<int32_t>(dataspace));

  return static_cast<Error>(error);
}

Return<Error> QtiComposerClient::setPowerMode_2_2(uint64_t display,
                                                  composer_V2_2::IComposerClient::PowerMode mode) {
  if (mode == IComposerClient::PowerMode::ON_SUSPEND) {
    return Error::UNSUPPORTED;
  }
  auto error = hwc_session_->SetPowerMode(display, static_cast<int32_t>(mode));

  if (error == HWC2_ERROR_NONE) {
    LtpoOnPowerMode(display, static_cast<int32_t>(mode));
  }

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getColorModes_2_2(uint64_t display,
                                                  getColorModes_2_2_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getColorModes_2_3
  hidl_vec<common_V1_1::ColorMode> modes;
  uint32_t count = 0;

  auto error = hwc_session_->GetColorModes(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), modes);
    return Void();
  }

  modes.resize(count);
  error = hwc_session_->GetColorModes(display, &count,
              reinterpret_cast<std::underlying_type<common_V1_1::ColorMode>::type*>(modes.data()));

  _hidl_cb(static_cast<Error>(error), modes);
  return Void();
}

Return<void> QtiComposerClient::getRenderIntents(uint64_t display, common_V1_1::ColorMode mode,
                                                 getRenderIntents_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t getRenderIntents_2_3
  uint32_t count = 0;
  std::vector<RenderIntent> intents;

  auto error = hwc_session_->GetRenderIntents(display, int32_t(mode), &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), intents);
    return Void();
  }

  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    _hidl_cb(Error::BAD_DISPLAY, intents);
    return Void();
  }

  intents.resize(count);
  error = hwc_session_->GetRenderIntents(display, int32_t(mode), &count,
  reinterpret_cast<std::underlying_type<RenderIntent>::type*>(intents.data()));

  _hidl_cb(static_cast<Error>(error), intents);
  return Void();
}

Return<Error> QtiComposerClient::setColorMode_2_2(uint64_t display, common_V1_1::ColorMode mode,
                                                  common_V1_1::RenderIntent intent) {
  auto error = hwc_session_->SetColorModeWithRenderIntent(display, static_cast<int32_t>(mode),
                                                          static_cast<int32_t>(intent));

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getDataspaceSaturationMatrix(common_V1_1::Dataspace dataspace,
                                                        getDataspaceSaturationMatrix_cb _hidl_cb) {
  if (dataspace != common_V1_1::Dataspace::SRGB_LINEAR) {
    _hidl_cb(Error::BAD_PARAMETER, std::array<float, 16>{0.0f}.data());
    return Void();
  }

  std::array<float, 16> matrix;
  int32_t error = HWC2_ERROR_UNSUPPORTED;
  error = hwc_session_->GetDataspaceSaturationMatrix(static_cast<int32_t>(dataspace),
                                                     matrix.data());
  if (error != HWC2_ERROR_NONE) {
    matrix = {
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
  }
  _hidl_cb(Error::NONE, matrix.data());
  return Void();
}

Return<void> QtiComposerClient::executeCommands_2_2(uint32_t inLength,
                                                    const hidl_vec<hidl_handle>& inHandles,
                                                    executeCommands_2_2_cb _hidl_cb) {
  std::lock_guard<std::mutex> lock(mCommandMutex);

  // This lock ensures that Client gets exclusive access to hwc display.
  // Failing which return to SF will be blocked leading to fence timeouts.
  std::lock_guard<std::mutex> hwc_lock(hwc_session_->command_seq_mutex_);
  bool outChanged = false;
  uint32_t outLength = 0;
  hidl_vec<hidl_handle> outHandles;

  if (!mReader.readQueue(inLength, inHandles)) {
    _hidl_cb(Error::BAD_PARAMETER, outChanged, outLength, outHandles);
    return Void();
  }

  Error err = mReader.parse();
  if (err == Error::NONE &&
      !mWriter.writeQueue(outChanged, outLength, outHandles)) {
      err = Error::NO_RESOURCES;
  }

  _hidl_cb(Error::NONE, outChanged, outLength, outHandles);

  mReader.reset();
  mWriter.reset();

  return Void();
}


// Methods from ::android::hardware::graphics::composer::V2_3::IComposerClient follow.
Return<void> QtiComposerClient::getDisplayIdentificationData(uint64_t display,
                                                        getDisplayIdentificationData_cb _hidl_cb) {
  uint8_t port = 0;
  uint32_t size = 0;
  std::vector<uint8_t> data(size);

  auto error = hwc_session_->GetDisplayIdentificationData(display, &port, &size, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), port, data);
    return Void();
  }

  data.resize(size);
  error = hwc_session_->GetDisplayIdentificationData(display, &port, &size, data.data());

  _hidl_cb(static_cast<Error>(error), port, data);
  return Void();
}

Return<void> QtiComposerClient::getReadbackBufferAttributes_2_3(uint64_t display,
                                                     getReadbackBufferAttributes_2_3_cb _hidl_cb) {
  int32_t format = 0;
  int32_t dataspace = 0;

  auto error = hwc_session_->GetReadbackBufferAttributes(display, &format, &dataspace);

  if (error != HWC2_ERROR_NONE) {
    format = 0;
    dataspace = 0;
  }

  _hidl_cb(static_cast<Error>(error), static_cast<common_V1_2::PixelFormat>(format),
           static_cast<common_V1_2::Dataspace>(dataspace));
  return Void();
}

Return<Error> QtiComposerClient::getClientTargetSupport_2_3(uint64_t display, uint32_t width,
                                                            uint32_t height,
                                                            common_V1_2::PixelFormat format,
                                                            common_V1_2::Dataspace dataspace) {
  auto error = hwc_session_->GetClientTargetSupport(display, width, height,
                                                    static_cast<int32_t>(format),
                                                    static_cast<int32_t>(dataspace));

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getDisplayedContentSamplingAttributes(uint64_t display,
                                               getDisplayedContentSamplingAttributes_cb _hidl_cb) {
  // getDisplayedContentSamplingAttributes is not supported
  int constexpr invalid = -1;
  auto error = Error::UNSUPPORTED;
  common_V1_2::PixelFormat format = static_cast<common_V1_2::PixelFormat>(invalid);
  common_V1_2::Dataspace dataspace = static_cast<common_V1_2::Dataspace>(invalid);
  hidl_bitfield<IComposerClient::FormatColorComponent> componentMask =
    static_cast<hidl_bitfield<IComposerClient::FormatColorComponent>>(invalid);

  _hidl_cb(error, format, dataspace, componentMask);
  return Void();
}

Return<Error> QtiComposerClient::setDisplayedContentSamplingEnabled(uint64_t display,
                                   composer_V2_3::IComposerClient::DisplayedContentSampling enable,
                                   hidl_bitfield<FormatColorComponent> componentMask,
                                   uint64_t maxFrames) {
  // setDisplayedContentSamplingEnabled is not supported
  return Error::UNSUPPORTED;
}

Return<void> QtiComposerClient::getDisplayedContentSample(uint64_t display, uint64_t maxFrames,
                                                          uint64_t timestamp,
                                                          getDisplayedContentSample_cb _hidl_cb) {
  // getDisplayedContentSample is not supported
  auto error = Error::UNSUPPORTED;
  uint64_t frameCount = 0;
  hidl_vec<uint64_t> sampleComponent0 = 0;
  hidl_vec<uint64_t> sampleComponent1 = 0;
  hidl_vec<uint64_t> sampleComponent2 = 0;
  hidl_vec<uint64_t> sampleComponent3 = 0;

  _hidl_cb(error, frameCount, sampleComponent0, sampleComponent1, sampleComponent2,
           sampleComponent3);
  return Void();
}

Return<void> QtiComposerClient::executeCommands_2_3(uint32_t inLength,
                                                    const hidl_vec<hidl_handle>& inHandles,
                                                    executeCommands_2_3_cb _hidl_cb) {
  // TODO(user): Implement combinedly w.r.t executeCommands_2_2
  std::lock_guard<std::mutex> lock(mCommandMutex);

  std::lock_guard<std::mutex> hwc_lock(hwc_session_->command_seq_mutex_);
  bool outChanged = false;
  uint32_t outLength = 0;
  hidl_vec<hidl_handle> outHandles;

  if (!mReader.readQueue(inLength, inHandles)) {
    _hidl_cb(Error::BAD_PARAMETER, outChanged, outLength, outHandles);
    return Void();
  }

  Error err = mReader.parse();
  if (err == Error::NONE &&
      !mWriter.writeQueue(outChanged, outLength, outHandles)) {
      err = Error::NO_RESOURCES;
  }

  _hidl_cb(Error::NONE, outChanged, outLength, outHandles);

  mReader.reset();
  mWriter.reset();

  return Void();
}

Return<void> QtiComposerClient::getRenderIntents_2_3(uint64_t display, common_V1_2::ColorMode mode,
                                                     getRenderIntents_2_3_cb _hidl_cb) {
  uint32_t count = 0;
  std::vector<RenderIntent> intents;

  auto error = hwc_session_->GetRenderIntents(display, int32_t(mode), &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), intents);
    return Void();
  }

  intents.resize(count);
  error = hwc_session_->GetRenderIntents(display, int32_t(mode), &count,
  reinterpret_cast<std::underlying_type<RenderIntent>::type*>(intents.data()));

  _hidl_cb(static_cast<Error>(error), intents);
  return Void();
}

Return<void> QtiComposerClient::getColorModes_2_3(uint64_t display,
                                                  getColorModes_2_3_cb _hidl_cb) {
  hidl_vec<common_V1_2::ColorMode> modes;
  uint32_t count = 0;

  auto error = hwc_session_->GetColorModes(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), modes);
    return Void();
  }

  modes.resize(count);
  error = hwc_session_->GetColorModes(display, &count,
              reinterpret_cast<std::underlying_type<common_V1_2::ColorMode>::type*>(modes.data()));

  _hidl_cb(static_cast<Error>(error), modes);
  return Void();
}

Return<Error> QtiComposerClient::setColorMode_2_3(uint64_t display, common_V1_2::ColorMode mode,
                                                  common_V1_1::RenderIntent intent) {
  auto error = hwc_session_->SetColorModeWithRenderIntent(display, static_cast<int32_t>(mode),
                                                          static_cast<int32_t>(intent));

  return static_cast<Error>(error);
}

Return<void> QtiComposerClient::getDisplayCapabilities(uint64_t display,
                                                       getDisplayCapabilities_cb _hidl_cb) {
  // Report optional capabilities that we do support to pass VTS.

  hidl_vec<composer_V2_3::IComposerClient::DisplayCapability> capabilities;

  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
     _hidl_cb(Error::BAD_DISPLAY, capabilities);
    return Void();
  }

  HwcDisplayConnectionType display_conn_type = HwcDisplayConnectionType::INTERNAL;
  int32_t ret = hwc_session_->GetDisplayConnectionType(display, &display_conn_type);

  if (HWC2_ERROR_NONE != ret) {
     _hidl_cb(static_cast<Error>(ret), capabilities);
    return Void();
  }

  if (HwcDisplayConnectionType::INTERNAL == display_conn_type) {
    int32_t has_doze_support = 0;
    hwc_session_->GetDozeSupport(display, &has_doze_support);
    if (has_doze_support) {
      capabilities = { composer_V2_3::IComposerClient::DisplayCapability::DOZE,
                       composer_V2_3::IComposerClient::DisplayCapability::BRIGHTNESS };
    } else {
      capabilities = { composer_V2_3::IComposerClient::DisplayCapability::BRIGHTNESS };
    }
  }

  _hidl_cb(Error::NONE, capabilities);
  return Void();
}

Return<void> QtiComposerClient::getPerFrameMetadataKeys_2_3(uint64_t display,
                                                         getPerFrameMetadataKeys_2_3_cb _hidl_cb) {
  std::vector<PerFrameMetadataKey> keys;
  uint32_t count = 0;

  auto error = hwc_session_->GetPerFrameMetadataKeys(display, &count, nullptr);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), keys);
    return Void();
  }

  keys.resize(count);
  error = hwc_session_->GetPerFrameMetadataKeys(display, &count,
                  reinterpret_cast<std::underlying_type<PerFrameMetadataKey>::type*>(keys.data()));

  _hidl_cb(static_cast<Error>(error), keys);
  return Void();
}

Return<void> QtiComposerClient::getHdrCapabilities_2_3(uint64_t display,
                                                       getHdrCapabilities_2_3_cb _hidl_cb) {
  uint32_t count = 0;
  hidl_vec<common_V1_2::Hdr> types;
  float max_lumi = 0.0f;
  float max_avg_lumi = 0.0f;
  float min_lumi = 0.0f;

  auto error = hwc_session_->GetHdrCapabilities(display, &count, nullptr, &max_lumi,
                                                &max_avg_lumi, &min_lumi);
  if (error != HWC2_ERROR_NONE) {
    _hidl_cb(static_cast<Error>(error), types, max_lumi, max_avg_lumi, min_lumi);
    return Void();
  }

  types.resize(count);
  error = hwc_session_->GetHdrCapabilities(display, &count,
           reinterpret_cast<std::underlying_type<common_V1_2::Hdr>::type*>(types.data()),
           &max_lumi, &max_avg_lumi, &min_lumi);

  _hidl_cb(static_cast<Error>(error), types, max_lumi, max_avg_lumi, min_lumi);
  return Void();
}

Return<void> QtiComposerClient::getDisplayBrightnessSupport(uint64_t display,
                                                         getDisplayBrightnessSupport_cb _hidl_cb) {
  bool support = false;

  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    _hidl_cb(Error::BAD_DISPLAY, support);
    return Void();
  }

  auto error = hwc_session_->GetDisplayBrightnessSupport(display, &support);

  _hidl_cb(static_cast<Error>(error), support);
  return Void();
}

Return<Error> QtiComposerClient::setDisplayBrightness(uint64_t display, float brightness) {
  if (std::isnan(brightness) || brightness > 1.0f || (brightness < 0.0f && brightness != -1.0f)) {
    return Error::BAD_PARAMETER;
  }

  auto error = hwc_session_->SetDisplayBrightness(display, brightness);
  return static_cast<Error>(error);
}

// Methods from ::android::hardware::graphics::composer::V2_4::IComposerClient follow.
Return<void> QtiComposerClient::registerCallback_2_4(
    const sp<composer_V2_4::IComposerCallback> &callback) {
  callback_ = sp<composer_V2_1::IComposerCallback>(callback.get());
  callback24_ = callback;
  mUseCallback24_ = true;
  enableCallback(callback != nullptr);
  return Void();
}

Return<void> QtiComposerClient::getDisplayCapabilities_2_4(uint64_t display,
                                                           getDisplayCapabilities_2_4_cb _hidl_cb) {
  hidl_vec<HwcDisplayCapability> capabilities;
  auto error = hwc_session_->GetDisplayCapabilities(display, &capabilities);
  _hidl_cb(static_cast<composer_V2_4::Error>(error), capabilities);
  return Void();
}

Return<void> QtiComposerClient::getDisplayConnectionType(uint64_t display,
                                                         getDisplayConnectionType_cb _hidl_cb) {
  HwcDisplayConnectionType type;
  auto error = hwc_session_->GetDisplayConnectionType(display, &type);
  _hidl_cb(static_cast<composer_V2_4::Error>(error), type);
  return Void();
}

Return<void> QtiComposerClient::getDisplayAttribute_2_4(
    uint64_t display, uint32_t config, composer_V2_4::IComposerClient::Attribute attribute,
    getDisplayAttribute_2_4_cb _hidl_cb) {
  if (LtpoAvailable() && LtpoIsPrimary(display) && LtpoIsSyntheticConfig(config)) {
    _hidl_cb(composer_V2_4::Error::NONE, LtpoAttr(display, config, attribute));
    return Void();
  }
  int32_t value = 0;
  auto error = hwc_session_->GetDisplayAttribute(display, config, attribute, &value);
  _hidl_cb(static_cast<composer_V2_4::Error>(error), value);
  return Void();
}

Return<void> QtiComposerClient::getDisplayVsyncPeriod(uint64_t display,
                                                      getDisplayVsyncPeriod_cb _hidl_cb) {
  VsyncPeriodNanos vsync_period = 0;
  auto error = hwc_session_->GetDisplayVsyncPeriod(display, &vsync_period);
  if (error == HWC2_ERROR_NONE && LtpoAvailable() && LtpoIsPrimary(display)) {
    const int32_t period_ns = LtpoCachedPeriodNs(display);
    if (period_ns > 0) vsync_period = period_ns;
  }
  _hidl_cb(static_cast<composer_V2_4::Error>(error), vsync_period);
  return Void();
}

Return<void> QtiComposerClient::setActiveConfigWithConstraints(
    uint64_t display, uint32_t config,
    const VsyncPeriodChangeConstraints &vsyncPeriodChangeConstraints,
    setActiveConfigWithConstraints_cb _hidl_cb) {
  VsyncPeriodChangeTimeline timeline;
  timeline.newVsyncAppliedTimeNanos = systemTime();
  timeline.refreshRequired = false;
  timeline.refreshTimeNanos = 0;

  if (LtpoAvailable() && LtpoIsPrimary(display)) {
    auto error = LtpoSetPanelConfig(display, config, &vsyncPeriodChangeConstraints, &timeline);
    _hidl_cb(static_cast<composer_V2_4::Error>(error), timeline);
    return Void();
  }

  auto error = hwc_session_->SetActiveConfigWithConstraints(
      display, config, &vsyncPeriodChangeConstraints, &timeline);
  _hidl_cb(static_cast<composer_V2_4::Error>(error), timeline);
  return Void();
}

Return<composer_V2_4::Error> QtiComposerClient::setAutoLowLatencyMode(uint64_t display, bool on) {
  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    return composer_V2_4::Error::BAD_DISPLAY;
  }
  return composer_V2_4::Error::UNSUPPORTED;
}

Return<void> QtiComposerClient::getSupportedContentTypes(uint64_t display,
                                                         getSupportedContentTypes_cb _hidl_cb) {
  hidl_vec<composer_V2_4::IComposerClient::ContentType> types = {};
  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    _hidl_cb(composer_V2_4::Error::BAD_DISPLAY, types);
    return Void();
  }
  _hidl_cb(composer_V2_4::Error::NONE, types);
  return Void();
}

Return<composer_V2_4::Error> QtiComposerClient::setContentType(
    uint64_t display, composer_V2_4::IComposerClient::ContentType type) {
  std::lock_guard<std::mutex> lock(mDisplayDataMutex);
  if (mDisplayData.find(display) == mDisplayData.end()) {
    return composer_V2_4::Error::BAD_DISPLAY;
  }
  if (type == composer_V2_4::IComposerClient::ContentType::NONE) {
    return composer_V2_4::Error::NONE;
  }
  return composer_V2_4::Error::UNSUPPORTED;
}

Return<void> QtiComposerClient::getLayerGenericMetadataKeys(
    getLayerGenericMetadataKeys_cb _hidl_cb) {
  hidl_vec<composer_V2_4::IComposerClient::LayerGenericMetadataKey> keys = {};
  _hidl_cb(composer_V2_4::Error::NONE, keys);
  return Void();
}

Return<Error> QtiComposerClient::tryDrawMethod(uint64_t display,
    IQtiComposerClient::DrawMethod drawMethod) {
  auto error = hwc_session_->TryDrawMethod(display, drawMethod);
  return static_cast<Error>(error);
}

QtiComposerClient::CommandReader::CommandReader(QtiComposerClient& client)
  : mClient(client), mWriter(client.mWriter) {
}

bool QtiComposerClient::CommandReader::parseCommonCmd(
    IComposerClient::Command command, uint16_t length) {
  bool parsed = false;

  switch (command) {
  // Commands from ::android::hardware::graphics::composer::V2_1::IComposerClient follow.
  case IComposerClient::Command::SELECT_DISPLAY: {
    parsed = parseSelectDisplay(length);
    std::lock_guard<std::mutex> lock(mClient.mDisplayDataMutex);
    // Displays will not be removed while processing the command queue.
    if (parsed && mClient.mDisplayData.find(mDisplay) == mClient.mDisplayData.end()) {
      ALOGW("Command::SELECT_DISPLAY: Display %" PRId64 "not found. Dropping commands.", mDisplay);
      mDisplay = sdm::HWCCallbacks::kNumDisplays;
    }
    break;
  }
  case IComposerClient::Command::SELECT_LAYER:
    parsed = parseSelectLayer(length);
    break;
  case IComposerClient::Command::SET_COLOR_TRANSFORM:
    parsed = parseSetColorTransform(length);
    break;
  case IComposerClient::Command::SET_CLIENT_TARGET:
    parsed = parseSetClientTarget(length);
    break;
  case IComposerClient::Command::SET_OUTPUT_BUFFER:
    parsed = parseSetOutputBuffer(length);
    break;
  case IComposerClient::Command::VALIDATE_DISPLAY:
    parsed = parseValidateDisplay(length);
    break;
  case IComposerClient::Command::ACCEPT_DISPLAY_CHANGES:
    parsed = parseAcceptDisplayChanges(length);
    break;
  case IComposerClient::Command::PRESENT_DISPLAY:
    parsed = parsePresentDisplay(length);
    break;
  case IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY:
    parsed = parsePresentOrValidateDisplay(length);
    break;
  case IComposerClient::Command::SET_LAYER_CURSOR_POSITION:
    parsed = parseSetLayerCursorPosition(length);
    break;
  case IComposerClient::Command::SET_LAYER_BUFFER:
    parsed = parseSetLayerBuffer(length);
    break;
  case IComposerClient::Command::SET_LAYER_SURFACE_DAMAGE:
    parsed = parseSetLayerSurfaceDamage(length);
    break;
  case IComposerClient::Command::SET_LAYER_BLEND_MODE:
    parsed = parseSetLayerBlendMode(length);
    break;
  case IComposerClient::Command::SET_LAYER_COLOR:
    parsed = parseSetLayerColor(length);
    break;
  case IComposerClient::Command::SET_LAYER_COMPOSITION_TYPE:
    parsed = parseSetLayerCompositionType(length);
    break;
  case IComposerClient::Command::SET_LAYER_DATASPACE:
    parsed = parseSetLayerDataspace(length);
    break;
  case IComposerClient::Command::SET_LAYER_DISPLAY_FRAME:
    parsed = parseSetLayerDisplayFrame(length);
    break;
  case IComposerClient::Command::SET_LAYER_PLANE_ALPHA:
    parsed = parseSetLayerPlaneAlpha(length);
    break;
  case IComposerClient::Command::SET_LAYER_SIDEBAND_STREAM:
    parsed = parseSetLayerSidebandStream(length);
    break;
  case IComposerClient::Command::SET_LAYER_SOURCE_CROP:
    parsed = parseSetLayerSourceCrop(length);
    break;
  case IComposerClient::Command::SET_LAYER_TRANSFORM:
    parsed = parseSetLayerTransform(length);
    break;
  case IComposerClient::Command::SET_LAYER_VISIBLE_REGION:
    parsed = parseSetLayerVisibleRegion(length);
    break;
  case IComposerClient::Command::SET_LAYER_Z_ORDER:
    parsed = parseSetLayerZOrder(length);
    break;
  // Commands from ::android::hardware::graphics::composer::V2_2::IComposerClient follow.
  case IComposerClient::Command::SET_LAYER_PER_FRAME_METADATA:
    parsed = parseSetLayerPerFrameMetadata(length);
    break;
  case IComposerClient::Command::SET_LAYER_FLOAT_COLOR:
    parsed = parseSetLayerFloatColor(length);
    break;
  // Commands from ::android::hardware::graphics::composer::V2_3::IComposerClient follow.
  case IComposerClient::Command::SET_LAYER_COLOR_TRANSFORM:
    parsed = parseSetLayerColorTransform(length);
    break;
  case IComposerClient::Command::SET_LAYER_PER_FRAME_METADATA_BLOBS:
    parsed = parseSetLayerPerFrameMetadataBlobs(length);
    break;
  default:
    parsed = false;
    break;
  }

  return parsed;
}

Error QtiComposerClient::CommandReader::parse() {
  IQtiComposerClient::Command qticommand;
  uint16_t length;

  while (!isEmpty()) {
    if (!beginCommand(qticommand, length)) {
      break;
    }

    bool parsed = false;
    switch (qticommand) {
      case IQtiComposerClient::Command::SET_LAYER_TYPE:
        parsed = parseSetLayerType(length);
        break;
      case IQtiComposerClient::Command::SET_DISPLAY_ELAPSE_TIME:
        parsed = parseSetDisplayElapseTime(length);
        break;
      case IQtiComposerClient::Command::SET_CLIENT_TARGET_3_1:
        parsed = parseSetClientTarget_3_1(length);
        break;
      case IQtiComposerClient::Command::SET_LAYER_FLAG_3_1:
        parsed = parseSetLayerFlag(length);
        break;
      default:
        parsed = parseCommonCmd(static_cast<IComposerClient::Command>(qticommand), length);
        break;
    }

    endCommand();

    if (!parsed) {
      ALOGE("failed to parse command 0x%x, length %" PRIu16,
          qticommand, length);
      break;
    }
  }

  return (isEmpty()) ? Error::NONE : Error::BAD_PARAMETER;
}

bool QtiComposerClient::CommandReader::parseSelectDisplay(uint16_t length) {
  if (length != CommandWriter::kSelectDisplayLength) {
    return false;
  }

  mDisplay = read64();
  mWriter.selectDisplay(mDisplay);

  return true;
}

bool QtiComposerClient::CommandReader::parseSelectLayer(uint16_t length) {
  if (length != CommandWriter::kSelectLayerLength) {
    return false;
  }

  mLayer = read64();

  return true;
}

bool QtiComposerClient::CommandReader::parseSetColorTransform(uint16_t length) {
  if (length != CommandWriter::kSetColorTransformLength) {
    return false;
  }

  float matrix[16];
  for (int i = 0; i < 16; i++) {
    matrix[i] = readFloat();
  }
  auto transform = readSigned();

  auto err = mClient.hwc_session_->SetColorTransform(mDisplay, matrix, transform);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetClientTarget(uint16_t length) {
  // 4 parameters followed by N rectangles
  if ((length - 4) % 4 != 0) {
    return false;
  }

  bool useCache = false;
  auto slot = read();
  auto clientTarget = readHandle(useCache);
  shared_ptr<Fence> fence = nullptr;
  readFence(&fence, "fbt");
  auto dataspace = readSigned();
  auto damage = readRegion((length - 4) / 4);
  hwc_region region = {damage.size(), damage.data()};
  auto err = lookupBuffer(BufferCache::CLIENT_TARGETS, slot, useCache, clientTarget, &clientTarget);
  if (err == Error::NONE) {
    auto error = mClient.hwc_session_->SetClientTarget(mDisplay, clientTarget, fence,
        dataspace, region);
    err = static_cast<Error>(error);
    auto updateBufErr = updateBuffer(BufferCache::CLIENT_TARGETS, slot,
        useCache, clientTarget);
    if (err == Error::NONE) {
      err = updateBufErr;
    }
  }
  if (err != Error::NONE) {
    mWriter.setError(getCommandLoc(), err);
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetClientTarget_3_1(uint16_t length) {
  bool useCache = true;
  auto slot = read();
  buffer_handle_t clientTarget = nullptr;
  shared_ptr<Fence> fence = nullptr;
  readFence(&fence, "fbt");
  auto dataspace = readSigned();
  hwc_region region = {};
  auto err = lookupBuffer(BufferCache::CLIENT_TARGETS, slot, useCache, clientTarget, &clientTarget);
  if (err == Error::NONE) {
    auto error = mClient.hwc_session_->SetClientTarget_3_1(mDisplay, clientTarget, fence,
        dataspace, region);
    err = static_cast<Error>(error);
    auto updateBufErr = updateBuffer(BufferCache::CLIENT_TARGETS, slot,
        useCache, clientTarget);
    if (err == Error::NONE) {
      err = updateBufErr;
    }
  }
  if (err != Error::NONE) {
    mWriter.setError(getCommandLoc(), err);
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetOutputBuffer(uint16_t length) {
  if (length != CommandWriter::kSetOutputBufferLength) {
    return false;
  }

  bool useCache;
  auto slot = read();
  auto outputBuffer = readHandle(useCache);
  shared_ptr<Fence> fence = nullptr;
  readFence(&fence, "outbuf");
  auto err = lookupBuffer(BufferCache::OUTPUT_BUFFERS, slot, useCache, outputBuffer, &outputBuffer);
  if (err == Error::NONE) {
    auto error = mClient.hwc_session_->SetOutputBuffer(mDisplay, outputBuffer, fence);
    err = static_cast<Error>(error);
    auto updateBufErr = updateBuffer(BufferCache::OUTPUT_BUFFERS, slot, useCache, outputBuffer);
    if (err == Error::NONE) {
      err = updateBufErr;
    }
  }

  if (err != Error::NONE) {
    mWriter.setError(getCommandLoc(), err);
  }

  return true;
}

Error QtiComposerClient::CommandReader::validateDisplay() {
  bool validate_only = true;
  bool needsCommit = false;
  uint32_t types_count = 0;
  uint32_t reqs_count = 0;
  shared_ptr<Fence> presentFence = nullptr;

  auto err = mClient.hwc_session_->CommitOrPrepare(mDisplay, validate_only, &presentFence,
                                                   &types_count, &reqs_count, &needsCommit);
  auto status = INT32(err);
  if (status != HWC2_ERROR_NONE && status != HWC2_ERROR_HAS_CHANGES) {
    return static_cast<Error>(err);
  }

  return postValidateDisplay(types_count, reqs_count);
}

bool QtiComposerClient::CommandReader::parseValidateDisplay(uint16_t length) {
  if (length != CommandWriter::kValidateDisplayLength) {
    return false;
  }

  std::vector<Layer> changedLayers;
  std::vector<IComposerClient::Composition> compositionTypes;
  std::vector<Layer> requestedLayers;
  std::vector<uint32_t> requestMasks;

  auto err = validateDisplay();

  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseAcceptDisplayChanges(uint16_t length) {
  if (length != CommandWriter::kAcceptDisplayChangesLength) {
    return false;
  }

  auto err = mClient.hwc_session_->AcceptDisplayChanges(mDisplay);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

Error QtiComposerClient::CommandReader::presentDisplay(Display display,
                                                  shared_ptr<Fence>* presentFence,
                                                  std::vector<Layer>& layers,
                                                  std::vector<shared_ptr<Fence>>& releaseFences) {
  int32_t err = mClient.hwc_session_->PresentDisplay(display, presentFence);
  if (err != HWC2_ERROR_NONE) {
    return static_cast<Error>(err);
  }

  auto error = postPresentDisplay(presentFence);
  mClient.LtpoOnPresent(display);
  return error;
}

bool QtiComposerClient::CommandReader::parsePresentDisplay(uint16_t length) {
  if (length != CommandWriter::kPresentDisplayLength) {
    return false;
  }

  shared_ptr<Fence> presentFence = nullptr;
  std::vector<Layer> layers;
  std::vector<shared_ptr<Fence>> fences;

  auto err = presentDisplay(mDisplay, &presentFence, layers, fences);
  if (err != Error::NONE) {
    mWriter.setError(getCommandLoc(), err);
  }

  return true;
}

Error QtiComposerClient::CommandReader::postPresentDisplay(shared_ptr<Fence>* presentFence) {
  uint32_t count = 0;
  auto err = mClient.hwc_session_->GetReleaseFences(mDisplay, &count, nullptr, nullptr);
  if (err != HWC2_ERROR_NONE) {
    ALOGW("failed to get release fences");
    return Error::NONE;
  }

  std::vector<Layer> layers;
  std::vector<shared_ptr<Fence>> releaseFences;
  layers.resize(count);
  releaseFences.resize(count);
  err = mClient.hwc_session_->GetReleaseFences(mDisplay, &count, layers.data(), &releaseFences);
  if (err != HWC2_ERROR_NONE) {
    ALOGW("failed to get release fences");
    layers.clear();
    releaseFences.clear();
    return Error::NONE;
  }
  mWriter.setPresentFence(*presentFence);
  mWriter.setReleaseFences(layers, releaseFences);

  return Error::NONE;
}

Error QtiComposerClient::CommandReader::postValidateDisplay(uint32_t& types_count,
                                                            uint32_t& reqs_count) {
  std::vector<Layer> changedLayers;
  std::vector<IComposerClient::Composition> compositionTypes;
  std::vector<Layer> requestedLayers;
  std::vector<uint32_t> requestMasks;
  IComposerClient::ClientTargetProperty clientTargetProperty;
  changedLayers.resize(types_count);
  compositionTypes.resize(types_count);
  auto err = mClient.hwc_session_->GetChangedCompositionTypes(mDisplay, &types_count,
                                                              nullptr, nullptr);
  if (err != HWC2_ERROR_NONE) {
    return static_cast<Error>(err);
  }

  err = mClient.hwc_session_->GetChangedCompositionTypes(mDisplay, &types_count,
                        changedLayers.data(),
                        reinterpret_cast<std::underlying_type<IComposerClient::Composition>::type*>(
                        compositionTypes.data()));

  if (err != HWC2_ERROR_NONE) {
    changedLayers.clear();
    compositionTypes.clear();
    return static_cast<Error>(err);
  }

  int32_t display_reqs = 0;
  err = mClient.hwc_session_->GetDisplayRequests(mDisplay, &display_reqs, &reqs_count, nullptr,
                                                 nullptr);
  if (err != HWC2_ERROR_NONE) {
    changedLayers.clear();
    compositionTypes.clear();
    return static_cast<Error>(err);
  }

  requestedLayers.resize(reqs_count);
  requestMasks.resize(reqs_count);
  err = mClient.hwc_session_->GetDisplayRequests(mDisplay, &display_reqs, &reqs_count,
                                                 requestedLayers.data(),
                                                 reinterpret_cast<int32_t*>(requestMasks.data()));
  if (err != HWC2_ERROR_NONE) {
    changedLayers.clear();
    compositionTypes.clear();

    requestedLayers.clear();
    requestMasks.clear();
  }

  err = mClient.hwc_session_->GetClientTargetProperty(mDisplay, &clientTargetProperty);
  if (err != HWC2_ERROR_NONE) {
    // todo: reset to default values
    return static_cast<Error>(err);
  }

  mWriter.setChangedCompositionTypes(changedLayers, compositionTypes);
  mWriter.setDisplayRequests(display_reqs, requestedLayers, requestMasks);
  if (mClient.mUseCallback24_) {
    mWriter.setClientTargetProperty(clientTargetProperty);
  }

  return static_cast<Error>(err);
}

bool QtiComposerClient::CommandReader::parsePresentOrValidateDisplay(uint16_t length) {
  if (length != CommandWriter::kPresentOrValidateDisplayLength) {
     return false;
  }

  // Handle unified commit.
  bool needsCommit = false;
  shared_ptr<Fence> presentFence = nullptr;
  uint32_t typesCount = 0;
  uint32_t reqsCount = 0;
  bool validate_only = false;
  auto status = mClient.hwc_session_->CommitOrPrepare(mDisplay, validate_only, &presentFence,
                                                      &typesCount, &reqsCount, &needsCommit);
  if (needsCommit) {
    if (status != HWC2::Error::None && status != HWC2::Error::HasChanges) {
      ALOGE("CommitOrPrepare failed %d", status);
    }
    // Implement post validation. Getcomptypes etc;
    postValidateDisplay(typesCount, reqsCount);
    mWriter.setPresentOrValidateResult(0);
  } else {
    if (status == HWC2::Error::HasChanges) {
      // Perform post validate.
      auto error = postValidateDisplay(typesCount, reqsCount);
      if (error == Error::NONE) {
        mClient.hwc_session_->AcceptDisplayChanges(mDisplay);
      }
      // Set result to 2.
      mWriter.setPresentOrValidateResult(2);
    } else {
      // Set result to 1.
      mWriter.setPresentOrValidateResult(1);
    }
    // perform post present display.
    postPresentDisplay(&presentFence);
    mClient.LtpoOnPresent(mDisplay);
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerCursorPosition(uint16_t length) {
  if (length != CommandWriter::kSetLayerCursorPositionLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetCursorPosition(mDisplay, mLayer, readSigned(), readSigned());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerBuffer(uint16_t length) {
  if (length != CommandWriter::kSetLayerBufferLength) {
    return false;
  }

  bool useCache;
  auto slot = read();
  auto buffer = readHandle(useCache);
  shared_ptr<Fence> fence = nullptr;
  readFence(&fence, "layer");
  auto error = lookupBuffer(BufferCache::LAYER_BUFFERS, slot, useCache, buffer, &buffer);
  if (error == Error::NONE) {
    auto err = mClient.hwc_session_->SetLayerBuffer(mDisplay, mLayer, buffer, fence);
    error = static_cast<Error>(err);
    auto updateBufErr = updateBuffer(BufferCache::LAYER_BUFFERS, slot, useCache, buffer);
    if (static_cast<Error>(error) == Error::NONE) {
      error = updateBufErr;
    }
  }
  if (static_cast<Error>(error) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(error));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerSurfaceDamage(uint16_t length) {
  // N rectangles
  if (length % 4 != 0) {
    return false;
  }

  auto damage = readRegion(length / 4);
  hwc_region region = {damage.size(), damage.data()};
  auto err = mClient.hwc_session_->SetLayerSurfaceDamage(mDisplay, mLayer, region);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerBlendMode(uint16_t length) {
  if (length != CommandWriter::kSetLayerBlendModeLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerBlendMode(mDisplay, mLayer, readSigned());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerColor(uint16_t length) {
  if (length != CommandWriter::kSetLayerColorLength) {
    return false;
  }
  auto color = readColor();
  hwc_color_t hwc_color{color.r, color.g, color.b, color.a};
  auto err = mClient.hwc_session_->SetLayerColor(mDisplay, mLayer, hwc_color);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerCompositionType(uint16_t length) {
  if (length != CommandWriter::kSetLayerCompositionTypeLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerCompositionType(mDisplay, mLayer, readSigned());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerDataspace(uint16_t length) {
  if (length != CommandWriter::kSetLayerDataspaceLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerDataspace(mDisplay, mLayer, readSigned());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerDisplayFrame(uint16_t length) {
  if (length != CommandWriter::kSetLayerDisplayFrameLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerDisplayFrame(mDisplay, mLayer, readRect());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerPlaneAlpha(uint16_t length) {
  if (length != CommandWriter::kSetLayerPlaneAlphaLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerPlaneAlpha(mDisplay, mLayer, readFloat());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerSidebandStream(uint16_t length) {
  if (length != CommandWriter::kSetLayerSidebandStreamLength) {
    return false;
  }

  // Sideband stream is not supported
  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerSourceCrop(uint16_t length) {
  if (length != CommandWriter::kSetLayerSourceCropLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerSourceCrop(mDisplay, mLayer, readFRect());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerTransform(uint16_t length) {
  if (length != CommandWriter::kSetLayerTransformLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerTransform(mDisplay, mLayer, readSigned());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerVisibleRegion(uint16_t length) {
  // N rectangles
  if (length % 4 != 0) {
    return false;
  }

  auto region = readRegion(length / 4);
  hwc_region visibleRegion = {region.size(), region.data()};
  auto err = mClient.hwc_session_->SetLayerVisibleRegion(mDisplay, mLayer, visibleRegion);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerZOrder(uint16_t length) {
  if (length != CommandWriter::kSetLayerZOrderLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerZOrder(mDisplay, mLayer, read());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerType(uint16_t length) {
  if (length != CommandWriter::kSetLayerTypeLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerType(mDisplay, mLayer,
            static_cast<IQtiComposerClient::LayerType>(read()));
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerFlag(uint16_t length) {
  if (length != CommandWriter::kSetLayerFlagLength) {
    return false;
  }

  auto err = mClient.hwc_session_->SetLayerFlag(mDisplay, mLayer,
                                                static_cast<IQtiComposerClient::LayerFlag>(read()));
  if (static_cast<Error>(err) != Error::NONE) {
     mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerPerFrameMetadata(uint16_t length) {
  // (key, value) pairs
  if (length % 2 != 0) {
    return false;
  }

  std::vector<IComposerClient::PerFrameMetadata> metadata;
  metadata.reserve(length / 2);
  while (length > 0) {
    metadata.emplace_back(IComposerClient::PerFrameMetadata{
                          static_cast<IComposerClient::PerFrameMetadataKey>(readSigned()),
                          readFloat()});
    length -= 2;
  }

  std::vector<int32_t> keys;
  std::vector<float> values;
  keys.reserve(metadata.size());
  values.reserve(metadata.size());
  for (const auto& m : metadata) {
    keys.push_back(static_cast<int32_t>(m.key));
    values.push_back(m.value);
  }

  auto err = mClient.hwc_session_->SetLayerPerFrameMetadata(mDisplay, mLayer, metadata.size(),
                                                            keys.data(), values.data());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerFloatColor(uint16_t length) {
  if (length != CommandWriter::kSetLayerFloatColorLength) {
    return false;
  }

  // setLayerFloatColor is not supported
  auto err = Error::UNSUPPORTED;
  mWriter.setError(getCommandLoc(), static_cast<Error>(err));

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerColorTransform(uint16_t length) {
  if (length != CommandWriter::kSetLayerColorTransformLength) {
    return false;
  }

  float matrix[16];
  for (int i = 0; i < 16; i++) {
    matrix[i] = readFloat();
  }

  auto error = mClient.hwc_session_->SetLayerColorTransform(mDisplay, mLayer, matrix);
  if (static_cast<Error>(error) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(error));
  }

  return true;
}

bool QtiComposerClient::CommandReader::parseSetLayerPerFrameMetadataBlobs(uint16_t length) {
  // must have at least one metadata blob
  // of at least size 1 in queue (i.e {/*numBlobs=*/1, key, size, blob})
  if (length < 4) {
    return false;
  }

  uint32_t numBlobs = read();
  length--;
  std::vector<IComposerClient::PerFrameMetadataBlob> metadata;

  for (size_t i = 0; i < numBlobs; i++) {
    IComposerClient::PerFrameMetadataKey key =
      static_cast<IComposerClient::PerFrameMetadataKey>(readSigned());
    uint32_t blobSize = read();
    length -= 2;

    if (length * sizeof(uint32_t) < blobSize) {
      return false;
    }

    metadata.push_back({key, std::vector<uint8_t>()});
    IComposerClient::PerFrameMetadataBlob& metadataBlob = metadata.back();
    metadataBlob.blob.resize(blobSize);
    readBlob(blobSize, metadataBlob.blob.data());
  }

  std::vector<int32_t> keys;
  std::vector<uint32_t> sizes_of_metablob_;
  std::vector<uint8_t> blob_of_data_;
  keys.reserve(metadata.size());
  sizes_of_metablob_.reserve(metadata.size());
  for (const auto& m : metadata) {
    keys.push_back(static_cast<int32_t>(m.key));
    sizes_of_metablob_.push_back(m.blob.size());
    for (size_t i = 0; i < m.blob.size(); i++) {
      blob_of_data_.push_back(m.blob[i]);
    }
  }
  auto err = mClient.hwc_session_->SetLayerPerFrameMetadataBlobs(mDisplay, mLayer, metadata.size(),
                                                                 keys.data(),
                                                                 sizes_of_metablob_.data(),
                                                                 blob_of_data_.data());
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }
  return true;
}

bool QtiComposerClient::CommandReader::parseSetDisplayElapseTime(uint16_t length) {
  if (length < CommandWriter::kSetDisplayElapseTime) {
    return false;
  }
  uint64_t time = read64();

  auto err = mClient.hwc_session_->SetDisplayElapseTime(mDisplay, time);
  if (static_cast<Error>(err) != Error::NONE) {
    mWriter.setError(getCommandLoc(), static_cast<Error>(err));
  }

  return true;
}

hwc_rect_t QtiComposerClient::CommandReader::readRect() {
  return hwc_rect_t{
    readSigned(),
    readSigned(),
    readSigned(),
    readSigned(),
  };
}

std::vector<hwc_rect_t> QtiComposerClient::CommandReader::readRegion(size_t count) {
  std::vector<hwc_rect_t> region;
  region.reserve(count);
  while (count > 0) {
    region.emplace_back(readRect());
    count--;
  }

  return region;
}

hwc_frect_t QtiComposerClient::CommandReader::readFRect() {
  return hwc_frect_t{
    readFloat(),
    readFloat(),
    readFloat(),
    readFloat(),
  };
}

Error QtiComposerClient::CommandReader::lookupBufferCacheEntryLocked(BufferCache cache,
                                                                     uint32_t slot,
                                                                     BufferCacheEntry** outEntry) {
  auto dpy = mClient.mDisplayData.find(mDisplay);
  if (dpy == mClient.mDisplayData.end()) {
    return Error::BAD_DISPLAY;
  }

  BufferCacheEntry* entry = nullptr;
  switch (cache) {
  case BufferCache::CLIENT_TARGETS:
    if (slot < dpy->second.ClientTargets.size()) {
      entry = &dpy->second.ClientTargets[slot];
    }
    break;
  case BufferCache::OUTPUT_BUFFERS:
    if (slot < dpy->second.OutputBuffers.size()) {
      entry = &dpy->second.OutputBuffers[slot];
    }
    break;
  case BufferCache::LAYER_BUFFERS:
    {
      auto ly = dpy->second.Layers.find(mLayer);
      if (ly == dpy->second.Layers.end()) {
        return Error::BAD_LAYER;
      }
      if (slot < ly->second.Buffers.size()) {
        entry = &ly->second.Buffers[slot];
      }
    }
    break;
  case BufferCache::LAYER_SIDEBAND_STREAMS:
    {
      auto ly = dpy->second.Layers.find(mLayer);
      if (ly == dpy->second.Layers.end()) {
        return Error::BAD_LAYER;
      }
      if (slot == 0) {
        entry = &ly->second.SidebandStream;
      }
    }
    break;
  default:
    break;
  }

  if (!entry) {
    ALOGW("invalid buffer slot %" PRIu32, slot);
    return Error::BAD_PARAMETER;
  }

  *outEntry = entry;

  return Error::NONE;
}

Error QtiComposerClient::CommandReader::lookupBuffer(BufferCache cache, uint32_t slot,
                                                     bool useCache, buffer_handle_t handle,
                                                     buffer_handle_t* outHandle) {
  std::lock_guard<std::mutex> lock(mClient.mDisplayDataMutex);
  BufferCacheEntry* entry;
  Error error = lookupBufferCacheEntryLocked(cache, slot, &entry);
  if (error != Error::NONE) {
    return error;
  }

  if (useCache) {
    // input handle is ignored
    *outHandle = entry->getHandle();
  } else if (cache == BufferCache::LAYER_SIDEBAND_STREAMS) {
    if (handle) {
      *outHandle = native_handle_clone(handle);
      if (*outHandle == nullptr) {
        return Error::NO_RESOURCES;
      }
    }
  } else {
    if (!mHandleImporter.importBuffer(handle)) {
      return Error::NO_RESOURCES;
    }

    *outHandle = handle;
  }

  return Error::NONE;
}

Error QtiComposerClient::CommandReader::updateBuffer(BufferCache cache, uint32_t slot,
                                                     bool useCache, buffer_handle_t handle) {
  // handle was looked up from cache
  if (useCache) {
    return Error::NONE;
  }

  std::lock_guard<std::mutex> lock(mClient.mDisplayDataMutex);

  BufferCacheEntry* entry = nullptr;
  Error error = lookupBufferCacheEntryLocked(cache, slot, &entry);
  if (error != Error::NONE) {
    return error;
  }

  *entry = handle;
  return Error::NONE;
}
// Methods from ::android::hidl::base::V1_0::IBase follow.

IQtiComposerClient* HIDL_FETCH_IQtiComposerClient(const char* /* name */) {
  return QtiComposerClient::CreateQtiComposerClientInstance();
}

}  // namespace implementation
}  // namespace V3_0
}  // namespace composer
}  // namespace display
}  // namespace hardware
}  // namespace qti
}  // namespace vendor
