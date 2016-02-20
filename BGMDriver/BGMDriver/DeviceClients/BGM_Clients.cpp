// This file is part of Background Music.
//
// Background Music is free software: you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 2 of the
// License, or (at your option) any later version.
//
// Background Music is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Background Music. If not, see <http://www.gnu.org/licenses/>.

//
//  BGM_Clients.cpp
//  BGMDriver
//
//  Copyright © 2016 Kyle Neideck
//

// Self Include
#include "BGM_Clients.h"

// Local Includes
#include "BGM_Types.h"
#include "BGM_PlugIn.h"

// PublicUtility Includes
#include "CAException.h"
#include "CACFDictionary.h"


#pragma mark Construction/Destruction

BGM_Clients::BGM_Clients(BGM_TaskQueue* inTaskQueue)
:
    mClientMap(inTaskQueue)
{
    mRelativeVolumeCurve.AddRange(kAppRelativeVolumeMinRawValue,
                                  kAppRelativeVolumeMaxRawValue,
                                  kAppRelativeVolumeMinDbValue,
                                  kAppRelativeVolumeMaxDbValue);
}

#pragma mark Add/Remove Clients

void    BGM_Clients::AddClient(BGM_Client inClient)
{
    CAMutex::Locker theLocker(mMutex);

    // Check whether this is the music player's client
    bool pidMatchesMusicPlayerProperty =
        (mMusicPlayerProcessIDProperty != 0 && inClient.mProcessID == mMusicPlayerProcessIDProperty);
    bool bundleIDMatchesMusicPlayerProperty =
        (mMusicPlayerBundleIDProperty != "" &&
         inClient.mBundleID.IsValid() &&
         inClient.mBundleID == mMusicPlayerBundleIDProperty);
    
    inClient.mIsMusicPlayer = (pidMatchesMusicPlayerProperty || bundleIDMatchesMusicPlayerProperty);
    
    if(inClient.mIsMusicPlayer)
    {
        DebugMsg("BGM_Clients::AddClient: Adding music player client. mClientID = %u", inClient.mClientID);
    }
    
    mClientMap.AddClient(inClient);
    
    // If we're adding BGMApp, update our local copy of its client ID
    if(inClient.mBundleID.IsValid() && inClient.mBundleID == kBGMAppBundleID)
    {
        mBGMAppClientID = inClient.mClientID;
    }
}

void    BGM_Clients::RemoveClient(const UInt32 inClientID)
{
    CAMutex::Locker theLocker(mMutex);
    
    BGM_Client theRemovedClient = mClientMap.RemoveClient(inClientID);
    
    // If we're removing BGMApp, clear our local copy of its client ID
    if(theRemovedClient.mClientID == mBGMAppClientID)
    {
        mBGMAppClientID = -1;
    }
}

#pragma mark IO Status

bool    BGM_Clients::StartIONonRT(UInt32 inClientID)
{
    CAMutex::Locker theLocker(mMutex);
    
    bool didStartIO = false;
    
    BGM_Client theClient;
    bool didFindClient = mClientMap.GetClientNonRT(inClientID, &theClient);
    
    ThrowIf(!didFindClient, BGM_InvalidClientException(), "BGM_Clients::StartIO: Cannot start IO for client that was never added");

    DebugMsg("BGM_Clients::StartIO: Client %u (%s) starting IO",
             inClientID,
             CFStringGetCStringPtr(theClient.mBundleID.GetCFString(), kCFStringEncodingUTF8));
    
    bool sendIsRunningNotification = false;
    bool sendIsRunningSomewhereOtherThanBGMAppNotification = false;

    if(!theClient.mDoingIO)
    {
        // Make sure we can start
        ThrowIf(mStartCount == UINT64_MAX, CAException(kAudioHardwareIllegalOperationError), "BGM_Clients::StartIO: failed to start because the ref count was maxxed out already");
        
        mClientMap.StartIONonRT(inClientID);
        
        mStartCount++;
        
        // Update mStartCountExcludingBGMApp
        if(!IsBGMApp(inClientID))
        {
            ThrowIf(mStartCountExcludingBGMApp == UINT64_MAX, CAException(kAudioHardwareIllegalOperationError), "BGM_Clients::StartIO: failed to start because mStartCountExcludingBGMApp was maxxed out already");
            
            mStartCountExcludingBGMApp++;
            
            if(mStartCountExcludingBGMApp == 1)
            {
                sendIsRunningSomewhereOtherThanBGMAppNotification = true;
            }
        }
        
        // Return true if no other clients were running IO before this one started, which means the device should start IO
        didStartIO = (mStartCount == 1);
        sendIsRunningNotification = didStartIO;
    }
    
    Assert(mStartCountExcludingBGMApp == mStartCount - 1 || mStartCountExcludingBGMApp == mStartCount,
           "mStartCount and mStartCountExcludingBGMApp are out of sync");
    
    SendIORunningNotifications(sendIsRunningNotification, sendIsRunningSomewhereOtherThanBGMAppNotification);

    return didStartIO;
}

bool    BGM_Clients::StopIONonRT(UInt32 inClientID)
{
    CAMutex::Locker theLocker(mMutex);
    
    bool didStopIO = false;
    
    BGM_Client theClient;
    bool didFindClient = mClientMap.GetClientNonRT(inClientID, &theClient);
    
    ThrowIf(!didFindClient, BGM_InvalidClientException(), "BGM_Clients::StopIO: Cannot stop IO for client that was never added");
    
    DebugMsg("BGM_Clients::StopIO: Client %u (%s) stopping IO",
             inClientID,
             CFStringGetCStringPtr(theClient.mBundleID.GetCFString(), kCFStringEncodingUTF8));
    
    bool sendIsRunningNotification = false;
    bool sendIsRunningSomewhereOtherThanBGMAppNotification = false;
    
    if(theClient.mDoingIO)
    {
        mClientMap.StopIONonRT(inClientID);
        
        ThrowIf(mStartCount <= 0, CAException(kAudioHardwareIllegalOperationError), "BGM_Clients::StopIO: Underflowed mStartCount");
        
        mStartCount--;
        
        // Update mStartCountExcludingBGMApp
        if(!IsBGMApp(inClientID))
        {
            ThrowIf(mStartCountExcludingBGMApp <= 0, CAException(kAudioHardwareIllegalOperationError), "BGM_Clients::StopIO: Underflowed mStartCountExcludingBGMApp");
            
            mStartCountExcludingBGMApp--;
            
            if(mStartCountExcludingBGMApp == 0)
            {
                sendIsRunningSomewhereOtherThanBGMAppNotification = true;
            }
        }
        
        // Return true if we stopped IO entirely (i.e. there are no clients still running IO)
        didStopIO = (mStartCount == 0);
        sendIsRunningNotification = didStopIO;
    }
    
    Assert(mStartCountExcludingBGMApp == mStartCount - 1 || mStartCountExcludingBGMApp == mStartCount,
           "mStartCount and mStartCountExcludingBGMApp are out of sync");
    
    SendIORunningNotifications(sendIsRunningNotification, sendIsRunningSomewhereOtherThanBGMAppNotification);
    
    return didStopIO;
}

bool    BGM_Clients::ClientsRunningIO() const
{
    return mStartCount > 0;
}

bool    BGM_Clients::ClientsOtherThanBGMAppRunningIO() const
{
    return mStartCountExcludingBGMApp > 0;
}

void    BGM_Clients::SendIORunningNotifications(bool sendIsRunningNotification, bool sendIsRunningSomewhereOtherThanBGMAppNotification) const
{
    if(sendIsRunningNotification || sendIsRunningSomewhereOtherThanBGMAppNotification)
    {
        AudioObjectPropertyAddress theChangedProperties[2];
        UInt32 theNotificationCount = 0;
        
        if(sendIsRunningNotification)
        {
            DebugMsg("BGM_Clients::SendIORunningNotifications: Sending kAudioDevicePropertyDeviceIsRunning");
            theChangedProperties[0] = { kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
            theNotificationCount++;
        }
        
        if(sendIsRunningSomewhereOtherThanBGMAppNotification)
        {
            DebugMsg("BGM_Clients::SendIORunningNotifications: Sending kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanBGMApp");
            theChangedProperties[theNotificationCount] = kBGMRunningSomewhereOtherThanBGMAppAddress;
            theNotificationCount++;
        }
        
        BGM_PlugIn::Host_PropertiesChanged(kObjectID_Device, theNotificationCount, theChangedProperties);
    }
}

#pragma mark Music Player

bool    BGM_Clients::SetMusicPlayer(const pid_t inPID)
{
    ThrowIf(inPID < 0, BGM_InvalidClientPIDException(), "BGM_Clients::SetMusicPlayer: Invalid music player PID");
    
    CAMutex::Locker theLocker(mMutex);
    
    if(mMusicPlayerProcessIDProperty == inPID)
    {
        // We're not changing the properties, so return false
        return false;
    }
    
    mMusicPlayerProcessIDProperty = inPID;
    // Unset the bundle ID property
    mMusicPlayerBundleIDProperty = "";
    
    DebugMsg("BGM_Clients::SetMusicPlayer: Setting music player by PID. inPID=%d", inPID);
    
    // Update the clients' mIsMusicPlayer fields
    mClientMap.UpdateMusicPlayerFlags(inPID);
    
    return true;
}

bool    BGM_Clients::SetMusicPlayer(const CACFString inBundleID)
{
    Assert(inBundleID.IsValid(), "BGM_Clients::SetMusicPlayer: Invalid CACFString given as bundle ID");
    
    CAMutex::Locker theLocker(mMutex);
    
    if(mMusicPlayerBundleIDProperty == inBundleID)
    {
        // We're not changing the properties, so return false
        return false;
    }
    
    mMusicPlayerBundleIDProperty = inBundleID;
    // Unset the PID property
    mMusicPlayerProcessIDProperty = 0;
    
    DebugMsg("BGM_Clients::SetMusicPlayer: Setting music player by bundle ID. inBundleID=%s",
             CFStringGetCStringPtr(inBundleID.GetCFString(), kCFStringEncodingUTF8));
    
    // Update the clients' mIsMusicPlayer fields
    mClientMap.UpdateMusicPlayerFlags(inBundleID);
    
    return true;
}

bool    BGM_Clients::IsMusicPlayerRT(const UInt32 inClientID)
{
    BGM_Client theClient;
    bool didGetClient = mClientMap.GetClientRT(inClientID, &theClient);
    return didGetClient && theClient.mIsMusicPlayer;
}

#pragma mark App Volumes

Float32 BGM_Clients::GetClientRelativeVolumeRT(UInt32 inClientID) const
{
    BGM_Client theClient;
    bool didGetClient = mClientMap.GetClientRT(inClientID, &theClient);
    return (didGetClient ? theClient.mRelativeVolume : 1.0);
}

bool    BGM_Clients::SetClientsRelativeVolumes(const CACFArray inAppVolumes)
{
    bool didChangeAppVolumes = false;
    
    // Each element in appVolumes is a CFDictionary containing the process id and/or bundle id of an app, and its
    // new relative volume
    for(int i = 0; i < inAppVolumes.GetNumberItems(); i++)
    {
        CACFDictionary theAppVolume(false);
        inAppVolumes.GetCACFDictionary(i, theAppVolume);
        
        // Get the app's PID from the dict
        pid_t theAppPID;
        bool didFindPID = theAppVolume.GetSInt32(CFSTR(kBGMAppVolumesKey_ProcessID), theAppPID);
        
        // Get the app's bundle ID from the dict
        CACFString theAppBundleID;
        theAppBundleID.DontAllowRelease();
        theAppVolume.GetCACFString(CFSTR(kBGMAppVolumesKey_BundleID), theAppBundleID);
        
        ThrowIf(!didFindPID && !theAppBundleID.IsValid(),
                BGM_InvalidClientRelativeVolumeException(),
                "BGM_Clients::SetClientsRelativeVolumes: App volume was sent without PID or bundle ID for app");
        
        Float32 theRelativeVolume;
        {
            SInt32 theRawRelativeVolume;
            bool didGetVolume = theAppVolume.GetSInt32(CFSTR(kBGMAppVolumesKey_RelativeVolume), theRawRelativeVolume);
            
            ThrowIf(!didGetVolume || theRawRelativeVolume < kAppRelativeVolumeMinRawValue || theRawRelativeVolume > kAppRelativeVolumeMaxRawValue,
                    BGM_InvalidClientRelativeVolumeException(),
                    "BGM_Clients::SetClientsRelativeVolumes: Relative volume for app missing or out of valid range");
            
            // Apply the volume curve to the raw volume
            //
            // mRelativeVolumeCurve uses the default kPow2Over1Curve transfer function, so we also multiply by 4 to
            // keep the middle volume equal to 1 (meaning apps' volumes are unchanged by default).
            theRelativeVolume = mRelativeVolumeCurve.ConvertRawToScalar(theRawRelativeVolume) * 4;
        }
        
        // Try to update the client's volume, first by PID and then, if that fails, by bundle ID
        //
        // TODO: Should we always try both in case an app has multiple clients?
        if(mClientMap.SetClientsRelativeVolume(theAppPID, theRelativeVolume))
        {
            didChangeAppVolumes = true;
        }
        else if(mClientMap.SetClientsRelativeVolume(theAppBundleID, theRelativeVolume))
        {
            didChangeAppVolumes = true;
        }
        else
        {
            // TODO: The app isn't currently a client, so we should add it to the past clients map, or update its
            //       past volume if it's already in there.
        }
    }
    
    return didChangeAppVolumes;
}
