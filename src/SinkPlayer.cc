/******************************************************************************
 * Copyright (c) 2013-2014, doubleTwist Corporation and AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/
#define __STDC_FORMAT_MACROS

#include <alljoyn/audio/SinkPlayer.h>

#include "Clock.h"
#include "Sink.h"
#include <alljoyn/audio/Audio.h>
#include <alljoyn/audio/AudioCodec.h>
#include <qcc/Debug.h>
#include <qcc/Mutex.h>
#include <qcc/Thread.h>
#include <algorithm>
#include <inttypes.h>


#define QCC_MODULE "ALLJOYN_AUDIO"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

using namespace ajn;
using namespace qcc;
using namespace std;

namespace ajn {
namespace services {
// ajn::BusAttachment* SinkPlayer::mMsgBus;

class FifoPositionHandler;

struct SinkInfo {
    enum {
        CLOSED = 0,
        OPENED,
    } mState;
    char* serviceName;
    SessionId sessionId;
    ProxyBusObject* portObj;
    ProxyBusObject* streamObj;
    uint32_t fifoSize;
    size_t numCapabilities;
    Capability* capabilities;
    AudioEncoder* encoder;
    Capability* selectedCapability;
    uint32_t framesPerPacket;
    FifoPositionHandler* fifoPositionHandler;
    uint32_t inputDataBytesRemaining;
    qcc::Mutex timestampMutex;
    uint64_t timestamp;
    int64_t offsettime;
};

struct FindSink {
    FindSink(const char* name) : name(name) { }
    bool operator()(const SinkInfo& sink) { return (strcmp(sink.serviceName, name) == 0); }
    const char* name;
};

struct GeneRic{
    int32_t offset;
    int32_t volume;
    int32_t result;
};

class SignallingObject : public BusObject {
  private:
    const InterfaceDescription::Member* mAudioDataMember;

    QStatus Get(const char* ifcName, const char* propName, MsgArg& val) {
        QStatus status = ER_OK;

        QCC_DbgTrace(("GetProperty called for %s.%s", ifcName, propName));

        if (0 == strcmp(ifcName, AUDIO_SOURCE_INTERFACE)) {
            if (0 == strcmp(propName, "Version")) {
                val.Set("q", INTERFACES_VERSION);
            } else {
                status = ER_BUS_NO_SUCH_PROPERTY;
            }
        } else {
            status = ER_BUS_NO_SUCH_INTERFACE;
        }

        if (status == ER_BUS_NO_SUCH_INTERFACE || status == ER_BUS_NO_SUCH_PROPERTY) {
            return BusObject::Get(ifcName, propName, val);
        }

        return status;
    }

  public:
    SignallingObject(BusAttachment* bus) : BusObject("/Player/Out/Audio") {
        const InterfaceDescription* audioSourceIntf = bus->GetInterface(AUDIO_SOURCE_INTERFACE);
        assert(audioSourceIntf);
        AddInterface(*audioSourceIntf);

        mAudioDataMember = audioSourceIntf->GetMember("Data");
        assert(mAudioDataMember);
    }

    QStatus EmitAudioDataSignal(SessionId sessionId, uint8_t* data, int32_t dataSize, uint64_t timestamp) {
        MsgArg args[2];
        args[0].Set("t", timestamp);
        args[1].Set("ay", dataSize, data);

        uint8_t flags = 0;
        QStatus status = Signal(NULL, sessionId, *mAudioDataMember, args, 2, 0, flags);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to emit Data signal"));
        }

        return status;
    }
};

class FifoPositionHandler : public MessageReceiver {
  public:
    FifoPositionHandler() : MessageReceiver() {
        mReadyToEmitEvent = new Event();
    }

    ~FifoPositionHandler() {
        delete mReadyToEmitEvent;
        mReadyToEmitEvent = NULL;
    }

    QStatus Register(BusAttachment* bus, const char* objectPath, SessionId sessionId) {
        const InterfaceDescription* audioSinkIntf = bus->GetInterface(AUDIO_SINK_INTERFACE);
        assert(audioSinkIntf);
        const InterfaceDescription::Member* fifoPositionChangedMember = audioSinkIntf->GetMember("FifoPositionChanged");
        assert(fifoPositionChangedMember);

        QStatus status = bus->RegisterSignalHandler(this,
                                                    static_cast<MessageReceiver::SignalHandler>(&FifoPositionHandler::FifoPositionChangedSignalHandler),
                                                    fifoPositionChangedMember, objectPath);
        if (status != ER_OK) {
            return status;
        }

        mReadyToEmitEvent->SetEvent();
        mSessionId = sessionId;

        return status;
    }

    void Unregister(BusAttachment* bus) {
        QStatus status = bus->UnregisterAllHandlers(this);
        if (status != ER_OK) {
            QCC_LogError(status, ("UnregisterAllHandlers failed"));
        }
    }

    QStatus WaitUntilReadyToEmit(uint32_t maxMs) {
        QStatus status = Event::Wait(*mReadyToEmitEvent, maxMs);
        if (status == ER_OK) {
            mReadyToEmitEvent->ResetEvent();
        }
        return status;
    }

  private:
    void FifoPositionChangedSignalHandler(const InterfaceDescription::Member* member,
                                          const char* sourcePath, Message& msg)
    {
        if (msg->GetSessionId() != mSessionId) {
            // Ignore signal intended for different handler
            return;
        }

        size_t numArgs = 0;
        const MsgArg* args = NULL;
        msg->GetArgs(numArgs, args);

        if (numArgs != 0) {
            QCC_LogError(ER_BAD_ARG_COUNT, ("FifoPositionChanged signal has invalid number of arguments"));
            return;
        }

        mReadyToEmitEvent->SetEvent();
    }

  private:
    Event* mReadyToEmitEvent;
    SessionId mSessionId;
};

class SinkSessionListener : public SessionListener {
  private:
    SinkPlayer* mSP;

  public:
    SinkSessionListener(SinkPlayer* sp) {
        mSP = sp;
    }

    void SessionLost(SessionId sessionId) {
        mSP->RemoveSink(sessionId, true);
    }
};

SinkPlayer::SinkPlayer(BusAttachment* msgBus)
    : MessageReceiver(), mSinkListenersMutex(new qcc::Mutex()), mDataSource(NULL), mDataSourceAnti(NULL),
    mSinksMutex(new qcc::Mutex()), mAddThreadsMutex(new qcc::Mutex()), mRemoveThreadsMutex(new qcc::Mutex()),
    mEmitThreadsMutex(new qcc::Mutex()), mSinkListenerThread(NULL) {
    mMsgBus = msgBus;
    mSessionListener = new SinkSessionListener(this);
    mPreferredFormat = strdup(MIMETYPE_AUDIO_RAW);
    mState = PlayerState::IDLE;

    QStatus status = msgBus->CreateInterfacesFromXml(INTERFACES_XML);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to create interfaces from XML"));
    } else {
        mSignallingObject = new SignallingObject(mMsgBus);
        mMsgBus->RegisterBusObject(*mSignallingObject);
    }

    const InterfaceDescription* volumeIntf = mMsgBus->GetInterface(VOLUME_INTERFACE);
    const InterfaceDescription::Member* muteChangedMember = volumeIntf ? volumeIntf->GetMember("MuteChanged") : NULL;
    const InterfaceDescription::Member* volumeChangedMember = volumeIntf ? volumeIntf->GetMember("VolumeChanged") : NULL;
    if (muteChangedMember) {
        status = mMsgBus->RegisterSignalHandler(this,
                                                static_cast<MessageReceiver::SignalHandler>(&SinkPlayer::MuteChangedSignalHandler),
                                                muteChangedMember, NULL);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to register MuteChanged signal handler"));
        }
    }
    if (volumeChangedMember) {
        status = mMsgBus->RegisterSignalHandler(this,
                                                static_cast<MessageReceiver::SignalHandler>(&SinkPlayer::VolumeChangedSignalHandler),
                                                volumeChangedMember, NULL);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to register VolumeChanged signal handler"));
        }
    }
}

SinkPlayer::~SinkPlayer() {
    RemoveAllSinks();

    if (mSignallingObject != NULL) {
        mMsgBus->UnregisterBusObject(*mSignallingObject);
        delete mSignallingObject;
        mSignallingObject = NULL;
    }

    if (mPreferredFormat != NULL) {
        free((void*)mPreferredFormat);
        mPreferredFormat = NULL;
    }

    if (mSessionListener != NULL) {
        delete mSessionListener;
        mSessionListener = NULL;
    }

    mMsgBus->UnregisterAllHandlers(this);

    delete mEmitThreadsMutex;
    delete mRemoveThreadsMutex;
    delete mAddThreadsMutex;
    delete mSinksMutex;
    delete mSinkListenersMutex;
}

bool SinkPlayer::SetDataSource(DataSource* theSource) {
    mSinksMutex->Lock();
    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        if (si->mState == SinkInfo::OPENED) {
            mSinksMutex->Unlock();
            QCC_LogError(ER_FAIL, ("Sinks must be closed before SetDataSource"));
            return false;
        }
    }
    mSinksMutex->Unlock();

    mDataSource = theSource;
    mState = PlayerState::INIT;

    return true;
}

bool SinkPlayer::SetDataSourceAnti(DataSource* theSource) {
    mSinksMutex->Lock();
    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        if (si->mState == SinkInfo::OPENED) {
            mSinksMutex->Unlock();
            QCC_LogError(ER_FAIL, ("Sinks must be closed before SetDataSource"));
            return false;
        }
    }
    mSinksMutex->Unlock();

    mDataSourceAnti = theSource;
    mState = PlayerState::INIT;

    return true;
}

bool SinkPlayer::SetPreferredFormat(const char* format) {
    if (!AudioEncoder::CanCreate(format)) {
        return false;
    }
    char* oldFormat = mPreferredFormat;
    mPreferredFormat = strdup(format);
    if (oldFormat != NULL) {
        free((void*)oldFormat);
    }
    return true;
}

void SinkPlayer::AddListener(SinkListener* listener) {
    mSinkListenersMutex->Lock();
    mSinkListeners.insert(listener);
    if (!mSinkListenerThread) {
        mSinkListenerThread = new Thread("SinkListener", &SinkListenerThread);
        mSinkListenerThread->Start(this);
    }
    mSinkListenersMutex->Unlock();
}

void SinkPlayer::RemoveListener(SinkListener* listener) {
    mSinkListenersMutex->Lock();
    SinkListeners::iterator it = mSinkListeners.find(listener);
    if (it != mSinkListeners.end()) {
        mSinkListeners.erase(it);
    }
    if (mSinkListeners.empty()) {
        mSinkListenerThread->Stop();
        mSinkListenerThread->Join();
        delete mSinkListenerThread;
        mSinkListenerThread = NULL;
    }
    mSinkListenersMutex->Unlock();
}

struct AddSinkInfo {
    char* name;
    SessionPort port;
    char* path;
    SinkPlayer* sp;
    AddSinkInfo() : name(NULL), path(NULL), sp(NULL) { }
};

bool SinkPlayer::AddSink(const char* name, SessionPort port, const char* path) {
    mSinksMutex->Lock();
    bool exists = find_if(mSinks.begin(), mSinks.end(), FindSink(name)) != mSinks.end();
    mSinksMutex->Unlock();
    if (exists) {
        QCC_LogError(ER_FAIL, ("AddSink error: already added"));
        return false;
    }

    mAddThreadsMutex->Lock();
    int count = mAddThreads.count(name);
    if (count > 0) {
        QCC_LogError(ER_FAIL, ("AddSink error: already being added"));
        mAddThreadsMutex->Unlock();
        return false;
    }

    AddSinkInfo* asi = new AddSinkInfo;
    asi->name = strdup(name);
    asi->path = strdup(path);
    asi->port = port;
    asi->sp = this;
    Thread* t = new Thread("AddSink", &AddSinkThread);
    mAddThreads[name] = t;
    mAddThreadsMutex->Unlock();
    t->Start(asi);

    return true;
}

struct EmitAudioInfo {
    SinkInfo* si;
    SinkPlayer* sp;
    EmitAudioInfo() : si(NULL), sp(NULL) { }
};

ThreadReturn SinkPlayer::AddSinkThread(void* arg) {
    AddSinkInfo* asi = reinterpret_cast<AddSinkInfo*>(arg);
    SinkPlayer* sp = asi->sp;

#define RETURN_ADD_FAILURE() { \
        sp->mSinkListenersMutex->Lock(); \
        SinkListeners::iterator it = sp->mSinkListeners.begin(); \
        while (it != sp->mSinkListeners.end()) { \
            SinkListener* listener = *it; \
            listener->SinkAddFailed(asi->name); \
            it = sp->mSinkListeners.upper_bound(listener); \
        } \
        sp->mSinkListenersMutex->Unlock(); \
        sp->FreeSinkInfo(&si); \
        if (asi->name != NULL) { \
            free((void*)asi->name); } \
        if (asi->path != NULL) { \
            free((void*)asi->path); } \
        delete asi; \
        return NULL; \
}

    SinkInfo si;
    memset(&si, 0, sizeof(si));

    QCC_DbgHLPrintf(("Joining session to %s", asi->name));
    SessionId sessionId;
    SessionOpts opts(SessionOpts::TRAFFIC_MESSAGES, false, SessionOpts::PROXIMITY_ANY, TRANSPORT_ANY);
    QStatus status = sp->mMsgBus->JoinSession(asi->name, asi->port, sp->mSessionListener, sessionId, opts);
    if (status == ER_OK) {
        QCC_DbgTrace(("JoinSession SUCCESS (Session id=%d)", sessionId));
    } else {
        QCC_LogError(status, ("JoinSession failed"));
        RETURN_ADD_FAILURE();
    }

    si.serviceName = strdup(asi->name);
    si.sessionId = sessionId;

    /* Stream interface */
    si.streamObj = new ProxyBusObject(*(sp->mMsgBus), si.serviceName, asi->path, si.sessionId);
    const InterfaceDescription* streamIntf = sp->mMsgBus->GetInterface(STREAM_INTERFACE);
    assert(streamIntf);
    si.streamObj->AddInterface(*streamIntf);
    const InterfaceDescription* clockIntf = sp->mMsgBus->GetInterface(CLOCK_INTERFACE);
    assert(clockIntf);
    si.streamObj->AddInterface(*clockIntf);

    sp->mSinksMutex->Lock();
    std::list<SinkInfo>::iterator sit = find_if(sp->mSinks.begin(), sp->mSinks.end(), FindSink(si.serviceName));
    if (sit != sp->mSinks.end()) {
        sp->mSinks.erase(sit);
    }
    sp->mSinks.push_back(si);
    sp->mSinksMutex->Unlock();

    sp->mAddThreadsMutex->Lock();
    sp->mAddThreads.erase(si.serviceName);
    sp->mAddThreadsMutex->Unlock();

    sp->mSinkListenersMutex->Lock();
    SinkListeners::iterator it = sp->mSinkListeners.begin();
    while (it != sp->mSinkListeners.end()) {
        SinkListener* listener = *it;
        listener->SinkAdded(si.serviceName);
        it = sp->mSinkListeners.upper_bound(listener);
    }
    sp->mSinkListenersMutex->Unlock();

    free((void*)asi->name);
    free((void*)asi->path);
    delete asi;

    return NULL;
}

bool SinkPlayer::addoffset(const char* name, int32_t offset){
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    SinkInfo* si = (it != mSinks.end()) ? &(*it) : NULL;

    si->offsettime += offset;
    // si->offsettime += (uint64_t)(offset * (double)(si->framesPerPacket / mDataSource->GetSampleRate()) * 1000000000);
    mSinksMutex->Unlock();

    // int64_t showdifftime =  (int64_t)(si->offsettime * (double)(si->framesPerPacket / mDataSource->GetSampleRate()) * 1000000);
    int64_t showdifftime = si->offsettime;
    // printf("The frame/packge is %lu\n",si->framesPerPacket);
    // printf("The frame/second is %lf\n",mDataSource->GetSampleRate());
   
    printf("The offset time is %lld ns\n", showdifftime);

    return true;

}

bool SinkPlayer::OpenSink(const char* name) {
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    SinkInfo* si = (it != mSinks.end()) ? &(*it) : NULL;
    mSinksMutex->Unlock();

    // printf("Smart audio says: Hello world!222 %s\n", name);
    if (!si) {
        QCC_LogError(ER_FAIL, ("OpenSink error: not found"));
        fprintf(stderr, "%s\n", name); fflush(stderr);
        return false;
    }

    /* Open the stream */
    Message openReply(*mMsgBus);
    QStatus status = si->streamObj->MethodCall(STREAM_INTERFACE, "Open", NULL, 0, openReply);
    if (status != ER_OK) {
        QCC_LogError(status, ("Stream.Open() failed"));
        return false;
    }

    /* Introspect */
    status = si->streamObj->IntrospectRemoteObject();
    if (status != ER_OK) {
        QCC_LogError(status, ("IntrospectRemoteObject(stream) failed"));
        return false;
    }

    size_t nChildren = si->streamObj->GetChildren(NULL);
    if (nChildren == 0) {
        QCC_LogError(ER_FAIL, ("Stream does not have any child objects"));
        return false;
    }

    ProxyBusObject** children = new ProxyBusObject *[nChildren];
    if (si->streamObj->GetChildren(children, nChildren) != nChildren) {
        QCC_LogError(ER_FAIL, ("Stream returned bad number of children"));
        delete[] children;
        return false;
    }

    for (size_t i = 0; i < nChildren; i++) {
        ProxyBusObject* child = children[i];

        status = child->IntrospectRemoteObject();
        if (status != ER_OK) {
            QCC_LogError(status, ("IntrospectRemoteObject(child) failed"));
            break;
        }

        if (child->ImplementsInterface(AUDIO_SINK_INTERFACE)) {
            si->portObj = child;
            break;
        }
    }

    delete[] children;

    if (si->portObj == NULL) {
        QCC_LogError(ER_FAIL, ("Stream does not have child object that implements AudioSink"));
        return false;
    }

    /* Get Capabilities */
    MsgArg capabilitiesReply;
    status = si->portObj->GetProperty(PORT_INTERFACE, "Capabilities", capabilitiesReply);
    if (status == ER_OK) {
        MSGARG_TO_CAPABILITIES(capabilitiesReply, si->capabilities, si->numCapabilities);
        //PRINT_CAPABILITIES(si->capabilities, si->numCapabilities);
    } else {
        QCC_LogError(status, ("GetProperty(Capabilities) failed"));
        return false;
    }

    Capability* capability = NULL;
    for (size_t i = 0; i < si->numCapabilities; i++) {
        if (si->capabilities[i].type == mPreferredFormat) {
            capability = &si->capabilities[i];
            break;
        } else if (si->capabilities[i].type == MIMETYPE_AUDIO_RAW) {
            capability = &si->capabilities[i];
        }
    }

    if (capability == NULL) {
        QCC_LogError(ER_FAIL, ("Sink does not even support raw format"));
        return false;
    }

    si->encoder = AudioEncoder::Create(capability->type.c_str());
    si->encoder->Configure(mDataSource);
    si->selectedCapability = new Capability;
    si->encoder->GetConfiguration(si->selectedCapability);
    si->framesPerPacket = si->encoder->GetFrameSize();

    MsgArg connectArgs[3];
    connectArgs[0].Set("s", ""); // host
    connectArgs[1].Set("o", "/"); // path
    Message connectReply(*mMsgBus);
    CAPABILITY_TO_MSGARG((*si->selectedCapability), connectArgs[2]);
    status = si->portObj->MethodCall(PORT_INTERFACE, "Connect", connectArgs, 3, connectReply);

    delete [] si->selectedCapability->parameters;
    si->selectedCapability->parameters = NULL;
    si->selectedCapability->numParameters = 0;

    if (status == ER_OK) {
        QCC_DbgTrace(("Port.Connect(%s) success", si->selectedCapability->type.c_str()));
    } else {
        QCC_LogError(status, ("Port.Connect() failed"));
        return false;
    }

    /* Get FifoSize */
    MsgArg fifoSizeReply;
    status = si->portObj->GetProperty(AUDIO_SINK_INTERFACE, "FifoSize", fifoSizeReply);
    if (status == ER_OK) {
        status = fifoSizeReply.Get("u", &si->fifoSize);
        if (status != ER_OK) {
            QCC_LogError(status, ("Bad FifoSize property"));
            return false;
        }
    } else {
        QCC_LogError(status, ("GetProperty(FifoSize) failed"));
        return false;
    }

    int64_t diffTime = 0;
    int64_t timelimit = 9000000;
    for (int i = 0; i < 5; i++) {
        uint64_t time = GetCurrentTimeNanos();
        MsgArg setTimeArgs[1];
        setTimeArgs[0].Set("t", time);
        Message setTimeReply(*mMsgBus);
        status = si->streamObj->MethodCall(CLOCK_INTERFACE, "SetTime", setTimeArgs, 1, setTimeReply);
        uint64_t newTime = GetCurrentTimeNanos();
        if (ER_OK == status) {
            QCC_DbgTrace(("Port.SetTime(%" PRIu64 ") success", time));
        } else {
            QCC_LogError(status, ("Port.SetTime() failed"));
            return false;
        }

        diffTime = (newTime - time) / 2;

        if (diffTime < timelimit) { // from 4ms to 9ms
            printf("The diff time is %lld ms\n", diffTime/1000000);
            break;
            
        }
        printf("It is the %d time, the diff time is %lld ms\n", i, diffTime/1000000);
        timelimit += 250000;
        /* Sleep for 1s and try again */
        SleepNanos(1000000000);
    }
    printf("The overall diff time is %lld ms\n", diffTime/1000000);

    MsgArg adjustTimeArgs[1];
    adjustTimeArgs[0].Set("x", diffTime);
    Message adjustTimeReply(*mMsgBus);
    status = si->streamObj->MethodCall(CLOCK_INTERFACE, "AdjustTime", adjustTimeArgs, 1, adjustTimeReply);
    if (ER_OK == status) {
        QCC_DbgHLPrintf(("Port.AdjustTime(%" PRId64 ") with %s succeeded", diffTime, si->serviceName));
    } else {
        QCC_LogError(status, ("Port.AdjustTime() with %s failed", si->serviceName));
        return false;
    }

    si->fifoPositionHandler = new FifoPositionHandler();
    status = si->fifoPositionHandler->Register(mMsgBus,
                                               si->portObj->GetPath().c_str(), si->sessionId);
    if (status != ER_OK) {
        QCC_LogError(status, ("FifoPositionHandler.Register() failed"));
        return false;
    }

    mSinksMutex->Lock();
    SinkInfo* fsi = NULL;
    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        if (it->mState == SinkInfo::OPENED) {
            fsi = &(*it);
            break;
        }
    }
    si->offsettime = 0; /* init the offset time */
    int fsiFlag = 0;
    if (!fsi) {
        /* Start from beginning if we're the first sink */
        si->inputDataBytesRemaining = mDataSource->GetInputSize();
        si->timestamp = GetCurrentTimeNanos() + 100000000; /* 0.1s */
        fsiFlag = 1;
        // si->timestamp = GetCurrentTimeNanos();
    } else {
        /* Start with values from first sink, note these are in the future due to semi-full fifo */
        fsi->timestampMutex.Lock();
        si->timestamp = fsi->timestamp;
        si->inputDataBytesRemaining = fsi->inputDataBytesRemaining;
        fsi->timestampMutex.Unlock();

        uint32_t inputDataBytesAvailable = mDataSource->GetInputSize() - si->inputDataBytesRemaining;
        uint32_t bytesPerSecond = mDataSource->GetSampleRate() * mDataSource->GetBytesPerFrame();
        uint32_t bytesDiff = ((double)(si->timestamp - GetCurrentTimeNanos()) / 1000000000) * bytesPerSecond;
        bytesDiff = MIN(bytesDiff, inputDataBytesAvailable);
        bytesDiff = bytesDiff * 0.90; /* Temporary to avoid sending outdated chunks */
        uint32_t inputPacketBytes = mDataSource->GetBytesPerFrame() * si->framesPerPacket;
        bytesDiff = bytesDiff - (bytesDiff % inputPacketBytes);

        /* Adjust values appropriately so that playback will start sooner on new sink */
        si->timestamp -= (uint64_t)(((double)bytesDiff / bytesPerSecond) * 1000000000);
        si->inputDataBytesRemaining += bytesDiff;
    }
    mSinksMutex->Unlock();

    if (mState == PlayerState::PLAYING) {
        EmitAudioInfo* eai = new EmitAudioInfo;
        eai->sp = this;

        mSinksMutex->Lock();
        std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(si->serviceName));
        eai->si = &(*it);
        mSinksMutex->Unlock();

        printf("servicename is %s\n", si->serviceName);

        mEmitThreadsMutex->Lock();
        Thread* t = new Thread("EmitAudio", &EmitAudioThread);
        mEmitThreads[si->serviceName] = t;
        t->Start(eai);
        if (fsiFlag == 1)
        {
            Thread* syn_t = new Thread("SynTime", &SyncTimeThread);
            mSyntThreads[si->serviceName] = syn_t;
            syn_t->Start(eai);
        }
        mEmitThreadsMutex->Unlock();
    }

    si->mState = SinkInfo::OPENED;
    return true;
}

/* copy start from here */
bool SinkPlayer::OpenSinkAnti(const char* name) {
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    SinkInfo* si = (it != mSinks.end()) ? &(*it) : NULL;
    mSinksMutex->Unlock();

    // printf("Smart audio says: Hello world!222 %s\n", name);
    if (!si) {
        QCC_LogError(ER_FAIL, ("OpenSink error: not found"));
        fprintf(stderr, "%s\n", name); fflush(stderr);
        return false;
    }

    /* Open the stream */
    Message openReply(*mMsgBus);
    QStatus status = si->streamObj->MethodCall(STREAM_INTERFACE, "Open", NULL, 0, openReply);
    if (status != ER_OK) {
        QCC_LogError(status, ("Stream.Open() failed"));
        return false;
    }

    /* Introspect */
    status = si->streamObj->IntrospectRemoteObject();
    if (status != ER_OK) {
        QCC_LogError(status, ("IntrospectRemoteObject(stream) failed"));
        return false;
    }

    size_t nChildren = si->streamObj->GetChildren(NULL);
    if (nChildren == 0) {
        QCC_LogError(ER_FAIL, ("Stream does not have any child objects"));
        return false;
    }

    ProxyBusObject** children = new ProxyBusObject *[nChildren];
    if (si->streamObj->GetChildren(children, nChildren) != nChildren) {
        QCC_LogError(ER_FAIL, ("Stream returned bad number of children"));
        delete[] children;
        return false;
    }

    for (size_t i = 0; i < nChildren; i++) {
        ProxyBusObject* child = children[i];

        status = child->IntrospectRemoteObject();
        if (status != ER_OK) {
            QCC_LogError(status, ("IntrospectRemoteObject(child) failed"));
            break;
        }

        if (child->ImplementsInterface(AUDIO_SINK_INTERFACE)) {
            si->portObj = child;
            break;
        }
    }

    delete[] children;

    if (si->portObj == NULL) {
        QCC_LogError(ER_FAIL, ("Stream does not have child object that implements AudioSink"));
        return false;
    }

    /* Get Capabilities */
    MsgArg capabilitiesReply;
    status = si->portObj->GetProperty(PORT_INTERFACE, "Capabilities", capabilitiesReply);
    if (status == ER_OK) {
        MSGARG_TO_CAPABILITIES(capabilitiesReply, si->capabilities, si->numCapabilities);
        //PRINT_CAPABILITIES(si->capabilities, si->numCapabilities);
    } else {
        QCC_LogError(status, ("GetProperty(Capabilities) failed"));
        return false;
    }

    Capability* capability = NULL;
    for (size_t i = 0; i < si->numCapabilities; i++) {
        if (si->capabilities[i].type == mPreferredFormat) {
            capability = &si->capabilities[i];
            break;
        } else if (si->capabilities[i].type == MIMETYPE_AUDIO_RAW) {
            capability = &si->capabilities[i];
        }
    }

    if (capability == NULL) {
        QCC_LogError(ER_FAIL, ("Sink does not even support raw format"));
        return false;
    }

    si->encoder = AudioEncoder::Create(capability->type.c_str());
    si->encoder->Configure(mDataSourceAnti);
    si->selectedCapability = new Capability;
    si->encoder->GetConfiguration(si->selectedCapability);
    si->framesPerPacket = si->encoder->GetFrameSize();

    MsgArg connectArgs[3];
    connectArgs[0].Set("s", ""); // host
    connectArgs[1].Set("o", "/"); // path
    Message connectReply(*mMsgBus);
    CAPABILITY_TO_MSGARG((*si->selectedCapability), connectArgs[2]);
    status = si->portObj->MethodCall(PORT_INTERFACE, "Connect", connectArgs, 3, connectReply);

    delete [] si->selectedCapability->parameters;
    si->selectedCapability->parameters = NULL;
    si->selectedCapability->numParameters = 0;

    if (status == ER_OK) {
        QCC_DbgTrace(("Port.Connect(%s) success", si->selectedCapability->type.c_str()));
    } else {
        QCC_LogError(status, ("Port.Connect() failed"));
        return false;
    }

    /* Get FifoSize */
    MsgArg fifoSizeReply;
    status = si->portObj->GetProperty(AUDIO_SINK_INTERFACE, "FifoSize", fifoSizeReply);
    if (status == ER_OK) {
        status = fifoSizeReply.Get("u", &si->fifoSize);
        if (status != ER_OK) {
            QCC_LogError(status, ("Bad FifoSize property"));
            return false;
        }
    } else {
        QCC_LogError(status, ("GetProperty(FifoSize) failed"));
        return false;
    }

    int64_t diffTime = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t time = GetCurrentTimeNanos();
        MsgArg setTimeArgs[1];
        setTimeArgs[0].Set("t", time);
        Message setTimeReply(*mMsgBus);
        status = si->streamObj->MethodCall(CLOCK_INTERFACE, "SetTime", setTimeArgs, 1, setTimeReply);
        uint64_t newTime = GetCurrentTimeNanos();
        if (ER_OK == status) {
            QCC_DbgTrace(("Port.SetTime(%" PRIu64 ") success", time));
        } else {
            QCC_LogError(status, ("Port.SetTime() failed"));
            return false;
        }

        diffTime = (newTime - time) / 2;
        if (diffTime < 4000000) { // 10ms to 4ms
            break;
        }

        /* Sleep for 1s and try again */
        SleepNanos(1000000000);
    }

    MsgArg adjustTimeArgs[1];
    adjustTimeArgs[0].Set("x", diffTime);
    Message adjustTimeReply(*mMsgBus);
    status = si->streamObj->MethodCall(CLOCK_INTERFACE, "AdjustTime", adjustTimeArgs, 1, adjustTimeReply);
    if (ER_OK == status) {
        QCC_DbgHLPrintf(("Port.AdjustTime(%" PRId64 ") with %s succeeded", diffTime, si->serviceName));
    } else {
        QCC_LogError(status, ("Port.AdjustTime() with %s failed", si->serviceName));
        return false;
    }

    si->fifoPositionHandler = new FifoPositionHandler();
    status = si->fifoPositionHandler->Register(mMsgBus,
                                               si->portObj->GetPath().c_str(), si->sessionId);
    if (status != ER_OK) {
        QCC_LogError(status, ("FifoPositionHandler.Register() failed"));
        return false;
    }

    mSinksMutex->Lock();
    SinkInfo* fsi = NULL;
    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        if (it->mState == SinkInfo::OPENED) {
            fsi = &(*it);
            break;
        }
    }
    if (!fsi) {
        /* Start from beginning if we're the first sink */
        si->inputDataBytesRemaining = mDataSourceAnti->GetInputSize();
        si->timestamp = GetCurrentTimeNanos() + 100000000; /* 0.1s */
    } else {
        /* Start with values from first sink, note these are in the future due to semi-full fifo */
        fsi->timestampMutex.Lock();
        si->timestamp = fsi->timestamp;
        si->inputDataBytesRemaining = fsi->inputDataBytesRemaining;
        fsi->timestampMutex.Unlock();

        uint32_t inputDataBytesAvailable = mDataSourceAnti->GetInputSize() - si->inputDataBytesRemaining;
        uint32_t bytesPerSecond = mDataSourceAnti->GetSampleRate() * mDataSourceAnti->GetBytesPerFrame();
        uint32_t bytesDiff = ((double)(si->timestamp - GetCurrentTimeNanos()) / 1000000000) * bytesPerSecond;
        bytesDiff = MIN(bytesDiff, inputDataBytesAvailable);
        bytesDiff = bytesDiff * 0.90; /* Temporary to avoid sending outdated chunks */
        uint32_t inputPacketBytes = mDataSourceAnti->GetBytesPerFrame() * si->framesPerPacket;
        bytesDiff = bytesDiff - (bytesDiff % inputPacketBytes);

        /* Adjust values appropriately so that playback will start sooner on new sink */
        si->timestamp -= (uint64_t)(((double)bytesDiff / bytesPerSecond) * 1000000000);
        si->inputDataBytesRemaining += bytesDiff;
    }
    mSinksMutex->Unlock();

    if (mState == PlayerState::PLAYING) {
        EmitAudioInfo* eai = new EmitAudioInfo;
        eai->sp = this;

        mSinksMutex->Lock();
        std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(si->serviceName));
        eai->si = &(*it);
        mSinksMutex->Unlock();

        mEmitThreadsMutex->Lock();
        Thread* t = new Thread("EmitAudio", &EmitAudioThreadAnti);
        mEmitThreads[si->serviceName] = t;
        t->Start(eai);
        mEmitThreadsMutex->Unlock();
    }

    si->mState = SinkInfo::OPENED;
    return true;
}
/*copy end here*/


struct RemoveSinkInfo {
    char* name;
    bool lost;
    SinkPlayer* sp;
    RemoveSinkInfo() : name(NULL), sp(NULL) { }
};

bool SinkPlayer::RemoveSink(const char* name) {
    return RemoveSink(name, false);
}

bool SinkPlayer::RemoveSink(ajn::SessionId sessionId, bool lost) {
    mSinksMutex->Lock();
    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        if (si->sessionId == sessionId) {
            RemoveSink(si->serviceName, lost);
            mSinksMutex->Unlock();
            return true;
        }
    }
    mSinksMutex->Unlock();

    return false;
}

bool SinkPlayer::RemoveSink(const char* name, bool lost) {
    mSinksMutex->Lock();
    bool exists = find_if(mSinks.begin(), mSinks.end(), FindSink(name)) != mSinks.end();
    mSinksMutex->Unlock();
    if (!exists) {
        QCC_LogError(ER_FAIL, ("RemoveSink error: not found"));
        return false;
    }

    mRemoveThreadsMutex->Lock();
    int count = mRemoveThreads.count(name);
    if (count > 0) {
        QCC_LogError(ER_FAIL, ("RemoveSink error: already being removed"));
        mRemoveThreadsMutex->Unlock();
        return false;
    }

    RemoveSinkInfo* rsi = new RemoveSinkInfo;
    rsi->name = strdup(name);
    rsi->lost = lost;
    rsi->sp = this;
    Thread* t = new Thread("RemoveSink", &RemoveSinkThread);
    mRemoveThreads[name] = t;
    mRemoveThreadsMutex->Unlock();
    t->Start(rsi);

    return true;
}

ThreadReturn SinkPlayer::RemoveSinkThread(void* arg) {
    RemoveSinkInfo* rsi = reinterpret_cast<RemoveSinkInfo*>(arg);
    SinkPlayer* sp = rsi->sp;

    sp->mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(sp->mSinks.begin(), sp->mSinks.end(), FindSink(rsi->name));
    SinkInfo* si = &(*it);
    QStatus status = sp->CloseSink(si, rsi->lost);
    sp->mSinksMutex->Unlock();
    if (status == ER_OK) {
        QCC_DbgTrace(("CloseSink success"));
    } else {
        QCC_LogError(status, ("CloseSink failed"));
    }

    if (!rsi->lost) {
        status = sp->mMsgBus->LeaveSession(si->sessionId);
        if (status == ER_OK) {
            QCC_DbgTrace(("LeaveSession success"));
        } else {
            QCC_LogError(status, ("LeaveSession failed"));
        }
    }

    sp->mSinksMutex->Lock();
    std::list<SinkInfo>::iterator sit = find_if(sp->mSinks.begin(), sp->mSinks.end(), FindSink(rsi->name));
    sp->mSinks.erase(sit);
    sp->mSinksMutex->Unlock();

    sp->FreeSinkInfo(si);

    sp->mRemoveThreadsMutex->Lock();
    sp->mRemoveThreads.erase(rsi->name);
    sp->mRemoveThreadsMutex->Unlock();

    sp->mSinkListenersMutex->Lock();
    SinkListeners::iterator lit = sp->mSinkListeners.begin();
    while (lit != sp->mSinkListeners.end()) {
        SinkListener* listener = *lit;
        listener->SinkRemoved(rsi->name, rsi->lost);
        lit = sp->mSinkListeners.upper_bound(listener);
    }
    sp->mSinkListenersMutex->Unlock();

    free((void*)rsi->name);
    delete rsi;

    return 0;
}

bool SinkPlayer::CloseSink(const char* name) {
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    if (it == mSinks.end()) {
        mSinksMutex->Unlock();
        QCC_LogError(ER_FAIL, ("CloseSink error: not found"));
        return false;
    }
    SinkInfo* si = &(*it);
    QStatus status = CloseSink(si);
    mSinksMutex->Unlock();
    if (status != ER_OK) {
        return false;
    }
    return true;
}

QStatus SinkPlayer::CloseSink(SinkInfo* si, bool lost) {
    Thread* t = NULL;
    Thread* syn_t = NULL;
    mEmitThreadsMutex->Lock();
    if (mEmitThreads.count(si->serviceName) > 0) {
        t = mEmitThreads[si->serviceName];
    }
    mEmitThreadsMutex->Unlock();

    if(mSyntThreads.count(si->serviceName) > 0) {
        syn_t = mSyntThreads[si->serviceName];
    }

    if (t != NULL) {
        t->Stop();
        t->Join();
    }

    if (syn_t != NULL)
    {
        syn_t->Stop();
        syn_t->Join();
    }

    mEmitThreadsMutex->Lock();
    mEmitThreads.erase(si->serviceName);
    mEmitThreadsMutex->Unlock();

    mSyntThreads.erase(si->serviceName);

    if (!lost) {
        Message closeReply(*mMsgBus);
        QStatus status = si->streamObj->MethodCall(STREAM_INTERFACE, "Close", NULL, 0, closeReply);
        if (status == ER_OK) {
            QCC_DbgTrace(("Stream.Close() success"));
        } else {
            QCC_LogError(status, ("Stream.Close() failed"));
        }
    }

    if (si->fifoPositionHandler != NULL) {
        si->fifoPositionHandler->Unregister(mMsgBus);
        delete si->fifoPositionHandler;
        si->fifoPositionHandler = NULL;
    }

    if (si->encoder != NULL) {
        delete si->encoder;
        si->encoder = NULL;
    }

    if (si->capabilities != NULL) {
        delete [] si->capabilities;
        si->capabilities = NULL;
    }

    if (si->selectedCapability != NULL) {
        delete si->selectedCapability;
        si->selectedCapability = NULL;
    }

    si->mState = SinkInfo::CLOSED;
    return ER_OK;
}

bool SinkPlayer::HasSink(const char* name) {
    mSinksMutex->Lock();
    bool has = find_if(mSinks.begin(), mSinks.end(), FindSink(name)) != mSinks.end();
    mSinksMutex->Unlock();
    return has;
}

bool SinkPlayer::RemoveAllSinks() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    if (count == 0) {
        mSinksMutex->Unlock();
        return false;
    }

    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        RemoveSink(si->serviceName, false);
    }

    mSinksMutex->Unlock();
    return true;
}

size_t SinkPlayer::GetSinkCount() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    mSinksMutex->Unlock();
    return count;
}

void SinkPlayer::ChangeVolume(int32_t myVolume){
    /* change colume */
    string setvol = "amixer cset numid=1 ";
    /*convert int to string*/
    char temp[10];
    sprintf(temp, "%d", myVolume);
    string myvol(temp);
    string finalcommand = setvol + myvol;
    /* adjust volume through cset function*/
    system(finalcommand.c_str());
    printf("\nThe volume has been adjusted to %s\n",myvol.c_str());
}

void SinkPlayer::GetNoise(CURL *curl, size_t &lastsize, size_t &lastestsize, string &diffBuffer){
    /* define the variables for HTTP GET from cloud */
    // CURLcode res; //return result-> false or success
    string micreadBuffer; //return value

    
    curl = curl_easy_init();

    lastsize = lastestsize;
    // printf("last size is%u\n", lastsize);
    curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.10.88:3000/channels/1/fields/1/last?key=5PTJZFXQ6SWD32PR");
    curl_easy_setopt(curl, CURLOPT_HTTPGET,1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &micreadBuffer);
    curl_easy_perform(curl);
    lastestsize = micreadBuffer.size();
    // printf("lastest size is%u\n", lastestsize);

    if (lastestsize > lastsize)
    {
        auto diffsize = lastestsize - lastsize;
        // printf("diff size is%u\n", diffsize);
        diffBuffer = micreadBuffer.substr(lastsize,diffsize);
        printf("The value of diffBuffer is %s",diffBuffer.c_str());
    }
}

bool SinkPlayer::CompareGene (const GeneRic& first, const GeneRic& second)
{
    if (first.result < second.result){
        return false;
    }
    else{
        return true;
    }
}

void SinkPlayer::StartExhaustion(SinkInfo* si, SinkPlayer* sp){
    /* init range of adjustment*/
    int32_t min_offset = -1000;
    int32_t max_offset = 1000;
    int32_t min_volume = 170;
    int32_t max_volume = 200;
    int32_t offsetStep = 40; //min is 2
    int32_t volumeStep = 10; //min is 1
    int32_t myoffset = 0, myvolume = 0;

    for (auto i = min_volume; i < max_volume; i += volumeStep)
    {
        myvolume = i;
        for (auto j = min_offset; j < max_offset; j += offsetStep)
        {
            myoffset = j;

            GeneRic gr;
            gr.volume = myvolume;
            gr.offset = myoffset;
            sp->mGenerics.push_back(gr);
        }
    }
}

void SinkPlayer::StartGeneric(SinkInfo* si, SinkPlayer* sp){
    /* init range of adjustment*/
    int32_t min_offset = -1000;
    int32_t max_offset = 1000;
    int32_t min_volume = 170;
    int32_t max_volume = 200;
    int32_t myoffset = 0, myvolume = 0;


    for (int i = 0; i < 9; ++i)
    {
        myoffset = min_offset + (rand() % (int)(max_offset - min_offset + 1));
        myvolume = min_volume + (rand() % (int)(max_volume - min_volume + 1));

        GeneRic gr;
        gr.volume = myvolume;
        gr.offset = myoffset;
        sp->mGenerics.push_back(gr);
    }
}

void SinkPlayer::GenerateNewGene(SinkInfo* si, SinkPlayer* sp){
    std::list<GeneRic> newGenerics;
    int32_t min_offset = -1000;
    int32_t max_offset = 1000;
    int32_t min_volume = 170;
    int32_t max_volume = 200;
    int32_t offsetStep = 40; //min is 2
    int32_t volumeStep = 10; //min is 1

    auto newgr = sp->mGenerics.begin();
    auto firstgr = sp->mGenerics.begin();
    newGenerics.push_back(*firstgr); //0 the best of init group
    /* cross first and second */
    auto secondgr = sp->mGenerics.begin() + 1;
    newgr->volume = secondgr->volume;
    newGenerics.push_back(*newgr); //1 cross 1
    newgr->volume = firstgr->volume;
    newgr->offset = secondgr->offset;
    newGenerics.push_back(*newgr); //2 cross 2
    /* Variation 4 genes */
    auto thirddgr = sp->mGenerics.begin() + 2;
    auto myoffset = min_offset + (rand() % (int)(max_offset - min_offset + 1));
    thirddgr->offset = myoffset;
    newGenerics.push_back(*thirddgr); //3 variation 1
    auto fourthdgr = sp->mGenerics.begin() + 3;
    myoffset = min_offset + (rand() % (int)(max_offset - min_offset + 1));
    fourthdgr->offset = myoffset;
    newGenerics.push_back(*fourthdgr); //4 variation 2
    auto fifthdgr = sp->mGenerics.begin() + 4;
    auto myvolume = min_volume + (rand() % (int)(max_volume - min_volume + 1));
    fifthdgr->volume = myvolume;
    newGenerics.push_back(*fifthdgr); //5 variation 3
    auto sixthdgr = sp->mGenerics.begin() + 5;
    myvolume = min_volume + (rand() % (int)(max_volume - min_volume + 1));
    sixthdgr->volume = myvolume;
    newGenerics.push_back(*sixthdgr); //6 variation 4
    /* import 3 new genes */
    for (int j = 0; j < 2; ++j)
    {
        myoffset = min_offset + (rand() % (int)(max_offset - min_offset + 1));
        myvolume = min_volume + (rand() % (int)(max_volume - min_volume + 1));
        newgr->volume = myvolume;
        newgr->offset = myoffset;
        newGenerics.push_back(*newgr); //17 18 19 new 1 2 3
    }
    /* copy the entile list back to mGenerics*/
    sp->mGenerics = newGenerics;
}

ThreadReturn SinkPlayer::SyncTimeThread(void* arg){
    
    EmitAudioInfo* eai = reinterpret_cast<EmitAudioInfo*>(arg);
    Thread* selfThread = Thread::GetThread();
    SinkPlayer* sp = eai->sp;
    SinkInfo* si = eai->si;
    QStatus status = ER_OK;

    ofstream myfile; //write data to myfile
    // myfile.open ("exhaustion.txt"); //1.exhaustion
    myfile.open ("generic.txt"); //2.generic
    size_t lastsize = 0;
    size_t lastestsize = 0;

    CURL *curl; //curl instance

    // SinkPlayer 
    //set time
    int64_t sumtime = 0;
    int64_t diffTime = 0;

    // sp->StartExhaustion(si, sp); //1.exhaustion
    sp->StartGeneric(si,sp); //2.generic
    auto gr = sp->mGenerics.begin();
    int initCount = 1;

    while(!selfThread->IsStopping() && si->inputDataBytesRemaining > 0){
        /* init the generic group */
        if (initCount % 10 != 0)
        {
            sp->ChangeVolume(gr->volume); // change volume

            /* change offset */
            sp->mSinksMutex->Lock();
            si->offsettime = gr->offset;
            sp->mSinksMutex->Unlock();
            printf("\nThe offset has been adjusted to%d\n", gr->offset);
            
            SleepNanos(6000000000); //6s
            /* get noise */
            string diffBuffer;
            sp->GetNoise(curl, lastsize,lastestsize,diffBuffer);
            gr->result = std::stoi(diffBuffer);
            // printf("The value of micreadBuffer is %s",micreadBuffer.c_str());

            /* write data to file*/
            myfile << gr->volume << "\t" << gr->offset << "\t" << gr->result << "\n";
            
            gr++;
            ++initCount;
        }
        if (initCount % 10 == 0)
        {
            /* sort the init result and generate now group */
            sp->mGenerics.sort(sp->CompareGene);
            auto firstgr = sp->mGenerics.begin();
            printf("The best result of init group is %d\n", firstgr->result);
            if (firstgr->result < 10 || (initCount/10) > 5)
            {
                /* if the object has been reached, set the best value and stop generic */
                sp->ChangeVolume(firstgr->volume); // change volume
                printf("The best volume is %d\n", firstgr->volume);
                /* change offset */
                sp->mSinksMutex->Lock();
                si->offsettime = gr->offset;
                sp->mSinksMutex->Unlock();
                printf("\nThe best offset is %d\n", firstgr->offset);

                break;
            }
            sp->GenerateNewGene(si,sp);
            ++initCount;
            gr = sp->mGenerics.begin();
        }
        
        for (int i = 0; i < 5; ++i)
        {
            uint64_t time = GetCurrentTimeNanos();
            MsgArg setTimeArgs[1];
            setTimeArgs[0].Set("t", time);
            Message setTimeReply(*sp->mMsgBus);
            status = si->streamObj->MethodCall(CLOCK_INTERFACE, "SetTime", setTimeArgs, 1, setTimeReply);
            uint64_t newTime = GetCurrentTimeNanos();
            if (ER_OK == status) {
                QCC_DbgTrace(("Port.SetTime(%" PRIu64 ") success", time));
            } else {
                QCC_LogError(status, ("Port.SetTime() failed"));
                return false;
            }

            diffTime = (newTime - time) / 2;
            if (diffTime < 10000000) { // 10ms
                break;
            }

            SleepNanos(1000000000);
        }
        printf("The transfer time is %lld ms\n", diffTime/1000000);
        
        // sumtime = (rand()%(addtime_max - addtime_min + 1)) + addtime_min;
        // float sumtime_f = sumtime/1000000;
        // printf("The random adj time is %f ms\n", sumtime_f);

        diffTime += sumtime;
        //adjust time
        MsgArg adjustTimeArgs[1];
        adjustTimeArgs[0].Set("x", diffTime);
        Message adjustTimeReply(*sp->mMsgBus);
        status = si->streamObj->MethodCall(CLOCK_INTERFACE, "AdjustTime", adjustTimeArgs, 1, adjustTimeReply);
        if (ER_OK == status) {
            QCC_DbgHLPrintf(("Port.AdjustTime(%" PRId64 ") with %s succeeded", diffTime, si->serviceName));
        } else {
            QCC_LogError(status, ("Port.AdjustTime() with %s failed", si->serviceName));
            return false;
        }


        // char setvol[300]="setvol.sh ";
        // strcat(setvol,"180");
        
    }

    curl_easy_cleanup(curl);
    myfile.close();
    
    return 0;
}

size_t SinkPlayer::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp){
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    // ((std::string*)userp) = append((char*)contents, size * nmemb);
    return size * nmemb;
}

ThreadReturn SinkPlayer::EmitAudioThread(void* arg) {
    EmitAudioInfo* eai = reinterpret_cast<EmitAudioInfo*>(arg);
    Thread* selfThread = Thread::GetThread();
    SinkPlayer* sp = eai->sp;
    SinkInfo* si = eai->si;
    QStatus status = ER_OK;

    uint32_t inputPacketBytes = sp->mDataSource->GetBytesPerFrame() * si->framesPerPacket;
    uint32_t bytesPerSecond = sp->mDataSource->GetSampleRate() * sp->mDataSource->GetBytesPerFrame();
    uint8_t* readBuffer = (uint8_t*)calloc(inputPacketBytes, 1);
    uint32_t bytesEmitted = 0;

    printf("the bytes per second is %d\n", bytesPerSecond);
    while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0 && (bytesEmitted + inputPacketBytes) <= si->fifoSize) {
        if (sp->mDataSource->IsDataReady()) {
            // int32_t numBytes = sp->mDataSource->ReadData(readBuffer, sp->mDataSource->GetInputSize() - si->inputDataBytesRemaining - (int32_t)(si->offsettime * bytesPerSecond / 1000000), inputPacketBytes);
            
            int32_t numBytes = sp->mDataSource->ReadData(readBuffer, sp->mDataSource->GetInputSize() - si->inputDataBytesRemaining - si->offsettime, inputPacketBytes);
            if (numBytes == 0) {            //EOF
                si->inputDataBytesRemaining = 0;
                break;
            }

            uint8_t* buffer = readBuffer;
            uint32_t numBytesToEmit = numBytes;
            si->encoder->Encode(&buffer, &numBytesToEmit);

            // printf("1.The offset time of %s is %d\n", si->serviceName, (int32_t)(si->offsettime * bytesPerSecond / 1000000));
            sp->mSignallingObject->EmitAudioDataSignal(si->sessionId, buffer, numBytesToEmit, si->timestamp);

            si->timestampMutex.Lock();
            si->timestamp += (uint64_t)(((double)numBytes / bytesPerSecond) * 1000000000);
            si->inputDataBytesRemaining -= numBytes;
            si->timestampMutex.Unlock();

            bytesEmitted += numBytes;

            QCC_DbgTrace(("Emitted %i bytes", numBytes));
        } else {       //Sleep for a few milli sec to wait for data ready again
            usleep(10 * 1000);
        }
    }

    while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0) {
        while (!selfThread->IsStopping()) {
            status = si->fifoPositionHandler->WaitUntilReadyToEmit(50);
            if (status == ER_OK) {
                break;
            }
        }

        if (status != ER_OK) {
            break;
        }

        // Get FifoPosition, try up to 15 times on timeout
        MsgArg fifoPositionReply;
        for (int i = 0; i < 15; i++) {
            fifoPositionReply.Clear();
            status = si->portObj->GetProperty(AUDIO_SINK_INTERFACE, "FifoPosition", fifoPositionReply);
            if (status != ER_TIMEOUT) {
                break;
            }
            SleepNanos(2 * 1000000000); // 2s
        }

        if (status != ER_OK) {
            QCC_LogError(status, ("GetProperty(FifoPosition) failed"));
            break;
        }

        uint32_t fifoPosition = 0;
        status = fifoPositionReply.Get("u", &fifoPosition);
        if (status != ER_OK) {
            QCC_LogError(status, ("Bad FifoPosition property"));
            break;
        }

        bytesEmitted = 0;
        uint32_t bytesToWrite = si->fifoSize - fifoPosition;

        while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0 && (bytesEmitted + inputPacketBytes) <= bytesToWrite) {
            if (sp->mDataSource->IsDataReady()) {
                int32_t numBytes = sp->mDataSource->ReadData(readBuffer, sp->mDataSource->GetInputSize() - si->inputDataBytesRemaining - si->offsettime, inputPacketBytes);
                if (numBytes == 0) {                //EOF
                    si->inputDataBytesRemaining = 0;
                    break;
                }

                uint8_t* buffer = readBuffer;
                uint32_t numBytesToEmit = numBytes;
                si->encoder->Encode(&buffer, &numBytesToEmit);

                uint64_t now = GetCurrentTimeNanos();
                if (si->timestamp < now) {
                    QCC_LogError(ER_WARNING, ("Skipping emit of audio that's outdated by %" PRIu64 " nanos", now - si->timestamp));
                } else {
                    // printf("2.The offset time of %s is %d\n", si->serviceName, (int32_t)(si->offsettime * bytesPerSecond / 1000000));
                    sp->mSignallingObject->EmitAudioDataSignal(si->sessionId, buffer, numBytesToEmit, si->timestamp + si->offsettime);
                    QCC_DbgTrace(("%d: timestamp %" PRIu64 " numBytes %d bytesPerSecond %d", si->sessionId, si->timestamp, numBytes, bytesPerSecond));
                    bytesEmitted += numBytes;
                    QCC_DbgTrace(("Emitted %i bytes", numBytes));
                }

                si->timestampMutex.Lock();
                si->timestamp += (uint64_t)(((double)numBytes / bytesPerSecond) * 1000000000);
                si->inputDataBytesRemaining -= numBytes;
                si->timestampMutex.Unlock();
            } else {           //Sleep for a few milli sec to wait for data ready again
                usleep(10 * 1000);
            }
        }
    }

    return 0;
}

/* copy start from here */
ThreadReturn SinkPlayer::EmitAudioThreadAnti(void* arg) {
    EmitAudioInfo* eai = reinterpret_cast<EmitAudioInfo*>(arg);
    Thread* selfThread = Thread::GetThread();
    SinkPlayer* sp = eai->sp;
    SinkInfo* si = eai->si;
    QStatus status = ER_OK;

    uint32_t inputPacketBytes = sp->mDataSourceAnti->GetBytesPerFrame() * si->framesPerPacket;
    uint32_t bytesPerSecond = sp->mDataSourceAnti->GetSampleRate() * sp->mDataSourceAnti->GetBytesPerFrame();
    uint8_t* readBuffer = (uint8_t*)calloc(inputPacketBytes, 1);
    uint32_t bytesEmitted = 0;

    while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0 && (bytesEmitted + inputPacketBytes) <= si->fifoSize) {
        if (sp->mDataSourceAnti->IsDataReady()) {
            int32_t numBytes = sp->mDataSourceAnti->ReadData(readBuffer, sp->mDataSourceAnti->GetInputSize() - si->inputDataBytesRemaining, inputPacketBytes);
            if (numBytes == 0) {            //EOF
                si->inputDataBytesRemaining = 0;
                break;
            }

            uint8_t* buffer = readBuffer;
            uint32_t numBytesToEmit = numBytes;
            si->encoder->Encode(&buffer, &numBytesToEmit);

            sp->mSignallingObject->EmitAudioDataSignal(si->sessionId, buffer, numBytesToEmit, si->timestamp);

            si->timestampMutex.Lock();
            si->timestamp += (uint64_t)(((double)numBytes / bytesPerSecond) * 1000000000);
            si->inputDataBytesRemaining -= numBytes;
            si->timestampMutex.Unlock();

            bytesEmitted += numBytes;

            QCC_DbgTrace(("Emitted %i bytes", numBytes));
        } else {       //Sleep for a few milli sec to wait for data ready again
            usleep(10 * 1000);
        }
    }

    while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0) {
        while (!selfThread->IsStopping()) {
            status = si->fifoPositionHandler->WaitUntilReadyToEmit(50);
            if (status == ER_OK) {
                break;
            }
        }

        if (status != ER_OK) {
            break;
        }

        // Get FifoPosition, try up to 15 times on timeout
        MsgArg fifoPositionReply;
        for (int i = 0; i < 15; i++) {
            fifoPositionReply.Clear();
            status = si->portObj->GetProperty(AUDIO_SINK_INTERFACE, "FifoPosition", fifoPositionReply);
            if (status != ER_TIMEOUT) {
                break;
            }
            SleepNanos(2 * 1000000000); // 2s
        }

        if (status != ER_OK) {
            QCC_LogError(status, ("GetProperty(FifoPosition) failed"));
            break;
        }

        uint32_t fifoPosition = 0;
        status = fifoPositionReply.Get("u", &fifoPosition);
        if (status != ER_OK) {
            QCC_LogError(status, ("Bad FifoPosition property"));
            break;
        }

        bytesEmitted = 0;
        uint32_t bytesToWrite = si->fifoSize - fifoPosition;

        while (!selfThread->IsStopping() && si->inputDataBytesRemaining > 0 && (bytesEmitted + inputPacketBytes) <= bytesToWrite) {
            if (sp->mDataSourceAnti->IsDataReady()) {
                int32_t numBytes = sp->mDataSourceAnti->ReadData(readBuffer, sp->mDataSourceAnti->GetInputSize() - si->inputDataBytesRemaining, inputPacketBytes);
                if (numBytes == 0) {                //EOF
                    si->inputDataBytesRemaining = 0;
                    break;
                }

                uint8_t* buffer = readBuffer;
                uint32_t numBytesToEmit = numBytes;
                si->encoder->Encode(&buffer, &numBytesToEmit);

                uint64_t now = GetCurrentTimeNanos();
                if (si->timestamp < now) {
                    QCC_LogError(ER_WARNING, ("Skipping emit of audio that's outdated by %" PRIu64 " nanos", now - si->timestamp));
                } else {
                    sp->mSignallingObject->EmitAudioDataSignal(si->sessionId, buffer, numBytesToEmit, si->timestamp);
                    QCC_DbgTrace(("%d: timestamp %" PRIu64 " numBytes %d bytesPerSecond %d", si->sessionId, si->timestamp, numBytes, bytesPerSecond));
                    bytesEmitted += numBytes;
                    QCC_DbgTrace(("Emitted %i bytes", numBytes));
                }

                si->timestampMutex.Lock();
                si->timestamp += (uint64_t)(((double)numBytes / bytesPerSecond) * 1000000000);
                si->inputDataBytesRemaining -= numBytes;
                si->timestampMutex.Unlock();
            } else {           //Sleep for a few milli sec to wait for data ready again
                usleep(10 * 1000);
            }
        }
    }

    return 0;
}

/* copy end here */


bool SinkPlayer::OpenAllSinks() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    if (count == 0) {
        mSinksMutex->Unlock();
        return false;
    }

    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        OpenSink(si->serviceName);
    }

    mSinksMutex->Unlock();
    return true;
}

bool SinkPlayer::OpenOneSink() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    if (count == 0) {
        mSinksMutex->Unlock();
        return false;
    }

    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        printf("%s\n", si->serviceName);
        OpenSink(si->serviceName);
        break;
    }

    mSinksMutex->Unlock();
    return true;
}

bool SinkPlayer::CloseAllSinks() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    if (count == 0) {
        mSinksMutex->Unlock();
        return false;
    }

    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        printf("%s\n", si->serviceName);
        CloseSink(si->serviceName);
    }

    mSinksMutex->Unlock();

    return true;
}

bool SinkPlayer::CloseOneSink() {
    mSinksMutex->Lock();
    int count = mSinks.size();
    if (count == 0) {
        mSinksMutex->Unlock();
        return false;
    }

    for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
        SinkInfo* si = &(*it);
        CloseSink(si->serviceName);
        break;
    }

    mSinksMutex->Unlock();

    return true;
}

bool SinkPlayer::IsPlaying() {
    return mState == PlayerState::PLAYING;
}

bool SinkPlayer::Play() {
    if (mState != PlayerState::PLAYING) {
        mSinksMutex->Lock();
        uint32_t inputDataBytesRemaining = 0;
        uint64_t timestamp = GetCurrentTimeNanos() + (mSinks.size() * 250000000); /* 0.25s */
        for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
            mEmitThreadsMutex->Lock();
            SinkInfo* si = &(*it);
            if (si->mState == SinkInfo::OPENED && mEmitThreads.count(si->serviceName) == 0) {
                Message playReply(*mMsgBus);
                QStatus status = si->portObj->MethodCall(AUDIO_SINK_INTERFACE, "Play", NULL, 0, playReply);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Play error"));
                }
                
                EmitAudioInfo* eai = new EmitAudioInfo;
                eai->si = si;
                eai->sp = this;
                Thread* t = new Thread("EmitAudio", &EmitAudioThread);
                Thread* syn_t = new Thread("SynTime", &SyncTimeThread);
                mEmitThreads[si->serviceName] = t;
                mSyntThreads[si->serviceName] = syn_t;
                if (inputDataBytesRemaining == 0) {
                    // Save value from first sink
                    inputDataBytesRemaining = si->inputDataBytesRemaining;
                } else {
                    // Apply to all other sinks
                    si->inputDataBytesRemaining = inputDataBytesRemaining;
                }
                si->timestamp = timestamp;
                t->Start(eai);
                syn_t->Start(eai);
            }
            mEmitThreadsMutex->Unlock();
        }
        mSinksMutex->Unlock();

        mState = PlayerState::PLAYING;

        uint64_t now = GetCurrentTimeNanos();
        if (now > timestamp) {
            QCC_DbgHLPrintf(("Play calls finished after timestamp by %" PRIu64 " nanos", now - timestamp));
        }
    }

    return true;
}

bool SinkPlayer::Pause() {
    if (mState == PlayerState::PLAYING) {
        mSinksMutex->Lock();
        uint64_t pauseTimeNanos = GetCurrentTimeNanos() + (mSinks.size() * 250000000); /* 0.25s */
        uint64_t flushTimeNanos = pauseTimeNanos + 1000000;
        for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
            mEmitThreadsMutex->Lock();
            SinkInfo* si = &(*it);
            if (si->mState == SinkInfo::OPENED && mEmitThreads.count(si->serviceName) > 0) {
                Message pauseReply(*mMsgBus);
                MsgArg pauseArgs("t", pauseTimeNanos);
                QStatus status = si->portObj->MethodCall(AUDIO_SINK_INTERFACE, "Pause", &pauseArgs, 1, pauseReply);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Pause error"));
                }

                mEmitThreadsMutex->Lock();
                Thread* t = mEmitThreads[si->serviceName];
                mEmitThreadsMutex->Unlock();
                t->Stop();
                t->Join();
                mEmitThreadsMutex->Lock();
                mEmitThreads.erase(si->serviceName);
                mEmitThreadsMutex->Unlock();

                Message flushReply(*mMsgBus);
                MsgArg flushArgs("t", flushTimeNanos);
                status = si->portObj->MethodCallAsync(AUDIO_SINK_INTERFACE, "Flush",
                                                      this, static_cast<MessageReceiver::ReplyHandler>(&SinkPlayer::FlushReplyHandler),
                                                      &flushArgs, 1, si);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Flush error"));
                }
            }
            mEmitThreadsMutex->Unlock();
        }
        mSinksMutex->Unlock();

        mState = PlayerState::PAUSED;

        uint64_t now = GetCurrentTimeNanos();
        if (now > pauseTimeNanos) {
            QCC_DbgHLPrintf(("Pause calls finished after timestamp by %" PRIu64 " nanos", now - pauseTimeNanos));
        }
    }

    return true;
}

bool SinkPlayer::GetVolumeRange(const char* name, int16_t& low, int16_t& high, int16_t& step) {
    uint64_t begin, end;
    bool success = false;
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    if (it != mSinks.end()) {
        SinkInfo* si = &(*it);
        MsgArg reply;
        begin = GetCurrentTimeNanos();
        QStatus status = si->portObj->GetProperty(VOLUME_INTERFACE, "VolumeRange", reply);
        end = GetCurrentTimeNanos();
        if (status == ER_OK) {
            status = reply.Get("(nnn)", &low, &high, &step);
        }
        if (status == ER_OK) {
            success = true;
            QCC_DbgHLPrintf(("Get volume range took %" PRIu64 " ns", end - begin));
        } else {
            success = false;
            QCC_LogError(status, ("Get volume range error"));
        }
    }
    mSinksMutex->Unlock();

    return success;
}

bool SinkPlayer::GetVolume(const char* name, int16_t& volume) {
    uint64_t begin, end;
    bool success = false;
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    if (it != mSinks.end()) {
        SinkInfo* si = &(*it);
        MsgArg reply;
        begin = GetCurrentTimeNanos();
        QStatus status = si->portObj->GetProperty(VOLUME_INTERFACE, "Volume", reply);
        end = GetCurrentTimeNanos();
        if (status == ER_OK) {
            status = reply.Get("n", &volume);
        }
        if (status == ER_OK) {
            success = true;
            QCC_DbgHLPrintf(("Get volume took %" PRIu64 " ns", end - begin));
        } else {
            success = false;
            QCC_LogError(status, ("Get volume error"));
        }
    }
    mSinksMutex->Unlock();

    return success;
}

bool SinkPlayer::SetVolume(const char* name, int16_t volume) {
    uint64_t begin, end;
    bool success = false;
    mSinksMutex->Lock();
    std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
    if (it != mSinks.end()) {
        SinkInfo* si = &(*it);
        MsgArg arg("n", volume);
        begin = GetCurrentTimeNanos();
        QStatus status = si->portObj->SetProperty(VOLUME_INTERFACE, "Volume", arg);
        end = GetCurrentTimeNanos();
        if (status == ER_OK) {
            success = true;
            QCC_DbgHLPrintf(("Set volume took %" PRIu64 " ns", end - begin));
        } else {
            success = false;
            QCC_LogError(status, ("Set volume error"));
        }
    }
    mSinksMutex->Unlock();

    return success;
}

bool SinkPlayer::GetMute(const char* name, bool& mute) {
    uint64_t begin, end;
    bool success = false;
    mSinksMutex->Lock();
    if (name) {
        std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
        if (it != mSinks.end()) {
            SinkInfo* si = &(*it);
            MsgArg reply;
            begin = GetCurrentTimeNanos();
            QStatus status = si->portObj->GetProperty(VOLUME_INTERFACE, "Mute", reply);
            end = GetCurrentTimeNanos();
            if (status == ER_OK) {
                status = reply.Get("b", &mute);
            }
            if (status == ER_OK) {
                success = true;
                QCC_DbgHLPrintf(("Get mute took %" PRIu64 " ns", end - begin));
            } else {
                success = false;
                QCC_LogError(status, ("Get mute error"));
            }
        }
    } else if (!mSinks.empty()) {
        success = true;
        mute = true;
        for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
            SinkInfo* si = &(*it);
            bool m;
            MsgArg reply;
            begin = GetCurrentTimeNanos();
            QStatus status = si->portObj->GetProperty(VOLUME_INTERFACE, "Mute", reply);
            end = GetCurrentTimeNanos();
            if (status == ER_OK) {
                status = reply.Get("b", &m);
            }
            if (status == ER_OK) {
                mute = mute && m;
                QCC_DbgHLPrintf(("Get mute took %" PRIu64 " ns", end - begin));
            } else {
                success = false;
                QCC_LogError(status, ("Get mute error"));
            }
        }
    }
    mSinksMutex->Unlock();

    return success;
}

bool SinkPlayer::SetMute(const char* name, bool mute) {
    uint64_t begin, end;
    bool success = false;
    mSinksMutex->Lock();
    if (name) {
        std::list<SinkInfo>::iterator it = find_if(mSinks.begin(), mSinks.end(), FindSink(name));
        if (it != mSinks.end()) {
            SinkInfo* si = &(*it);
            MsgArg arg("b", mute);
            begin = GetCurrentTimeNanos();
            QStatus status = si->portObj->SetProperty(VOLUME_INTERFACE, "Mute", arg);
            end = GetCurrentTimeNanos();
            if (status == ER_OK) {
                success = true;
                QCC_DbgHLPrintf(("Set mute took %" PRIu64 " ns", end - begin));
            } else {
                success = false;
                QCC_LogError(status, ("Set mute error"));
            }
            success = (status == ER_OK);
        }
    } else if (!mSinks.empty()) {
        success = true;
        for (std::list<SinkInfo>::iterator it = mSinks.begin(); it != mSinks.end(); ++it) {
            SinkInfo* si = &(*it);
            MsgArg arg("b", mute);
            begin = GetCurrentTimeNanos();
            QStatus status = si->portObj->SetProperty(VOLUME_INTERFACE, "Mute", arg);
            end = GetCurrentTimeNanos();
            if (status == ER_OK) {
                QCC_DbgHLPrintf(("Set mute took %" PRIu64 " ns", end - begin));
            } else {
                success = false;
                QCC_LogError(status, ("Set mute error"));
            }
        }
    }
    mSinksMutex->Unlock();

    return success;
}

void SinkPlayer::FlushReplyHandler(Message& msg, void* context) {
    size_t numArgs = 0;
    const MsgArg* args = NULL;
    msg->GetArgs(numArgs, args);

    if (numArgs != 1) {
        QCC_LogError(ER_BAD_ARG_COUNT, ("Flush reply has invalid number of arguments"));
        return;
    }

    if (mState != PlayerState::PAUSED) {
        QCC_DbgHLPrintf(("Ignoring flush reply as state is not paused"));
        return;
    }

    SinkInfo* si = reinterpret_cast<SinkInfo*>(context);

    uint32_t inputPacketBytes = mDataSource->GetBytesPerFrame() * si->framesPerPacket;
    uint32_t flushedBytes = msg->GetArg(0)->v_uint32;
    flushedBytes = flushedBytes - (flushedBytes % inputPacketBytes);
    if (si->inputDataBytesRemaining + flushedBytes < mDataSource->GetInputSize()) {
        /* Adjust value so that when playback is resumed we resend flushed data */
        si->inputDataBytesRemaining += flushedBytes;
    } else {
        si->inputDataBytesRemaining = mDataSource->GetInputSize();
    }
}

void SinkPlayer::FreeSinkInfo(SinkInfo* si) {
    if (si->serviceName != NULL) {
        free((void*)si->serviceName);
        si->serviceName = NULL;
    }

    if (si->streamObj != NULL) {
        delete si->streamObj;
        si->streamObj = NULL;
    }

    if (si->encoder != NULL) {
        delete si->encoder;
        si->encoder = NULL;
    }

    if (si->capabilities != NULL) {
        delete [] si->capabilities;
        si->capabilities = NULL;
    }

    if (si->selectedCapability != NULL) {
        delete si->selectedCapability;
        si->selectedCapability = NULL;
    }

    if (si->fifoPositionHandler != NULL) {
        delete si->fifoPositionHandler;
        si->fifoPositionHandler = NULL;
    }
}

void SinkPlayer::MuteChangedSignalHandler(const InterfaceDescription::Member* member,
                                          const char* sourcePath, Message& msg) {
    mSinkListenersMutex->Lock();
    if (mSinkListenerThread) {
        mSinkListenerQueue.push_back(msg);
        mSinkListenerThread->Alert();
    }
    mSinkListenersMutex->Unlock();
}

void SinkPlayer::VolumeChangedSignalHandler(const InterfaceDescription::Member* member,
                                            const char* sourcePath, Message& msg) {
    mSinkListenersMutex->Lock();
    if (mSinkListenerThread) {
        mSinkListenerQueue.push_back(msg);
        mSinkListenerThread->Alert();
    }
    mSinkListenersMutex->Unlock();
}

ThreadReturn SinkPlayer::SinkListenerThread(void* arg) {
    SinkPlayer* sp = reinterpret_cast<SinkPlayer*>(arg);
    Thread* t = Thread::GetThread();

    while (t->IsRunning()) {
        QStatus status = Event::Wait(Event::neverSet);
        if (ER_ALERTED_THREAD == status) {
            t->GetStopEvent().ResetEvent();

            sp->mSinkListenersMutex->Lock();
            while (!sp->mSinkListenerQueue.empty()) {
                Message msg = sp->mSinkListenerQueue.front();
                sp->mSinkListenerQueue.pop_front();
                sp->mSinkListenersMutex->Unlock();

                SinkInfo si;
                sp->mSinksMutex->Lock();
                std::list<SinkInfo>::iterator sit;
                for (sit = sp->mSinks.begin(); sit != sp->mSinks.end(); ++sit) {
                    si = (*sit);
                    if (si.portObj->GetPath() == msg->GetObjectPath() && si.sessionId == msg->GetSessionId()) {
                        break;
                    }
                }
                if (sit == sp->mSinks.end()) {
                    // Ignore signal from unknown sink
                    sp->mSinksMutex->Unlock();
                    sp->mSinkListenersMutex->Lock();
                    continue;
                }
                sp->mSinksMutex->Unlock();

                if (strcmp("MuteChanged", msg->GetMemberName()) == 0) {
                    bool mute;
                    QStatus status = msg->GetArg(0)->Get("b", &mute);
                    if (ER_OK != status) {
                        QCC_LogError(status, ("MuteChanged signal has invalid argument"));
                        sp->mSinkListenersMutex->Lock();
                        continue;
                    }

                    sp->mSinkListenersMutex->Lock();
                    SinkListeners::iterator it = sp->mSinkListeners.begin();
                    while (it != sp->mSinkListeners.end()) {
                        SinkListener* listener = *it;
                        listener->MuteChanged(si.serviceName, mute);
                        it = sp->mSinkListeners.upper_bound(listener);
                    }
                    sp->mSinkListenersMutex->Unlock();

                } else if (strcmp("VolumeChanged", msg->GetMemberName()) == 0) {
                    int16_t volume;
                    QStatus status = msg->GetArg(0)->Get("n", &volume);
                    if (ER_OK != status) {
                        QCC_LogError(status, ("VolumeChanged signal has invalid argument"));
                        sp->mSinkListenersMutex->Lock();
                        continue;
                    }

                    sp->mSinkListenersMutex->Lock();
                    SinkListeners::iterator it = sp->mSinkListeners.begin();
                    while (it != sp->mSinkListeners.end()) {
                        SinkListener* listener = *it;
                        listener->VolumeChanged(si.serviceName, volume);
                        it = sp->mSinkListeners.upper_bound(listener);
                    }
                    sp->mSinkListenersMutex->Unlock();
                }

                sp->mSinkListenersMutex->Lock();
            }
            sp->mSinkListenersMutex->Unlock();
        }
    }

    return 0;
}

}
}
