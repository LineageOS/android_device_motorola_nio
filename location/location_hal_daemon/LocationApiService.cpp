/* Copyright (c) 2018-2020 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <memory>
#include <SystemStatus.h>
#include <LocationApiMsg.h>
#include <gps_extended_c.h>

#ifdef POWERMANAGER_ENABLED
#include <PowerEvtHandler.h>
#endif
#include <LocHalDaemonClientHandler.h>
#include <LocationApiService.h>
#include <location_interface.h>
#include <loc_misc_utils.h>

using namespace std;

#define MAX_GEOFENCE_COUNT (200)
#define MAINT_TIMER_INTERVAL_MSEC (60000)
#define AUTO_START_CLIENT_NAME "default"

typedef void* (getLocationInterface)();
typedef void  (createOSFramework)();
typedef void  (destroyOSFramework)();

/******************************************************************************
LocationApiService - static members
******************************************************************************/
LocationApiService* LocationApiService::mInstance = nullptr;
std::mutex LocationApiService::mMutex;

/******************************************************************************
LocHaldIpcListener
******************************************************************************/
class LocHaldIpcListener : public ILocIpcListener {
protected:
    LocationApiService& mService;
public:
    inline LocHaldIpcListener(LocationApiService& service) : mService(service) {}
    // override from LocIpc
    inline void onReceive(const char* data, uint32_t length,
                          const LocIpcRecver* recver) override {
        mService.processClientMsg(data, length);
    }
};
class LocHaldLocalIpcListener : public LocHaldIpcListener {
    const string mClientSockPath = SOCKET_LOC_CLIENT_DIR;
    const string mClientSockPathnamePrefix = SOCKET_LOC_CLIENT_DIR LOC_CLIENT_NAME_PREFIX;
public:
    inline LocHaldLocalIpcListener(LocationApiService& service) : LocHaldIpcListener(service) {}
    inline void onListenerReady() override {
        if (0 != chown(SOCKET_TO_LOCATION_HAL_DAEMON, UID_GPS, GID_LOCCLIENT)) {
            LOC_LOGe("chown to group locclient failed %s", strerror(errno));
        }

        // traverse client sockets directory - then broadcast READY message
        LOC_LOGd(">-- onListenerReady Finding client sockets...");

        DIR *dirp = opendir(mClientSockPath.c_str());
        if (!dirp) {
            LOC_LOGw("%s not created", mClientSockPath.c_str());
            return;
        }

        struct dirent *dp = nullptr;
        struct stat sbuf = {0};
        while (nullptr != (dp = readdir(dirp))) {
            std::string fname = mClientSockPath;
            fname += dp->d_name;
            if (-1 == lstat(fname.c_str(), &sbuf)) {
                continue;
            }
            if ('.' == (dp->d_name[0])) {
                continue;
            }

            if (0 == fname.compare(0, mClientSockPathnamePrefix.size(),
                                   mClientSockPathnamePrefix)) {
                shared_ptr<LocIpcSender> sender = LocHalDaemonClientHandler::createSender(fname);
                LocAPIHalReadyIndMsg msg(SERVICE_NAME);
                LOC_LOGd("<-- Sending ready to socket: %s, msg size %d",
                         fname.c_str(), sizeof(msg));
                LocIpc::send(*sender, reinterpret_cast<const uint8_t*>(&msg),
                             sizeof(msg));
            }
        }
        closedir(dirp);
    }
};

/******************************************************************************
LocationApiService - constructors
******************************************************************************/
LocationApiService::LocationApiService(const configParamToRead & configParamRead) :

    mLocationControlId(0),
    mAutoStartGnss(configParamRead.autoStartGnss),
    mPowerState(POWER_STATE_UNKNOWN),
    mPositionMode((GnssSuplMode)configParamRead.positionMode),
    mMaintTimer(this)
#ifdef POWERMANAGER_ENABLED
    ,mPowerEventObserver(nullptr)
#endif
    {

    LOC_LOGd("AutoStartGnss=%u", mAutoStartGnss);
    LOC_LOGd("GnssSessionTbfMs=%u", configParamRead.gnssSessionTbfMs);
    LOC_LOGd("DeleteAllBeforeAutoStart=%u", configParamRead.deleteAllBeforeAutoStart);
    LOC_LOGd("DeleteAllOnEnginesMask=%u", configParamRead.posEngineMask);
    LOC_LOGd("PositionMode=%u", configParamRead.positionMode);

    // create Location control API
    mControlCallabcks.size = sizeof(mControlCallabcks);
    mControlCallabcks.responseCb = [this](LocationError err, uint32_t id) {
        onControlResponseCallback(err, id);
    };
    mControlCallabcks.collectiveResponseCb =
            [this](size_t count, LocationError *errs, uint32_t *ids) {
        onControlCollectiveResponseCallback(count, errs, ids);
    };
    mControlCallabcks.gnssConfigCb =
            [this](uint32_t sessionId, const GnssConfig& config) {
        onGnssConfigCallback(sessionId, config);
    };

    mLocationControlApi = LocationControlAPI::createInstance(mControlCallabcks);
    if (nullptr == mLocationControlApi) {
        LOC_LOGd("Failed to create LocationControlAPI");
        return;
    }

    // enable
    mLocationControlId = mLocationControlApi->enable(LOCATION_TECHNOLOGY_TYPE_GNSS);
    LOC_LOGd("-->enable=%u", mLocationControlId);
    // this is a unique id assigned to this daemon - will be used when disable

#ifdef POWERMANAGER_ENABLED
    // register power event handler
    mPowerEventObserver = PowerEvtHandler::getPwrEvtHandler(this);
    if (nullptr == mPowerEventObserver) {
        LOC_LOGe("Failed to regiseter Powerevent handler");
        return;
    }
#endif

    // Create OSFramework and IzatManager instance
    createOSFrameworkInstance();

    mMaintTimer.start(MAINT_TIMER_INTERVAL_MSEC, false);

    // create a default client if enabled by config
    if (mAutoStartGnss) {
        if ((configParamRead.deleteAllBeforeAutoStart) &&
                (configParamRead.posEngineMask != 0)) {
            GnssAidingData aidingData = {};
            aidingData.deleteAll = true;
            aidingData.posEngineMask = configParamRead.posEngineMask;
            mLocationControlApi->gnssDeleteAidingData(aidingData);
        }

        LOC_LOGd("--> Starting a default client...");
        LocHalDaemonClientHandler* pClient =
                new LocHalDaemonClientHandler(this, AUTO_START_CLIENT_NAME, LOCATION_CLIENT_API);
        mClients.emplace(AUTO_START_CLIENT_NAME, pClient);

        pClient->updateSubscription(
                E_LOC_CB_GNSS_LOCATION_INFO_BIT | E_LOC_CB_GNSS_SV_BIT);

        LocationOptions locationOption = {};
        locationOption.size = sizeof(locationOption);
        locationOption.minInterval = configParamRead.gnssSessionTbfMs;
        locationOption.minDistance = 0;
        locationOption.mode = mPositionMode;

        pClient->startTracking(locationOption);
        pClient->mTracking = true;
        loc_boot_kpi_marker("L - Auto Session Start");
        pClient->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);
    }

    // start receiver - never return
    LOC_LOGd("Ready, start Ipc Receivers");
    auto recver = LocIpc::getLocIpcLocalRecver(make_shared<LocHaldLocalIpcListener>(*this),
            SOCKET_TO_LOCATION_HAL_DAEMON);
    // blocking: set to false
    mIpc.startNonBlockingListening(recver);

    mBlockingRecver = LocIpc::getLocIpcQrtrRecver(make_shared<LocHaldIpcListener>(*this),
            LOCATION_CLIENT_API_QSOCKET_HALDAEMON_SERVICE_ID,
            LOCATION_CLIENT_API_QSOCKET_HALDAEMON_INSTANCE_ID);
    mIpc.startBlockingListening(*mBlockingRecver);
}

LocationApiService::~LocationApiService() {
    mIpc.stopNonBlockingListening();
    mIpc.stopBlockingListening(*mBlockingRecver);

    // free resource associated with the client
    for (auto each : mClients) {
        LOC_LOGd(">-- deleted client [%s]", each.first.c_str());
        each.second->cleanup();
    }

    // Destroy OSFramework instance
    destroyOSFrameworkInstance();

    // delete location contorol API handle
    mLocationControlApi->disable(mLocationControlId);
    mLocationControlApi->destroy();
}

/******************************************************************************
LocationApiService - implementation - registration
******************************************************************************/
void LocationApiService::processClientMsg(const char* data, uint32_t length) {

    // parse received message
    LocAPIMsgHeader* pMsg = (LocAPIMsgHeader*)data;

    // throw away msg that does not come from location hal daemon client, e.g. LCA/LIA
    if (false == pMsg->isValidClientMsg(length)) {
        return;
    }

    LOC_LOGi(">-- onReceive len=%u remote client=%s msgId=%u\n",
            length, pMsg->mSocketName, pMsg->msgId);

    switch (pMsg->msgId) {
        case E_LOCAPI_CLIENT_REGISTER_MSG_ID: {
            // new client
            if (sizeof(LocAPIClientRegisterReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            newClient(reinterpret_cast<LocAPIClientRegisterReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_CLIENT_DEREGISTER_MSG_ID: {
            // delete client
            if (sizeof(LocAPIClientDeregisterReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            deleteClient(reinterpret_cast<LocAPIClientDeregisterReqMsg*>(pMsg));
            break;
        }

        case E_LOCAPI_START_TRACKING_MSG_ID: {
            // start
            if (sizeof(LocAPIStartTrackingReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            startTracking(reinterpret_cast<LocAPIStartTrackingReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_STOP_TRACKING_MSG_ID: {
            // stop
            if (sizeof(LocAPIStopTrackingReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            stopTracking(reinterpret_cast<LocAPIStopTrackingReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_UPDATE_CALLBACKS_MSG_ID: {
            // update subscription
            if (sizeof(LocAPIUpdateCallbacksReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            updateSubscription(reinterpret_cast<LocAPIUpdateCallbacksReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_UPDATE_TRACKING_OPTIONS_MSG_ID: {
            if (sizeof(LocAPIUpdateTrackingOptionsReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            updateTrackingOptions(reinterpret_cast
                    <LocAPIUpdateTrackingOptionsReqMsg*>(pMsg));
            break;
        }

        case E_LOCAPI_START_BATCHING_MSG_ID: {
            // start
            if (sizeof(LocAPIStartBatchingReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            startBatching(reinterpret_cast<LocAPIStartBatchingReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_STOP_BATCHING_MSG_ID: {
            // stop
            if (sizeof(LocAPIStopBatchingReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            stopBatching(reinterpret_cast<LocAPIStopBatchingReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_UPDATE_BATCHING_OPTIONS_MSG_ID: {
            if (sizeof(LocAPIUpdateBatchingOptionsReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            updateBatchingOptions(reinterpret_cast
                    <LocAPIUpdateBatchingOptionsReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_ADD_GEOFENCES_MSG_ID: {
            addGeofences(reinterpret_cast<LocAPIAddGeofencesReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_REMOVE_GEOFENCES_MSG_ID: {
            removeGeofences(reinterpret_cast<LocAPIRemoveGeofencesReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_MODIFY_GEOFENCES_MSG_ID: {
            modifyGeofences(reinterpret_cast<LocAPIModifyGeofencesReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_PAUSE_GEOFENCES_MSG_ID: {
            pauseGeofences(reinterpret_cast<LocAPIPauseGeofencesReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_RESUME_GEOFENCES_MSG_ID: {
            resumeGeofences(reinterpret_cast<LocAPIResumeGeofencesReqMsg*>(pMsg));
            break;
        }
        case E_LOCAPI_CONTROL_UPDATE_NETWORK_AVAILABILITY_MSG_ID: {
            if (sizeof(LocAPIUpdateNetworkAvailabilityReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            updateNetworkAvailability(reinterpret_cast
                    <LocAPIUpdateNetworkAvailabilityReqMsg*>(pMsg)->mAvailability);
            break;
        }
        case E_LOCAPI_GET_GNSS_ENGERY_CONSUMED_MSG_ID: {
            if (sizeof(LocAPIGetGnssEnergyConsumedReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            getGnssEnergyConsumed(reinterpret_cast
                    <LocAPIGetGnssEnergyConsumedReqMsg*>(pMsg)->mSocketName);
            break;
        }
        case E_LOCAPI_PINGTEST_MSG_ID: {
            if (sizeof(LocAPIPingTestReqMsg) != length) {
                LOC_LOGe("invalid message");
                break;
            }
            pingTest(reinterpret_cast<LocAPIPingTestReqMsg*>(pMsg));
            break;
        }

        // location configuration API
        case E_INTAPI_CONFIG_CONSTRAINTED_TUNC_MSG_ID: {
            if (sizeof(LocConfigConstrainedTuncReqMsg) != length) {
                LOC_LOGe("invalid LocConfigConstrainedTuncReqMsg");
                break;
            }
            configConstrainedTunc(reinterpret_cast<LocConfigConstrainedTuncReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_POSITION_ASSISTED_CLOCK_ESTIMATOR_MSG_ID: {
            if (sizeof(LocConfigPositionAssistedClockEstimatorReqMsg) != length) {
                LOC_LOGe("invalid LocConfigPositionAssistedClockEstimatorReqMsg");
                break;
            }
            configPositionAssistedClockEstimator(reinterpret_cast
                    <LocConfigPositionAssistedClockEstimatorReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_SV_CONSTELLATION_MSG_ID: {
            if (sizeof(LocConfigSvConstellationReqMsg) != length) {
                LOC_LOGe("invalid LocConfigSvConstellationReqMsg");
                break;
            }
            configConstellations(reinterpret_cast
                    <LocConfigSvConstellationReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_CONSTELLATION_SECONDARY_BAND_MSG_ID: {
            if (sizeof(LocConfigConstellationSecondaryBandReqMsg) != length) {
                LOC_LOGe("invalid LocConfigConstellationSecondaryBandReqMsg");
                break;
            }
            configConstellationSecondaryBand(reinterpret_cast
                    <LocConfigConstellationSecondaryBandReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_AIDING_DATA_DELETION_MSG_ID: {
            if (sizeof(LocConfigAidingDataDeletionReqMsg) != length) {
                LOC_LOGe("invalid LocConfigAidingDataDeletionReqMsg");
                break;
            }
            configAidingDataDeletion(reinterpret_cast<LocConfigAidingDataDeletionReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_LEVER_ARM_MSG_ID: {
            if (sizeof(LocConfigLeverArmReqMsg) != length) {
                LOC_LOGe("invalid LocConfigLeverArmReqMsg");
                break;
            }
            configLeverArm(reinterpret_cast<LocConfigLeverArmReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_ROBUST_LOCATION_MSG_ID: {
            if (sizeof(LocConfigRobustLocationReqMsg) != length) {
                LOC_LOGe("invalid LocConfigRobustLocationReqMsg");
                break;
            }
            configRobustLocation(reinterpret_cast<LocConfigRobustLocationReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_MIN_GPS_WEEK_MSG_ID: {
            if (sizeof(LocConfigMinGpsWeekReqMsg) != length) {
                LOC_LOGe("invalid LocConfigMinGpsWeekReqMsg");
                break;
            }
            configMinGpsWeek(reinterpret_cast<LocConfigMinGpsWeekReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_DEAD_RECKONING_ENGINE_MSG_ID: {
            if (sizeof(LocConfigDrEngineParamsReqMsg) != length) {
                LOC_LOGe("invalid LocConfigDrEngineParamsReqMsg");
                break;
            }
            configDeadReckoningEngineParams(reinterpret_cast<LocConfigDrEngineParamsReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_CONFIG_MIN_SV_ELEVATION_MSG_ID: {
            if (sizeof(LocConfigMinSvElevationReqMsg) != length) {
                LOC_LOGe("invalid LocConfigMinSvElevationReqMsg");
                break;
            }
            configMinSvElevation(reinterpret_cast<LocConfigMinSvElevationReqMsg*>(pMsg));
            break;
        }

        case E_INTAPI_GET_ROBUST_LOCATION_CONFIG_REQ_MSG_ID: {
            if (sizeof(LocConfigGetRobustLocationConfigReqMsg) != length) {
                LOC_LOGe("invalid LocConfigGetRobustLocationConfigReqMsg");
                break;
            }
            getGnssConfig(pMsg, GNSS_CONFIG_FLAGS_ROBUST_LOCATION_BIT);
            break;
        }

        case E_INTAPI_GET_MIN_GPS_WEEK_REQ_MSG_ID: {
            if (sizeof(LocConfigGetMinGpsWeekReqMsg) != length) {
                LOC_LOGe("invalid LocConfigGetMinGpsWeekReqMsg");
                break;
            }
            getGnssConfig(pMsg, GNSS_CONFIG_FLAGS_MIN_GPS_WEEK_BIT);
            break;
        }

        case E_INTAPI_GET_MIN_SV_ELEVATION_REQ_MSG_ID: {
            if (sizeof(LocConfigGetMinSvElevationReqMsg) != length) {
                LOC_LOGe("invalid LocConfigGetMinSvElevationReqMsg");
                break;
            }
            getGnssConfig(pMsg, GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT);
            break;
        }

        case E_INTAPI_GET_CONSTELLATION_SECONDARY_BAND_CONFIG_REQ_MSG_ID: {
            if (sizeof(LocConfigGetConstellationSecondaryBandConfigReqMsg) != length) {
                LOC_LOGe("invalid LocConfigGetConstellationSecondaryBandConfigReqMsg");
                break;
            }
            getConstellationSecondaryBandConfig(
                    (const LocConfigGetConstellationSecondaryBandConfigReqMsg*) pMsg);
            break;
        }

        default: {
            LOC_LOGe("Unknown message with id: %d ", pMsg->msgId);
            break;
        }
    }
}

/******************************************************************************
LocationApiService - implementation - registration
******************************************************************************/
void LocationApiService::newClient(LocAPIClientRegisterReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    std::string clientname(pMsg->mSocketName);

    // if this name is already used return error
    if (mClients.find(clientname) != mClients.end()) {
        LOC_LOGe("invalid client=%s already existing", clientname.c_str());
        return;
    }

    // store it in client property database
    LocHalDaemonClientHandler *pClient =
            new LocHalDaemonClientHandler(this, clientname, pMsg->mClientType);
    if (!pClient) {
        LOC_LOGe("failed to register client=%s", clientname.c_str());
        return;
    }

    mClients.emplace(clientname, pClient);
    LOC_LOGi(">-- registered new client=%s", clientname.c_str());
}

void LocationApiService::deleteClient(LocAPIClientDeregisterReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    std::string clientname(pMsg->mSocketName);
    deleteClientbyName(clientname);
}

void LocationApiService::deleteClientbyName(const std::string clientname) {
    LOC_LOGi(">-- deleteClient client=%s", clientname.c_str());

    // delete this client from property db
    LocHalDaemonClientHandler* pClient = getClient(clientname);

    if (!pClient) {
        LOC_LOGe(">-- deleteClient invlalid client=%s", clientname.c_str());
        return;
    }
    mClients.erase(clientname);
    pClient->cleanup();
}
/******************************************************************************
LocationApiService - implementation - tracking
******************************************************************************/
void LocationApiService::startTracking(LocAPIStartTrackingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invlalid client=%s", pMsg->mSocketName);
        return;
    }

    LocationOptions locationOption = pMsg->locOptions;
    // set the mode according to the master position mode
    locationOption.mode = mPositionMode;

    if (!pClient->startTracking(locationOption)) {
        LOC_LOGe("Failed to start session");
        return;
    }
    // success
    pClient->mTracking = true;
    pClient->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);

    LOC_LOGi(">-- start started session");
    return;
}

void LocationApiService::stopTracking(LocAPIStopTrackingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- stop invlalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->mTracking = false;
    pClient->unsubscribeLocationSessionCb();
    pClient->stopTracking();
    pClient->mPendingMessages.push(E_LOCAPI_STOP_TRACKING_MSG_ID);
    LOC_LOGi(">-- stopping session");
}

// no need to hold the lock as lock has been held on calling functions
void LocationApiService::suspendAllTrackingSessions() {
    for (auto client : mClients) {
        // stop session if running
        if (client.second && client.second->mTracking) {
            client.second->stopTracking();
            client.second->mPendingMessages.push(E_LOCAPI_STOP_TRACKING_MSG_ID);
            LOC_LOGi("--> suspended");
        }
    }
}

// no need to hold the lock as lock has been held on calling functions
void LocationApiService::resumeAllTrackingSessions() {
    for (auto client : mClients) {
        // start session if not running
        if (client.second && client.second->mTracking) {

            // resume session with preserved options
            if (!client.second->startTracking()) {
                LOC_LOGe("Failed to start session");
                return;
            }
            // success
            client.second->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);
            LOC_LOGi("--> resumed");
        }
    }
}

void LocationApiService::updateSubscription(LocAPIUpdateCallbacksReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- updateSubscription invlalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->updateSubscription(pMsg->locationCallbacks);

    LOC_LOGi(">-- update subscription client=%s mask=0x%x",
            pMsg->mSocketName, pMsg->locationCallbacks);
}

void LocationApiService::updateTrackingOptions(LocAPIUpdateTrackingOptionsReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);

    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (pClient) {
        LocationOptions locationOption = pMsg->locOptions;
        // set the mode according to the master position mode
        locationOption.mode = mPositionMode;
        pClient->updateTrackingOptions(locationOption);
        pClient->mPendingMessages.push(E_LOCAPI_UPDATE_TRACKING_OPTIONS_MSG_ID);
    }

    LOC_LOGi(">-- update tracking options");
}

void LocationApiService::updateNetworkAvailability(bool availability) {

    LOC_LOGi(">-- updateNetworkAvailability=%u", availability);
    GnssInterface* gnssInterface = getGnssInterface();
    if (gnssInterface) {
        // Map the network connectivity to MOBILE for now.
        // In next phase, when we support third party connectivity manager,
        // we plan to deplicate this API.
        gnssInterface->updateConnectionStatus(
                availability, loc_core::TYPE_MOBILE, false, NETWORK_HANDLE_UNKNOWN);
    }
}

void LocationApiService::getGnssEnergyConsumed(const char* clientSocketName) {

    LOC_LOGi(">-- getGnssEnergyConsumed by=%s", clientSocketName);

    GnssInterface* gnssInterface = getGnssInterface();
    if (!gnssInterface) {
        LOC_LOGe(">-- getGnssEnergyConsumed null GnssInterface");
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    bool requestAlreadyPending = false;
    for (auto each : mClients) {
        if ((each.second != nullptr) &&
            (each.second->hasPendingEngineInfoRequest(E_ENGINE_INFO_CB_GNSS_ENERGY_CONSUMED_BIT))) {
            requestAlreadyPending = true;
            break;
        }
    }

    std::string clientname(clientSocketName);
    LocHalDaemonClientHandler* pClient = getClient(clientname);
    if (pClient) {
        pClient->addEngineInfoRequst(E_ENGINE_INFO_CB_GNSS_ENERGY_CONSUMED_BIT);

        // this is first client coming to request GNSS energy consumed
        if (requestAlreadyPending == false) {
            LOC_LOGd("--< issue request to GNSS HAL");

            // callback function for engine hub to report back sv event
            GnssEnergyConsumedCallback reportEnergyCb =
                [this](uint64_t total) {
                    onGnssEnergyConsumedCb(total);
                };

            gnssInterface->getGnssEnergyConsumed(reportEnergyCb);
        }
    }
}

void LocationApiService::getConstellationSecondaryBandConfig(
        const LocConfigGetConstellationSecondaryBandConfigReqMsg* pReqMsg) {

    LOC_LOGi(">--getConstellationConfig");
    GnssInterface* gnssInterface = getGnssInterface();
    if (!gnssInterface) {
        LOC_LOGe(">-- null GnssInterface");
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    // retrieve the constellation enablement/disablement config
    // blacklisted SV info and secondary band config
    uint32_t sessionId = gnssInterface-> gnssGetSecondaryBandConfig();

    // if sessionId is 0, e.g.: error callback will be delivered
    // by addConfigRequestToMap
    addConfigRequestToMap(sessionId, pReqMsg);
}

/******************************************************************************
LocationApiService - implementation - batching
******************************************************************************/
void LocationApiService::startBatching(LocAPIStartBatchingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invalid client=%s", pMsg->mSocketName);
        return;
    }

    if (!pClient->startBatching(pMsg->intervalInMs, pMsg->distanceInMeters,
                pMsg->batchingMode)) {
        LOC_LOGe("Failed to start session");
        return;
    }
    // success
    pClient->mBatching = true;
    pClient->mBatchingMode = pMsg->batchingMode;
    pClient->mPendingMessages.push(E_LOCAPI_START_BATCHING_MSG_ID);

    LOC_LOGi(">-- start batching session");
    return;
}

void LocationApiService::stopBatching(LocAPIStopBatchingReqMsg *pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- stop invalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->mBatching = false;
    pClient->mBatchingMode = BATCHING_MODE_NO_AUTO_REPORT;
    pClient->updateSubscription(0);
    pClient->stopBatching();
    pClient->mPendingMessages.push(E_LOCAPI_STOP_BATCHING_MSG_ID);
    LOC_LOGi(">-- stopping batching session");
}

void LocationApiService::updateBatchingOptions(LocAPIUpdateBatchingOptionsReqMsg *pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (pClient) {
        pClient->updateBatchingOptions(pMsg->intervalInMs, pMsg->distanceInMeters,
                pMsg->batchingMode);
        pClient->mPendingMessages.push(E_LOCAPI_UPDATE_BATCHING_OPTIONS_MSG_ID);
    }

    LOC_LOGi(">-- update batching options");
}

/******************************************************************************
LocationApiService - implementation - geofence
******************************************************************************/
void LocationApiService::addGeofences(LocAPIAddGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invlalid client=%s", pMsg->mSocketName);
        return;
    }
    if (pMsg->geofences.count > MAX_GEOFENCE_COUNT) {
        LOC_LOGe(">-- geofence count greater than MAX =%d", pMsg->geofences.count);
        return;
    }
    GeofenceOption* gfOptions =
            (GeofenceOption*)malloc(pMsg->geofences.count * sizeof(GeofenceOption));
    GeofenceInfo* gfInfos = (GeofenceInfo*)malloc(pMsg->geofences.count * sizeof(GeofenceInfo));
    uint32_t* clientIds = (uint32_t*)malloc(pMsg->geofences.count * sizeof(uint32_t));
    if ((nullptr == gfOptions) || (nullptr == gfInfos) || (nullptr == clientIds)) {
        LOC_LOGe("Failed to malloc memory!");
        if (clientIds != nullptr) {
            free(clientIds);
        }
        if (gfInfos != nullptr) {
            free(gfInfos);
        }
        if (gfOptions != nullptr) {
            free(gfOptions);
        }
        return;
    }

    for(int i=0; i < pMsg->geofences.count; ++i) {
        gfOptions[i] = (*(pMsg->geofences.gfPayload + i)).gfOption;
        gfInfos[i] = (*(pMsg->geofences.gfPayload + i)).gfInfo;
        clientIds[i] = (*(pMsg->geofences.gfPayload + i)).gfClientId;
    }

    uint32_t* sessions = pClient->addGeofences(pMsg->geofences.count, gfOptions, gfInfos);
    if (!sessions) {
        LOC_LOGe("Failed to add geofences");
        free(clientIds);
        free(gfInfos);
        free(gfOptions);
        return;
    }
    pClient->setGeofenceIds(pMsg->geofences.count, clientIds, sessions);
    // success
    pClient->mGfPendingMessages.push(E_LOCAPI_ADD_GEOFENCES_MSG_ID);

    LOC_LOGi(">-- add geofences");
    free(clientIds);
    free(gfInfos);
    free(gfOptions);
}

void LocationApiService::removeGeofences(LocAPIRemoveGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (nullptr == pClient) {
        LOC_LOGe("removeGeofences - Null client!");
        return;
    }
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->removeGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_REMOVE_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- remove geofences");
    free(sessions);
}
void LocationApiService::modifyGeofences(LocAPIModifyGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (nullptr == pClient) {
        LOC_LOGe("modifyGeofences - Null client!");
        return;
    }
    if (pMsg->geofences.count > MAX_GEOFENCE_COUNT) {
        LOC_LOGe("modifyGeofences - geofence count greater than MAX =%d", pMsg->geofences.count);
        return;
    }
    GeofenceOption* gfOptions = (GeofenceOption*)
            malloc(sizeof(GeofenceOption) * pMsg->geofences.count);
    uint32_t* clientIds = (uint32_t*)malloc(sizeof(uint32_t) * pMsg->geofences.count);
    if (nullptr == gfOptions || nullptr == clientIds) {
        LOC_LOGe("Failed to malloc memory!");
        if (clientIds != nullptr) {
            free(clientIds);
        }
        if (gfOptions != nullptr) {
            free(gfOptions);
        }
        return;
    }
    for (int i=0; i<pMsg->geofences.count; ++i) {
        gfOptions[i] = (*(pMsg->geofences.gfPayload + i)).gfOption;
        clientIds[i] = (*(pMsg->geofences.gfPayload + i)).gfClientId;
    }
    uint32_t* sessions = pClient->getSessionIds(pMsg->geofences.count, clientIds);

    if (pClient && sessions) {
        pClient->modifyGeofences(pMsg->geofences.count, sessions, gfOptions);
        pClient->mGfPendingMessages.push(E_LOCAPI_MODIFY_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- modify geofences");
    free(sessions);
    free(clientIds);
    free(gfOptions);
}
void LocationApiService::pauseGeofences(LocAPIPauseGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (nullptr == pClient) {
        LOC_LOGe("pauseGeofences - Null client!");
        return;
    }
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->pauseGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_PAUSE_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- pause geofences");
    free(sessions);
}
void LocationApiService::resumeGeofences(LocAPIResumeGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (nullptr == pClient) {
        LOC_LOGe("resumeGeofences - Null client!");
        return;
    }
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->resumeGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_RESUME_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- resume geofences");
    free(sessions);
}

void LocationApiService::pingTest(LocAPIPingTestReqMsg* pMsg) {

    // test only - ignore this request when config is not enabled
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- pingTest invlalid client=%s", pMsg->mSocketName);
        return;
    }
    pClient->pingTest();
    LOC_LOGd(">-- pingTest");
}

void LocationApiService::configConstrainedTunc(
        const LocConfigConstrainedTuncReqMsg* pMsg){

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    uint32_t sessionId = mLocationControlApi->configConstrainedTimeUncertainty(
            pMsg->mEnable, pMsg->mTuncConstraint, pMsg->mEnergyBudget);
    LOC_LOGi(">-- enable: %d, tunc constraint %f, energy budget %d, session ID = %d",
             pMsg->mEnable, pMsg->mTuncConstraint, pMsg->mEnergyBudget,
             sessionId);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configPositionAssistedClockEstimator(
        const LocConfigPositionAssistedClockEstimatorReqMsg* pMsg)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!pMsg || !mLocationControlApi) {
        return;
    }

    uint32_t sessionId = mLocationControlApi->
            configPositionAssistedClockEstimator(pMsg->mEnable);
    LOC_LOGi(">-- enable: %d, session ID = %d", pMsg->mEnable,  sessionId);

    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configConstellations(const LocConfigSvConstellationReqMsg* pMsg) {

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    uint32_t sessionId = mLocationControlApi->configConstellations(
            pMsg->mConstellationEnablementConfig, pMsg->mBlacklistSvConfig);

    LOC_LOGe(">-- reset sv type config: %d, enable constellations: 0x%" PRIx64 ", "
             "blacklisted consteallations: 0x%" PRIx64 ", ",
             (pMsg->mConstellationEnablementConfig.size == 0),
             pMsg->mConstellationEnablementConfig.enabledSvTypesMask,
             pMsg->mConstellationEnablementConfig.blacklistedSvTypesMask);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configConstellationSecondaryBand(
        const LocConfigConstellationSecondaryBandReqMsg* pMsg) {

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    uint32_t sessionId = mLocationControlApi->configConstellationSecondaryBand(
            pMsg->mSecondaryBandConfig);

    LOC_LOGe(">-- secondary band size %d, enabled constellation: 0x%" PRIx64 ", "
             "secondary band disabed constellation: 0x%" PRIx64 "",
             pMsg->mSecondaryBandConfig.size,
             pMsg->mSecondaryBandConfig.enabledSvTypesMask,
             pMsg->mSecondaryBandConfig.blacklistedSvTypesMask);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configAidingDataDeletion(LocConfigAidingDataDeletionReqMsg* pMsg) {

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    LOC_LOGi(">-- client %s, deleteAll %d",
             pMsg->mSocketName, pMsg->mAidingData.deleteAll);

    // suspend all sessions before calling delete
    suspendAllTrackingSessions();

    uint32_t sessionId = mLocationControlApi->gnssDeleteAidingData(pMsg->mAidingData);
    addConfigRequestToMap(sessionId, pMsg);

#ifdef POWERMANAGER_ENABLED
    // We do not need to resume the session if device is suspend/shutdown state
    // as sessions will resumed when power state changes to resume
    if ((POWER_STATE_SUSPEND == mPowerState) ||
        (POWER_STATE_SHUTDOWN == mPowerState)) {
        return;
    }
#endif

    // resume all sessions after calling aiding data deletion
    resumeAllTrackingSessions();
}

void LocationApiService::configLeverArm(const LocConfigLeverArmReqMsg* pMsg){

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    uint32_t sessionId = mLocationControlApi->configLeverArm(pMsg->mLeverArmConfigInfo);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configRobustLocation(const LocConfigRobustLocationReqMsg* pMsg){

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    LOC_LOGi(">-- client %s, enable %d, enableForE911 %d",
             pMsg->mSocketName, pMsg->mEnable, pMsg->mEnableForE911);

    uint32_t sessionId = mLocationControlApi->configRobustLocation(
            pMsg->mEnable, pMsg->mEnableForE911);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configMinGpsWeek(const LocConfigMinGpsWeekReqMsg* pMsg){

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);

    LOC_LOGi(">-- client %s, minGpsWeek %u",
             pMsg->mSocketName, pMsg->mMinGpsWeek);

    uint32_t sessionId =
            mLocationControlApi->configMinGpsWeek(pMsg->mMinGpsWeek);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::configMinSvElevation(const LocConfigMinSvElevationReqMsg* pMsg){

    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGi(">-- client %s, minSvElevation %u", pMsg->mSocketName, pMsg->mMinSvElevation);

    GnssConfig gnssConfig = {};
    gnssConfig.flags = GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT;
    gnssConfig.minSvElevation = pMsg->mMinSvElevation;
    uint32_t sessionId = gnssUpdateConfig(gnssConfig);

    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::getGnssConfig(const LocAPIMsgHeader* pReqMsg,
                                       GnssConfigFlagsBits configFlag) {

    std::lock_guard<std::mutex> lock(mMutex);
    if (!pReqMsg) {
        return;
    }

    uint32_t sessionId = 0;
    uint32_t* sessionIds = mLocationControlApi->gnssGetConfig(
                configFlag);
    if (sessionIds) {
        LOC_LOGd(">-- session id %d", *sessionIds);
        sessionId = *sessionIds;
    }
    // if sessionId is 0, e.g.: error callback will be delivered
    // by addConfigRequestToMap
    addConfigRequestToMap(sessionId, pReqMsg);
}

void LocationApiService::configDeadReckoningEngineParams(const LocConfigDrEngineParamsReqMsg* pMsg){
    if (!pMsg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    uint32_t sessionId = mLocationControlApi->configDeadReckoningEngineParams(
            pMsg->mDreConfig);
    addConfigRequestToMap(sessionId, pMsg);
}

void LocationApiService::addConfigRequestToMap(
        uint32_t sessionId, const LocAPIMsgHeader* pMsg){
    // for config request that is invoked from location integration API
    // if session id is valid, we need to add it to the map so when response
    // comes back, we can deliver the response to the integration api client
    if (sessionId != 0) {
        ConfigReqClientData configClientData;
        configClientData.configMsgId = pMsg->msgId;
        configClientData.clientName  = pMsg->mSocketName;
        mConfigReqs.emplace(sessionId, configClientData);
    } else {
        // if session id is 0, we need to deliver failed response back to the
        // client
        LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
        if (pClient) {
            pClient->onControlResponseCb(LOCATION_ERROR_GENERAL_FAILURE, pMsg->msgId);
        }
    }
}

/******************************************************************************
LocationApiService - Location Control API callback functions
******************************************************************************/
void LocationApiService::onControlResponseCallback(LocationError err, uint32_t sessionId) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onControlResponseCallback err=%u id=%u", err, sessionId);

    auto configReqData = mConfigReqs.find(sessionId);
    if (configReqData != std::end(mConfigReqs)) {
        LocHalDaemonClientHandler* pClient = getClient(configReqData->second.clientName);
        if (pClient) {
            pClient->onControlResponseCb(err, configReqData->second.configMsgId);
        }
        mConfigReqs.erase(configReqData);
        LOC_LOGd("--< map size %d", mConfigReqs.size());
    } else {
        LOC_LOGe("--< client not found for session id %d", sessionId);
    }
}

void LocationApiService::onControlCollectiveResponseCallback(
    size_t count, LocationError *errs, uint32_t *ids) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (count != 1) {
        LOC_LOGe("--< onControlCollectiveResponseCallback, count is %d, expecting 1", count);
        return;
    }

    uint32_t sessionId = *ids;
    LocationError err = *errs;
    LOC_LOGd("--< onControlCollectiveResponseCallback, session id is %d, err is %d",
             sessionId, err);
    // as we only update one setting at a time, we only need to process
    // the first id
    auto configReqData = mConfigReqs.find(sessionId);
    if (configReqData != std::end(mConfigReqs)) {
        LocHalDaemonClientHandler* pClient = getClient(configReqData->second.clientName.c_str());
        if (pClient) {
            pClient->onControlResponseCb(err, configReqData->second.configMsgId);
        }
        mConfigReqs.erase(configReqData);
        LOC_LOGd("--< map size %d", mConfigReqs.size());
    } else {
        LOC_LOGe("--< client not found for session id %d", sessionId);
    }

}

void LocationApiService::onGnssConfigCallback(uint32_t sessionId,
                                              const GnssConfig& config) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onGnssConfigCallback, req cnt %d", mConfigReqs.size());

    auto configReqData = mConfigReqs.find(sessionId);
    if (configReqData != std::end(mConfigReqs)) {
        LocHalDaemonClientHandler* pClient = getClient(configReqData->second.clientName);
        if (pClient) {
            // invoke the respCb to deliver success status
            pClient->onControlResponseCb(LOCATION_ERROR_SUCCESS, configReqData->second.configMsgId);
            // invoke the configCb to deliver the config
            pClient->onGnssConfigCb(configReqData->second.configMsgId, config);
        }
        mConfigReqs.erase(configReqData);
        LOC_LOGd("--< map size %d", mConfigReqs.size());
    } else {
        LOC_LOGe("--< client not found for session id %d", sessionId);
    }
}

/******************************************************************************
LocationApiService - power event handlers
******************************************************************************/
#ifdef POWERMANAGER_ENABLED
void LocationApiService::onPowerEvent(PowerStateType powerState) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onPowerEvent %d", powerState);

    mPowerState = powerState;
    if ((POWER_STATE_SUSPEND == powerState) ||
            (POWER_STATE_SHUTDOWN == powerState)) {
        suspendAllTrackingSessions();
    } else if (POWER_STATE_RESUME == powerState) {
        resumeAllTrackingSessions();
    }

    GnssInterface* gnssInterface = getGnssInterface();
    if (!gnssInterface) {
        LOC_LOGe(">-- getGnssEnergyConsumed null GnssInterface");
        return;
    }
    gnssInterface->updateSystemPowerState(powerState);
}
#endif

/******************************************************************************
LocationApiService - on query callback from location engines
******************************************************************************/
void LocationApiService::onGnssEnergyConsumedCb(uint64_t totalGnssEnergyConsumedSinceFirstBoot) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onGnssEnergyConsumedCb");

    LocAPIGnssEnergyConsumedIndMsg msg(SERVICE_NAME, totalGnssEnergyConsumedSinceFirstBoot);
    for (auto each : mClients) {
        // deliver the engergy info to registered client
        each.second->onGnssEnergyConsumedInfoAvailable(msg);
    }
}

/******************************************************************************
LocationApiService - other utilities
******************************************************************************/
GnssInterface* LocationApiService::getGnssInterface() {

    static bool getGnssInterfaceFailed = false;
    static GnssInterface* gnssInterface = nullptr;

    if (nullptr == gnssInterface && !getGnssInterfaceFailed) {
        void * tempPtr = nullptr;
        getLocationInterface* getter = (getLocationInterface*)
                dlGetSymFromLib(tempPtr, "libgnss.so", "getGnssInterface");

        if (nullptr == getter) {
            getGnssInterfaceFailed = true;
        } else {
            gnssInterface = (GnssInterface*)(*getter)();
        }
    }
    return gnssInterface;
}

// Create OSFramework instance
void LocationApiService::createOSFrameworkInstance() {
    void* libHandle = nullptr;
    createOSFramework* getter = (createOSFramework*)dlGetSymFromLib(libHandle,
            "liblocationservice_glue.so", "createOSFramework");
    if (getter != nullptr) {
        (*getter)();
    } else {
        LOC_LOGe("dlGetSymFromLib failed for liblocationservice_glue.so");
    }
}

// Destroy OSFramework instance
void LocationApiService::destroyOSFrameworkInstance() {
    void* libHandle = nullptr;
    destroyOSFramework* getter = (destroyOSFramework*)dlGetSymFromLib(libHandle,
            "liblocationservice_glue.so", "destroyOSFramework");
    if (getter != nullptr) {
        (*getter)();
    } else {
        LOC_LOGe("dlGetSymFromLib failed for liblocationservice_glue.so");
    }
}

void LocationApiService::performMaintenance() {
    ClientNameIpcSenderMap   clientsToCheck;

    // Hold the lock when we access global variable of mClients
    // copy out the client name and shared_ptr of ipc sender for the clients.
    // We do not use mClients directly or making a copy of mClients, as the
    // client handler object can become invalid when the client gets
    // deleted by the thread of LocationApiService.
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto client : mClients) {
            if (client.first.compare(AUTO_START_CLIENT_NAME) != 0) {
                clientsToCheck.emplace(client.first, client.second->getIpcSender());
            }
        }
    }

    for (auto client : clientsToCheck) {
        LocAPIPingTestReqMsg msg(SERVICE_NAME);
        bool messageSent = LocIpc::send(*client.second, reinterpret_cast<const uint8_t*>(&msg),
                                            sizeof(msg));
        LOC_LOGd("send ping message returned %d for client %s", messageSent, client.first.c_str());
        if (messageSent == false) {
            LOC_LOGe("--< ping failed for client %s", client.first.c_str());
            deleteClientbyName(client.first);
        }
    }

    // after maintenace, start next timer
    mMaintTimer.start(MAINT_TIMER_INTERVAL_MSEC, false);
}


// Maintenance timer to clean up resources when client exists without sending
// out de-registration message
void MaintTimer::timeOutCallback() {
    LOC_LOGd("maint timer fired");

    struct PerformMaintenanceReq : public LocMsg {
        PerformMaintenanceReq(LocationApiService* locationApiService) :
                mLocationApiService(locationApiService){}
        virtual ~PerformMaintenanceReq() {}
        void proc() const {
            mLocationApiService->performMaintenance();
        }
        LocationApiService* mLocationApiService;
    };
    mMsgTask.sendMsg(new (nothrow) PerformMaintenanceReq(mLocationApiService));
}
