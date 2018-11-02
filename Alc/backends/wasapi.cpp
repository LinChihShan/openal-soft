/**
 * OpenAL cross platform audio library
 * Copyright (C) 2011 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include <wtypes.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cguid.h>
#include <devpropdef.h>
#include <mmreg.h>
#include <propsys.h>
#include <propkey.h>
#include <devpkey.h>
#ifndef _WAVEFORMATEXTENSIBLE_
#include <ks.h>
#include <ksmedia.h>
#endif

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

#include "alMain.h"
#include "alu.h"
#include "ringbuffer.h"
#include "compat.h"
#include "alstring.h"
#include "converter.h"

#include "backends/base.h"


extern "C" {
extern const GUID KSDATAFORMAT_SUBTYPE_PCM;
extern const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
} // extern "C"

DEFINE_DEVPROPKEY(DEVPKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80,0x20, 0x67,0xd1,0x46,0xa8,0x50,0xe0, 14);
DEFINE_PROPERTYKEY(PKEY_AudioEndpoint_FormFactor, 0x1da5d803, 0xd492, 0x4edd, 0x8c,0x23, 0xe0,0xc0,0xff,0xee,0x7f,0x0e, 0);
DEFINE_PROPERTYKEY(PKEY_AudioEndpoint_GUID, 0x1da5d803, 0xd492, 0x4edd, 0x8c, 0x23,0xe0, 0xc0,0xff,0xee,0x7f,0x0e, 4 );

#define MONO SPEAKER_FRONT_CENTER
#define STEREO (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT)
#define QUAD (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X5DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X5DOT1REAR (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X6DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_CENTER|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1_WIDE (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_FRONT_LEFT_OF_CENTER|SPEAKER_FRONT_RIGHT_OF_CENTER)

#define REFTIME_PER_SEC ((REFERENCE_TIME)10000000)

#define DEVNAME_HEAD "OpenAL Soft on "


/* Scales the given value using 64-bit integer math, ceiling the result. */
static inline ALint64 ScaleCeil(ALint64 val, ALint64 new_scale, ALint64 old_scale)
{
    return (val*new_scale + old_scale-1) / old_scale;
}


namespace {

struct DevMap {
    std::string name;
    std::string endpoint_guid; // obtained from PKEY_AudioEndpoint_GUID , set to "Unknown device GUID" if absent.
    std::wstring devid;

    template<typename T0, typename T1, typename T2>
    DevMap(T0&& name_, T1&& guid_, T2&& devid_)
      : name{std::forward<T0>(name_)}
      , endpoint_guid{std::forward<T1>(guid_)}
      , devid{std::forward<T2>(devid_)}
    { }
};

bool checkName(const std::vector<DevMap> &list, const std::string &name)
{
    return std::find_if(list.cbegin(), list.cend(),
        [&name](const DevMap &entry) -> bool
        { return entry.name == name; }
    ) != list.cend();
}

std::vector<DevMap> PlaybackDevices;
std::vector<DevMap> CaptureDevices;


std::string wstr_to_string(const WCHAR *wstr)
{
    std::string ret;

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if(len > 0)
    {
        ret.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &ret[0], len, nullptr, nullptr);
        ret.pop_back();
    }

    return ret;
}


HANDLE ThreadHdl;
DWORD ThreadID;

struct ThreadRequest {
    HANDLE FinishedEvt;
    HRESULT result;
};

} // namespace


#define WM_USER_First       (WM_USER+0)
#define WM_USER_OpenDevice  (WM_USER+0)
#define WM_USER_ResetDevice (WM_USER+1)
#define WM_USER_StartDevice (WM_USER+2)
#define WM_USER_StopDevice  (WM_USER+3)
#define WM_USER_CloseDevice (WM_USER+4)
#define WM_USER_Enumerate   (WM_USER+5)
#define WM_USER_Last        (WM_USER+5)

static const char MessageStr[WM_USER_Last+1-WM_USER][20] = {
    "Open Device",
    "Reset Device",
    "Start Device",
    "Stop Device",
    "Close Device",
    "Enumerate Devices",
};

static inline void ReturnMsgResponse(ThreadRequest *req, HRESULT res)
{
    req->result = res;
    SetEvent(req->FinishedEvt);
}

static HRESULT WaitForResponse(ThreadRequest *req)
{
    if(WaitForSingleObject(req->FinishedEvt, INFINITE) == WAIT_OBJECT_0)
        return req->result;
    ERR("Message response error: %lu\n", GetLastError());
    return E_FAIL;
}


using NameGUIDPair = std::pair<std::string,std::string>;
static NameGUIDPair get_device_name_and_guid(IMMDevice *device)
{
    std::string name{DEVNAME_HEAD};

    IPropertyStore *ps;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &ps);
    if(FAILED(hr))
    {
        WARN("OpenPropertyStore failed: 0x%08lx\n", hr);
        return { name+"Unknown Device Name", "Unknown Device GUID" };
    }

    PROPVARIANT pvname;
    PropVariantInit(&pvname);

    hr = ps->GetValue(reinterpret_cast<const PROPERTYKEY&>(DEVPKEY_Device_FriendlyName), &pvname);
    if(FAILED(hr))
    {
        WARN("GetValue Device_FriendlyName failed: 0x%08lx\n", hr);
        name += "Unknown Device Name";
    }
    else if(pvname.vt == VT_LPWSTR)
        name += wstr_to_string(pvname.pwszVal);
    else
    {
        WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvname.vt);
        name += "Unknown Device Name";
    }
    PropVariantClear(&pvname);

    std::string guid;
    PROPVARIANT pvguid;
    PropVariantInit(&pvguid);

    hr = ps->GetValue(reinterpret_cast<const PROPERTYKEY&>(PKEY_AudioEndpoint_GUID), &pvguid);
    if(FAILED(hr))
    {
        WARN("GetValue AudioEndpoint_GUID failed: 0x%08lx\n", hr);
        guid = "Unknown Device GUID";
    }
    else if(pvname.vt == VT_LPWSTR)
        guid = wstr_to_string(pvname.pwszVal);
    else
    {
        WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvguid.vt);
        guid = "Unknown Device GUID";
    }

    PropVariantClear(&pvguid);
    ps->Release();

    return {name, guid};
}

static void get_device_formfactor(IMMDevice *device, EndpointFormFactor *formfactor)
{
    IPropertyStore *ps;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &ps);
    if(FAILED(hr))
    {
        WARN("OpenPropertyStore failed: 0x%08lx\n", hr);
        return;
    }

    PROPVARIANT pvform;
    PropVariantInit(&pvform);

    hr = ps->GetValue(reinterpret_cast<const PROPERTYKEY&>(PKEY_AudioEndpoint_FormFactor), &pvform);
    if(FAILED(hr))
        WARN("GetValue AudioEndpoint_FormFactor failed: 0x%08lx\n", hr);
    else if(pvform.vt == VT_UI4)
        *formfactor = static_cast<EndpointFormFactor>(pvform.ulVal);
    else if(pvform.vt == VT_EMPTY)
        *formfactor = UnknownFormFactor;
    else
        WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvform.vt);

    PropVariantClear(&pvform);
    ps->Release();
}


static void add_device(IMMDevice *device, const WCHAR *devid, std::vector<DevMap> &list)
{
    std::string basename, guidstr;
    std::tie(basename, guidstr) = get_device_name_and_guid(device);

    int count{1};
    std::string newname{basename};
    while(checkName(PlaybackDevices, newname))
    {
        newname = basename;
        newname += " #";
        newname += std::to_string(++count);
    }
    list.emplace_back(std::move(newname), std::move(guidstr), devid);
    const DevMap &newentry = list.back();

    TRACE("Got device \"%s\", \"%s\", \"%ls\"\n", newentry.name.c_str(),
          newentry.endpoint_guid.c_str(), newentry.devid.c_str());
}

static WCHAR *get_device_id(IMMDevice *device)
{
    WCHAR *devid;

    HRESULT hr = device->GetId(&devid);
    if(FAILED(hr))
    {
        ERR("Failed to get device id: %lx\n", hr);
        return nullptr;
    }

    return devid;
}

static HRESULT probe_devices(IMMDeviceEnumerator *devenum, EDataFlow flowdir, std::vector<DevMap> &list)
{
    IMMDeviceCollection *coll;
    HRESULT hr = devenum->EnumAudioEndpoints(flowdir, DEVICE_STATE_ACTIVE, &coll);
    if(FAILED(hr))
    {
        ERR("Failed to enumerate audio endpoints: 0x%08lx\n", hr);
        return hr;
    }

    IMMDevice *defdev{nullptr};
    WCHAR *defdevid{nullptr};
    UINT count{0};
    hr = coll->GetCount(&count);
    if(SUCCEEDED(hr) && count > 0)
    {
        list.clear();
        list.reserve(count);

        hr = devenum->GetDefaultAudioEndpoint(flowdir, eMultimedia, &defdev);
    }
    if(SUCCEEDED(hr) && defdev != nullptr)
    {
        defdevid = get_device_id(defdev);
        if(defdevid)
            add_device(defdev, defdevid, list);
    }

    for(UINT i{0};i < count;++i)
    {
        IMMDevice *device;
        hr = coll->Item(i, &device);
        if(FAILED(hr)) continue;

        WCHAR *devid = get_device_id(device);
        if(devid)
        {
            if(wcscmp(devid, defdevid) != 0)
                add_device(device, devid, list);
            CoTaskMemFree(devid);
        }
        device->Release();
    }

    if(defdev) defdev->Release();
    if(defdevid) CoTaskMemFree(defdevid);
    coll->Release();

    return S_OK;
}


/* Proxy interface used by the message handler. */
struct ALCwasapiProxyVtable;

typedef struct ALCwasapiProxy {
    const struct ALCwasapiProxyVtable *vtbl;
} ALCwasapiProxy;

struct ALCwasapiProxyVtable {
    HRESULT (*const openProxy)(ALCwasapiProxy*);
    void (*const closeProxy)(ALCwasapiProxy*);

    HRESULT (*const resetProxy)(ALCwasapiProxy*);
    HRESULT (*const startProxy)(ALCwasapiProxy*);
    void  (*const stopProxy)(ALCwasapiProxy*);
};

#define DEFINE_ALCWASAPIPROXY_VTABLE(T)                                       \
DECLARE_THUNK(T, ALCwasapiProxy, HRESULT, openProxy)                          \
DECLARE_THUNK(T, ALCwasapiProxy, void, closeProxy)                            \
DECLARE_THUNK(T, ALCwasapiProxy, HRESULT, resetProxy)                         \
DECLARE_THUNK(T, ALCwasapiProxy, HRESULT, startProxy)                         \
DECLARE_THUNK(T, ALCwasapiProxy, void, stopProxy)                             \
                                                                              \
static const struct ALCwasapiProxyVtable T##_ALCwasapiProxy_vtable = {        \
    T##_ALCwasapiProxy_openProxy,                                             \
    T##_ALCwasapiProxy_closeProxy,                                            \
    T##_ALCwasapiProxy_resetProxy,                                            \
    T##_ALCwasapiProxy_startProxy,                                            \
    T##_ALCwasapiProxy_stopProxy,                                             \
}

static void ALCwasapiProxy_Construct(ALCwasapiProxy* UNUSED(self)) { }
static void ALCwasapiProxy_Destruct(ALCwasapiProxy* UNUSED(self)) { }

static DWORD CALLBACK ALCwasapiProxy_messageHandler(void *ptr)
{
    auto req = reinterpret_cast<ThreadRequest*>(ptr);

    TRACE("Starting message thread\n");

    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if(FAILED(cohr))
    {
        WARN("Failed to initialize COM: 0x%08lx\n", cohr);
        ReturnMsgResponse(req, cohr);
        return 0;
    }

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IMMDeviceEnumerator, &ptr);
    if(FAILED(hr))
    {
        WARN("Failed to create IMMDeviceEnumerator instance: 0x%08lx\n", hr);
        CoUninitialize();
        ReturnMsgResponse(req, hr);
        return 0;
    }
    auto Enumerator = reinterpret_cast<IMMDeviceEnumerator*>(ptr);
    Enumerator->Release();
    Enumerator = nullptr;

    CoUninitialize();

    /* HACK: Force Windows to create a message queue for this thread before
     * returning success, otherwise PostThreadMessage may fail if it gets
     * called before GetMessage.
     */
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    TRACE("Message thread initialization complete\n");
    ReturnMsgResponse(req, S_OK);

    TRACE("Starting message loop\n");
    ALuint deviceCount{0};
    while(GetMessage(&msg, nullptr, WM_USER_First, WM_USER_Last))
    {
        TRACE("Got message \"%s\" (0x%04x, lparam=%p, wparam=%p)\n",
            (msg.message >= WM_USER && msg.message <= WM_USER_Last) ?
            MessageStr[msg.message-WM_USER] : "Unknown",
            msg.message, (void*)msg.lParam, (void*)msg.wParam
        );

        ALCwasapiProxy *proxy{nullptr};
        switch(msg.message)
        {
        case WM_USER_OpenDevice:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);
            proxy = reinterpret_cast<ALCwasapiProxy*>(msg.lParam);

            hr = cohr = S_OK;
            if(++deviceCount == 1)
                hr = cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if(SUCCEEDED(hr))
                hr = V0(proxy,openProxy)();
            if(FAILED(hr))
            {
                if(--deviceCount == 0 && SUCCEEDED(cohr))
                    CoUninitialize();
            }

            ReturnMsgResponse(req, hr);
            continue;

        case WM_USER_ResetDevice:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);
            proxy = reinterpret_cast<ALCwasapiProxy*>(msg.lParam);

            hr = V0(proxy,resetProxy)();
            ReturnMsgResponse(req, hr);
            continue;

        case WM_USER_StartDevice:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);
            proxy = reinterpret_cast<ALCwasapiProxy*>(msg.lParam);

            hr = V0(proxy,startProxy)();
            ReturnMsgResponse(req, hr);
            continue;

        case WM_USER_StopDevice:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);
            proxy = reinterpret_cast<ALCwasapiProxy*>(msg.lParam);

            V0(proxy,stopProxy)();
            ReturnMsgResponse(req, S_OK);
            continue;

        case WM_USER_CloseDevice:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);
            proxy = reinterpret_cast<ALCwasapiProxy*>(msg.lParam);

            V0(proxy,closeProxy)();
            if(--deviceCount == 0)
                CoUninitialize();

            ReturnMsgResponse(req, S_OK);
            continue;

        case WM_USER_Enumerate:
            req = reinterpret_cast<ThreadRequest*>(msg.wParam);

            hr = cohr = S_OK;
            if(++deviceCount == 1)
                hr = cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if(SUCCEEDED(hr))
                hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator, &ptr);
            if(SUCCEEDED(hr))
            {
                Enumerator = reinterpret_cast<IMMDeviceEnumerator*>(ptr);

                if(msg.lParam == ALL_DEVICE_PROBE)
                    hr = probe_devices(Enumerator, eRender, PlaybackDevices);
                else if(msg.lParam == CAPTURE_DEVICE_PROBE)
                    hr = probe_devices(Enumerator, eCapture, CaptureDevices);

                Enumerator->Release();
                Enumerator = nullptr;
            }

            if(--deviceCount == 0 && SUCCEEDED(cohr))
                CoUninitialize();

            ReturnMsgResponse(req, hr);
            continue;

        default:
            ERR("Unexpected message: %u\n", msg.message);
            continue;
        }
    }
    TRACE("Message loop finished\n");

    return 0;
}


typedef struct ALCwasapiPlayback {
    DERIVE_FROM_TYPE(ALCbackend);
    DERIVE_FROM_TYPE(ALCwasapiProxy);

    std::wstring devid;

    IMMDevice *mmdev{nullptr};
    IAudioClient *client{nullptr};
    IAudioRenderClient *render{nullptr};
    HANDLE NotifyEvent{nullptr};

    HANDLE MsgEvent{nullptr};

    std::atomic<UINT32> Padding{0u};

    std::atomic<ALenum> killNow{AL_TRUE};
    std::thread thread;
} ALCwasapiPlayback;

static int ALCwasapiPlayback_mixerProc(ALCwasapiPlayback *self);

static void ALCwasapiPlayback_Construct(ALCwasapiPlayback *self, ALCdevice *device);
static void ALCwasapiPlayback_Destruct(ALCwasapiPlayback *self);
static ALCenum ALCwasapiPlayback_open(ALCwasapiPlayback *self, const ALCchar *name);
static HRESULT ALCwasapiPlayback_openProxy(ALCwasapiPlayback *self);
static void ALCwasapiPlayback_closeProxy(ALCwasapiPlayback *self);
static ALCboolean ALCwasapiPlayback_reset(ALCwasapiPlayback *self);
static HRESULT ALCwasapiPlayback_resetProxy(ALCwasapiPlayback *self);
static ALCboolean ALCwasapiPlayback_start(ALCwasapiPlayback *self);
static HRESULT ALCwasapiPlayback_startProxy(ALCwasapiPlayback *self);
static void ALCwasapiPlayback_stop(ALCwasapiPlayback *self);
static void ALCwasapiPlayback_stopProxy(ALCwasapiPlayback *self);
static DECLARE_FORWARD2(ALCwasapiPlayback, ALCbackend, ALCenum, captureSamples, ALCvoid*, ALCuint)
static DECLARE_FORWARD(ALCwasapiPlayback, ALCbackend, ALCuint, availableSamples)
static ClockLatency ALCwasapiPlayback_getClockLatency(ALCwasapiPlayback *self);
static DECLARE_FORWARD(ALCwasapiPlayback, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCwasapiPlayback, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCwasapiPlayback)

DEFINE_ALCWASAPIPROXY_VTABLE(ALCwasapiPlayback);
DEFINE_ALCBACKEND_VTABLE(ALCwasapiPlayback);


static void ALCwasapiPlayback_Construct(ALCwasapiPlayback *self, ALCdevice *device)
{
    new (self) ALCwasapiPlayback{};
    SET_VTABLE2(ALCwasapiPlayback, ALCbackend, self);
    SET_VTABLE2(ALCwasapiPlayback, ALCwasapiProxy, self);
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    ALCwasapiProxy_Construct(STATIC_CAST(ALCwasapiProxy, self));
}

static void ALCwasapiPlayback_Destruct(ALCwasapiPlayback *self)
{
    if(self->MsgEvent)
    {
        ThreadRequest req = { self->MsgEvent, 0 };
        if(PostThreadMessage(ThreadID, WM_USER_CloseDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
            (void)WaitForResponse(&req);

        CloseHandle(self->MsgEvent);
        self->MsgEvent = nullptr;
    }

    if(self->NotifyEvent)
        CloseHandle(self->NotifyEvent);
    self->NotifyEvent = nullptr;

    if(self->NotifyEvent != nullptr)
        CloseHandle(self->NotifyEvent);
    self->NotifyEvent = nullptr;
    if(self->MsgEvent != nullptr)
        CloseHandle(self->MsgEvent);
    self->MsgEvent = nullptr;

    ALCwasapiProxy_Destruct(STATIC_CAST(ALCwasapiProxy, self));
    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
    self->~ALCwasapiPlayback();
}


FORCE_ALIGN static int ALCwasapiPlayback_mixerProc(ALCwasapiPlayback *self)
{
    ALCdevice *device{STATIC_CAST(ALCbackend, self)->mDevice};
    IAudioClient *client{self->client};
    IAudioRenderClient *render{self->render};

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if(FAILED(hr))
    {
        ERR("CoInitializeEx(nullptr, COINIT_MULTITHREADED) failed: 0x%08lx\n", hr);
        V0(device->Backend,lock)();
        aluHandleDisconnect(device, "COM init failed: 0x%08lx", hr);
        V0(device->Backend,unlock)();
        return 1;
    }

    SetRTPriority();
    althrd_setname(althrd_current(), MIXER_THREAD_NAME);

    ALuint update_size{device->UpdateSize};
    UINT32 buffer_len{update_size * device->NumUpdates};
    while(!self->killNow.load(std::memory_order_relaxed))
    {
        UINT32 written;
        hr = client->GetCurrentPadding(&written);
        if(FAILED(hr))
        {
            ERR("Failed to get padding: 0x%08lx\n", hr);
            V0(device->Backend,lock)();
            aluHandleDisconnect(device, "Failed to retrieve buffer padding: 0x%08lx", hr);
            V0(device->Backend,unlock)();
            break;
        }
        self->Padding.store(written, std::memory_order_relaxed);

        ALuint len{buffer_len - written};
        if(len < update_size)
        {
            DWORD res;
            res = WaitForSingleObjectEx(self->NotifyEvent, 2000, FALSE);
            if(res != WAIT_OBJECT_0)
                ERR("WaitForSingleObjectEx error: 0x%lx\n", res);
            continue;
        }
        len -= len%update_size;

        BYTE *buffer;
        hr = render->GetBuffer(len, &buffer);
        if(SUCCEEDED(hr))
        {
            ALCwasapiPlayback_lock(self);
            aluMixData(device, buffer, len);
            self->Padding.store(written + len, std::memory_order_relaxed);
            ALCwasapiPlayback_unlock(self);
            hr = render->ReleaseBuffer(len, 0);
        }
        if(FAILED(hr))
        {
            ERR("Failed to buffer data: 0x%08lx\n", hr);
            V0(device->Backend,lock)();
            aluHandleDisconnect(device, "Failed to send playback samples: 0x%08lx", hr);
            V0(device->Backend,unlock)();
            break;
        }
    }
    self->Padding.store(0u, std::memory_order_release);

    CoUninitialize();
    return 0;
}


static ALCboolean MakeExtensible(WAVEFORMATEXTENSIBLE *out, const WAVEFORMATEX *in)
{
    memset(out, 0, sizeof(*out));
    if(in->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        *out = *(const WAVEFORMATEXTENSIBLE*)in;
    else if(in->wFormatTag == WAVE_FORMAT_PCM)
    {
        out->Format = *in;
        out->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        out->Format.cbSize = sizeof(*out) - sizeof(*in);
        if(out->Format.nChannels == 1)
            out->dwChannelMask = MONO;
        else if(out->Format.nChannels == 2)
            out->dwChannelMask = STEREO;
        else
            ERR("Unhandled PCM channel count: %d\n", out->Format.nChannels);
        out->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else if(in->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        out->Format = *in;
        out->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        out->Format.cbSize = sizeof(*out) - sizeof(*in);
        if(out->Format.nChannels == 1)
            out->dwChannelMask = MONO;
        else if(out->Format.nChannels == 2)
            out->dwChannelMask = STEREO;
        else
            ERR("Unhandled IEEE float channel count: %d\n", out->Format.nChannels);
        out->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    else
    {
        ERR("Unhandled format tag: 0x%04x\n", in->wFormatTag);
        return ALC_FALSE;
    }
    return ALC_TRUE;
}

static ALCenum ALCwasapiPlayback_open(ALCwasapiPlayback *self, const ALCchar *deviceName)
{
    HRESULT hr = S_OK;

    self->NotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    self->MsgEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(self->NotifyEvent == nullptr || self->MsgEvent == nullptr)
    {
        ERR("Failed to create message events: %lu\n", GetLastError());
        hr = E_FAIL;
    }

    if(SUCCEEDED(hr))
    {
        if(deviceName)
        {
            if(PlaybackDevices.empty())
            {
                ThreadRequest req = { self->MsgEvent, 0 };
                if(PostThreadMessage(ThreadID, WM_USER_Enumerate, (WPARAM)&req, ALL_DEVICE_PROBE))
                    (void)WaitForResponse(&req);
            }

            hr = E_FAIL;
            auto iter = std::find_if(PlaybackDevices.cbegin(), PlaybackDevices.cend(),
                [deviceName](const DevMap &entry) -> bool
                { return entry.name == deviceName || entry.endpoint_guid == deviceName; }
            );
            if(iter == PlaybackDevices.cend())
            {
                int len;
                if((len=MultiByteToWideChar(CP_UTF8, 0, deviceName, -1, nullptr, 0)) > 0)
                {
                    std::vector<WCHAR> wname(len);
                    MultiByteToWideChar(CP_UTF8, 0, deviceName, -1, wname.data(), len);
                    iter = std::find_if(PlaybackDevices.cbegin(), PlaybackDevices.cend(),
                        [&wname](const DevMap &entry) -> bool
                        { return entry.devid == wname.data(); }
                    );
                }
            }
            if(iter == PlaybackDevices.cend())
                WARN("Failed to find device name matching \"%s\"\n", deviceName);
            else
            {
                ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
                self->devid = iter->devid;
                alstr_copy_range(&device->DeviceName, &*iter->name.cbegin(), &*iter->name.cend());
                hr = S_OK;
            }
        }
    }

    if(SUCCEEDED(hr))
    {
        ThreadRequest req{ self->MsgEvent, 0 };

        hr = E_FAIL;
        if(PostThreadMessage(ThreadID, WM_USER_OpenDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
            hr = WaitForResponse(&req);
        else
            ERR("Failed to post thread message: %lu\n", GetLastError());
    }

    if(FAILED(hr))
    {
        if(self->NotifyEvent != nullptr)
            CloseHandle(self->NotifyEvent);
        self->NotifyEvent = nullptr;
        if(self->MsgEvent != nullptr)
            CloseHandle(self->MsgEvent);
        self->MsgEvent = nullptr;

        self->devid.clear();

        ERR("Device init failed: 0x%08lx\n", hr);
        return ALC_INVALID_VALUE;
    }

    return ALC_NO_ERROR;
}

static HRESULT ALCwasapiPlayback_openProxy(ALCwasapiPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    void *ptr;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator, &ptr);
    if(SUCCEEDED(hr))
    {
        auto Enumerator = reinterpret_cast<IMMDeviceEnumerator*>(ptr);
        if(self->devid.empty())
            hr = Enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &self->mmdev);
        else
            hr = Enumerator->GetDevice(self->devid.c_str(), &self->mmdev);
        Enumerator->Release();
    }
    if(SUCCEEDED(hr))
        hr = self->mmdev->Activate(IID_IAudioClient, CLSCTX_INPROC_SERVER, nullptr, &ptr);
    if(SUCCEEDED(hr))
    {
        self->client = reinterpret_cast<IAudioClient*>(ptr);
        if(alstr_empty(device->DeviceName))
        {
            std::string devname;
            std::tie(devname, std::ignore) = get_device_name_and_guid(self->mmdev);
            alstr_copy_range(&device->DeviceName, &*devname.cbegin(), &*devname.cend());
        }
    }

    if(FAILED(hr))
    {
        if(self->mmdev)
            self->mmdev->Release();
        self->mmdev = nullptr;
    }

    return hr;
}


static void ALCwasapiPlayback_closeProxy(ALCwasapiPlayback *self)
{
    if(self->client)
        self->client->Release();
    self->client = nullptr;

    if(self->mmdev)
        self->mmdev->Release();
    self->mmdev = nullptr;
}


static ALCboolean ALCwasapiPlayback_reset(ALCwasapiPlayback *self)
{
    ThreadRequest req{ self->MsgEvent, 0 };
    HRESULT hr{E_FAIL};

    if(PostThreadMessage(ThreadID, WM_USER_ResetDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
        hr = WaitForResponse(&req);

    return SUCCEEDED(hr) ? ALC_TRUE : ALC_FALSE;
}

static HRESULT ALCwasapiPlayback_resetProxy(ALCwasapiPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    EndpointFormFactor formfactor = UnknownFormFactor;
    WAVEFORMATEXTENSIBLE OutputType;
    WAVEFORMATEX *wfx = nullptr;
    REFERENCE_TIME min_per, buf_time;
    UINT32 buffer_len, min_len;
    void *ptr = nullptr;
    HRESULT hr;

    if(self->client)
        self->client->Release();
    self->client = nullptr;

    hr = self->mmdev->Activate(IID_IAudioClient, CLSCTX_INPROC_SERVER, nullptr, &ptr);
    if(FAILED(hr))
    {
        ERR("Failed to reactivate audio client: 0x%08lx\n", hr);
        return hr;
    }
    self->client = reinterpret_cast<IAudioClient*>(ptr);

    hr = self->client->GetMixFormat(&wfx);
    if(FAILED(hr))
    {
        ERR("Failed to get mix format: 0x%08lx\n", hr);
        return hr;
    }

    if(!MakeExtensible(&OutputType, wfx))
    {
        CoTaskMemFree(wfx);
        return E_FAIL;
    }
    CoTaskMemFree(wfx);
    wfx = nullptr;

    buf_time = ScaleCeil(device->UpdateSize*device->NumUpdates, REFTIME_PER_SEC,
                         device->Frequency);

    if(!(device->Flags&DEVICE_FREQUENCY_REQUEST))
        device->Frequency = OutputType.Format.nSamplesPerSec;
    if(!(device->Flags&DEVICE_CHANNELS_REQUEST))
    {
        if(OutputType.Format.nChannels == 1 && OutputType.dwChannelMask == MONO)
            device->FmtChans = DevFmtMono;
        else if(OutputType.Format.nChannels == 2 && OutputType.dwChannelMask == STEREO)
            device->FmtChans = DevFmtStereo;
        else if(OutputType.Format.nChannels == 4 && OutputType.dwChannelMask == QUAD)
            device->FmtChans = DevFmtQuad;
        else if(OutputType.Format.nChannels == 6 && OutputType.dwChannelMask == X5DOT1)
            device->FmtChans = DevFmtX51;
        else if(OutputType.Format.nChannels == 6 && OutputType.dwChannelMask == X5DOT1REAR)
            device->FmtChans = DevFmtX51Rear;
        else if(OutputType.Format.nChannels == 7 && OutputType.dwChannelMask == X6DOT1)
            device->FmtChans = DevFmtX61;
        else if(OutputType.Format.nChannels == 8 && (OutputType.dwChannelMask == X7DOT1 || OutputType.dwChannelMask == X7DOT1_WIDE))
            device->FmtChans = DevFmtX71;
        else
            ERR("Unhandled channel config: %d -- 0x%08lx\n", OutputType.Format.nChannels, OutputType.dwChannelMask);
    }

    switch(device->FmtChans)
    {
        case DevFmtMono:
            OutputType.Format.nChannels = 1;
            OutputType.dwChannelMask = MONO;
            break;
        case DevFmtAmbi3D:
            device->FmtChans = DevFmtStereo;
            /*fall-through*/
        case DevFmtStereo:
            OutputType.Format.nChannels = 2;
            OutputType.dwChannelMask = STEREO;
            break;
        case DevFmtQuad:
            OutputType.Format.nChannels = 4;
            OutputType.dwChannelMask = QUAD;
            break;
        case DevFmtX51:
            OutputType.Format.nChannels = 6;
            OutputType.dwChannelMask = X5DOT1;
            break;
        case DevFmtX51Rear:
            OutputType.Format.nChannels = 6;
            OutputType.dwChannelMask = X5DOT1REAR;
            break;
        case DevFmtX61:
            OutputType.Format.nChannels = 7;
            OutputType.dwChannelMask = X6DOT1;
            break;
        case DevFmtX71:
            OutputType.Format.nChannels = 8;
            OutputType.dwChannelMask = X7DOT1;
            break;
    }
    switch(device->FmtType)
    {
        case DevFmtByte:
            device->FmtType = DevFmtUByte;
            /* fall-through */
        case DevFmtUByte:
            OutputType.Format.wBitsPerSample = 8;
            OutputType.Samples.wValidBitsPerSample = 8;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtUShort:
            device->FmtType = DevFmtShort;
            /* fall-through */
        case DevFmtShort:
            OutputType.Format.wBitsPerSample = 16;
            OutputType.Samples.wValidBitsPerSample = 16;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtUInt:
            device->FmtType = DevFmtInt;
            /* fall-through */
        case DevFmtInt:
            OutputType.Format.wBitsPerSample = 32;
            OutputType.Samples.wValidBitsPerSample = 32;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtFloat:
            OutputType.Format.wBitsPerSample = 32;
            OutputType.Samples.wValidBitsPerSample = 32;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            break;
    }
    OutputType.Format.nSamplesPerSec = device->Frequency;

    OutputType.Format.nBlockAlign = OutputType.Format.nChannels *
                                    OutputType.Format.wBitsPerSample / 8;
    OutputType.Format.nAvgBytesPerSec = OutputType.Format.nSamplesPerSec *
                                        OutputType.Format.nBlockAlign;

    hr = self->client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &OutputType.Format, &wfx);
    if(FAILED(hr))
    {
        ERR("Failed to check format support: 0x%08lx\n", hr);
        hr = self->client->GetMixFormat(&wfx);
    }
    if(FAILED(hr))
    {
        ERR("Failed to find a supported format: 0x%08lx\n", hr);
        return hr;
    }

    if(wfx != nullptr)
    {
        if(!MakeExtensible(&OutputType, wfx))
        {
            CoTaskMemFree(wfx);
            return E_FAIL;
        }
        CoTaskMemFree(wfx);
        wfx = nullptr;

        device->Frequency = OutputType.Format.nSamplesPerSec;
        if(OutputType.Format.nChannels == 1 && OutputType.dwChannelMask == MONO)
            device->FmtChans = DevFmtMono;
        else if(OutputType.Format.nChannels == 2 && OutputType.dwChannelMask == STEREO)
            device->FmtChans = DevFmtStereo;
        else if(OutputType.Format.nChannels == 4 && OutputType.dwChannelMask == QUAD)
            device->FmtChans = DevFmtQuad;
        else if(OutputType.Format.nChannels == 6 && OutputType.dwChannelMask == X5DOT1)
            device->FmtChans = DevFmtX51;
        else if(OutputType.Format.nChannels == 6 && OutputType.dwChannelMask == X5DOT1REAR)
            device->FmtChans = DevFmtX51Rear;
        else if(OutputType.Format.nChannels == 7 && OutputType.dwChannelMask == X6DOT1)
            device->FmtChans = DevFmtX61;
        else if(OutputType.Format.nChannels == 8 && (OutputType.dwChannelMask == X7DOT1 || OutputType.dwChannelMask == X7DOT1_WIDE))
            device->FmtChans = DevFmtX71;
        else
        {
            ERR("Unhandled extensible channels: %d -- 0x%08lx\n", OutputType.Format.nChannels, OutputType.dwChannelMask);
            device->FmtChans = DevFmtStereo;
            OutputType.Format.nChannels = 2;
            OutputType.dwChannelMask = STEREO;
        }

        if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            if(OutputType.Format.wBitsPerSample == 8)
                device->FmtType = DevFmtUByte;
            else if(OutputType.Format.wBitsPerSample == 16)
                device->FmtType = DevFmtShort;
            else if(OutputType.Format.wBitsPerSample == 32)
                device->FmtType = DevFmtInt;
            else
            {
                device->FmtType = DevFmtShort;
                OutputType.Format.wBitsPerSample = 16;
            }
        }
        else if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        {
            device->FmtType = DevFmtFloat;
            OutputType.Format.wBitsPerSample = 32;
        }
        else
        {
            ERR("Unhandled format sub-type\n");
            device->FmtType = DevFmtShort;
            OutputType.Format.wBitsPerSample = 16;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        }
        OutputType.Samples.wValidBitsPerSample = OutputType.Format.wBitsPerSample;
    }
    get_device_formfactor(self->mmdev, &formfactor);
    device->IsHeadphones = (device->FmtChans == DevFmtStereo &&
                            (formfactor == Headphones || formfactor == Headset)
                           );

    SetDefaultWFXChannelOrder(device);

    hr = self->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  buf_time, 0, &OutputType.Format, nullptr);
    if(FAILED(hr))
    {
        ERR("Failed to initialize audio client: 0x%08lx\n", hr);
        return hr;
    }

    hr = self->client->GetDevicePeriod(&min_per, nullptr);
    if(SUCCEEDED(hr))
    {
        min_len = (UINT32)ScaleCeil(min_per, device->Frequency, REFTIME_PER_SEC);
        /* Find the nearest multiple of the period size to the update size */
        if(min_len < device->UpdateSize)
            min_len *= (device->UpdateSize + min_len/2)/min_len;
        hr = self->client->GetBufferSize(&buffer_len);
    }
    if(FAILED(hr))
    {
        ERR("Failed to get audio buffer info: 0x%08lx\n", hr);
        return hr;
    }

    device->UpdateSize = min_len;
    device->NumUpdates = buffer_len / device->UpdateSize;
    if(device->NumUpdates <= 1)
    {
        ERR("Audio client returned buffer_len < period*2; expect break up\n");
        device->NumUpdates = 2;
        device->UpdateSize = buffer_len / device->NumUpdates;
    }

    hr = self->client->SetEventHandle(self->NotifyEvent);
    if(FAILED(hr))
    {
        ERR("Failed to set event handle: 0x%08lx\n", hr);
        return hr;
    }

    return hr;
}


static ALCboolean ALCwasapiPlayback_start(ALCwasapiPlayback *self)
{
    ThreadRequest req{ self->MsgEvent, 0 };
    HRESULT hr{E_FAIL};

    if(PostThreadMessage(ThreadID, WM_USER_StartDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
        hr = WaitForResponse(&req);

    return SUCCEEDED(hr) ? ALC_TRUE : ALC_FALSE;
}

static HRESULT ALCwasapiPlayback_startProxy(ALCwasapiPlayback *self)
{
    HRESULT hr;
    void *ptr;

    ResetEvent(self->NotifyEvent);
    hr = self->client->Start();
    if(FAILED(hr))
    {
        ERR("Failed to start audio client: 0x%08lx\n", hr);
        return hr;
    }

    hr = self->client->GetService(IID_IAudioRenderClient, &ptr);
    if(SUCCEEDED(hr))
    {
        self->render = reinterpret_cast<IAudioRenderClient*>(ptr);
        try {
            self->killNow.store(AL_FALSE, std::memory_order_release);
            self->thread = std::thread(ALCwasapiPlayback_mixerProc, self);
        }
        catch(...) {
            self->render->Release();
            self->render = nullptr;
            ERR("Failed to start thread\n");
            hr = E_FAIL;
        }
    }

    if(FAILED(hr))
        self->client->Stop();

    return hr;
}


static void ALCwasapiPlayback_stop(ALCwasapiPlayback *self)
{
    ThreadRequest req{ self->MsgEvent, 0 };
    if(PostThreadMessage(ThreadID, WM_USER_StopDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
        (void)WaitForResponse(&req);
}

static void ALCwasapiPlayback_stopProxy(ALCwasapiPlayback *self)
{
    if(!self->render || !self->thread.joinable())
        return;

    self->killNow.store(AL_TRUE);
    self->thread.join();

    self->render->Release();
    self->render = nullptr;
    self->client->Stop();
}


static ClockLatency ALCwasapiPlayback_getClockLatency(ALCwasapiPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    ClockLatency ret;

    ALCwasapiPlayback_lock(self);
    ret.ClockTime = GetDeviceClockTime(device);
    ret.Latency = self->Padding.load(std::memory_order_relaxed) * DEVICE_CLOCK_RES /
                  device->Frequency;
    ALCwasapiPlayback_unlock(self);

    return ret;
}


typedef struct ALCwasapiCapture {
    DERIVE_FROM_TYPE(ALCbackend);
    DERIVE_FROM_TYPE(ALCwasapiProxy);

    std::wstring devid;

    IMMDevice *mmdev{nullptr};
    IAudioClient *client{nullptr};
    IAudioCaptureClient *capture{nullptr};
    HANDLE NotifyEvent{nullptr};

    HANDLE MsgEvent{nullptr};

    ChannelConverter *ChannelConv{nullptr};
    SampleConverter *SampleConv{nullptr};
    ll_ringbuffer_t *Ring{nullptr};

    std::atomic<int> killNow{AL_TRUE};
    std::thread thread;
} ALCwasapiCapture;

static int ALCwasapiCapture_recordProc(ALCwasapiCapture *self);

static void ALCwasapiCapture_Construct(ALCwasapiCapture *self, ALCdevice *device);
static void ALCwasapiCapture_Destruct(ALCwasapiCapture *self);
static ALCenum ALCwasapiCapture_open(ALCwasapiCapture *self, const ALCchar *name);
static HRESULT ALCwasapiCapture_openProxy(ALCwasapiCapture *self);
static void ALCwasapiCapture_closeProxy(ALCwasapiCapture *self);
static DECLARE_FORWARD(ALCwasapiCapture, ALCbackend, ALCboolean, reset)
static HRESULT ALCwasapiCapture_resetProxy(ALCwasapiCapture *self);
static ALCboolean ALCwasapiCapture_start(ALCwasapiCapture *self);
static HRESULT ALCwasapiCapture_startProxy(ALCwasapiCapture *self);
static void ALCwasapiCapture_stop(ALCwasapiCapture *self);
static void ALCwasapiCapture_stopProxy(ALCwasapiCapture *self);
static ALCenum ALCwasapiCapture_captureSamples(ALCwasapiCapture *self, ALCvoid *buffer, ALCuint samples);
static ALuint ALCwasapiCapture_availableSamples(ALCwasapiCapture *self);
static DECLARE_FORWARD(ALCwasapiCapture, ALCbackend, ClockLatency, getClockLatency)
static DECLARE_FORWARD(ALCwasapiCapture, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCwasapiCapture, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCwasapiCapture)

DEFINE_ALCWASAPIPROXY_VTABLE(ALCwasapiCapture);
DEFINE_ALCBACKEND_VTABLE(ALCwasapiCapture);


static void ALCwasapiCapture_Construct(ALCwasapiCapture *self, ALCdevice *device)
{
    new (self) ALCwasapiCapture{};
    SET_VTABLE2(ALCwasapiCapture, ALCbackend, self);
    SET_VTABLE2(ALCwasapiCapture, ALCwasapiProxy, self);
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    ALCwasapiProxy_Construct(STATIC_CAST(ALCwasapiProxy, self));
}

static void ALCwasapiCapture_Destruct(ALCwasapiCapture *self)
{
    if(self->MsgEvent)
    {
        ThreadRequest req{ self->MsgEvent, 0 };
        if(PostThreadMessage(ThreadID, WM_USER_CloseDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
            (void)WaitForResponse(&req);

        CloseHandle(self->MsgEvent);
        self->MsgEvent = nullptr;
    }

    if(self->NotifyEvent != nullptr)
        CloseHandle(self->NotifyEvent);
    self->NotifyEvent = nullptr;

    ll_ringbuffer_free(self->Ring);
    self->Ring = nullptr;

    DestroySampleConverter(&self->SampleConv);
    DestroyChannelConverter(&self->ChannelConv);

    ALCwasapiProxy_Destruct(STATIC_CAST(ALCwasapiProxy, self));
    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
    self->~ALCwasapiCapture();
}


FORCE_ALIGN int ALCwasapiCapture_recordProc(ALCwasapiCapture *self)
{
    ALCdevice *device{STATIC_CAST(ALCbackend, self)->mDevice};
    IAudioCaptureClient *capture{self->capture};

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if(FAILED(hr))
    {
        ERR("CoInitializeEx(nullptr, COINIT_MULTITHREADED) failed: 0x%08lx\n", hr);
        V0(device->Backend,lock)();
        aluHandleDisconnect(device, "COM init failed: 0x%08lx", hr);
        V0(device->Backend,unlock)();
        return 1;
    }

    althrd_setname(althrd_current(), RECORD_THREAD_NAME);

    std::vector<float> samples;
    while(!self->killNow.load(std::memory_order_relaxed))
    {
        UINT32 avail;
        DWORD res;

        hr = capture->GetNextPacketSize(&avail);
        if(FAILED(hr))
            ERR("Failed to get next packet size: 0x%08lx\n", hr);
        else if(avail > 0)
        {
            UINT32 numsamples;
            DWORD flags;
            BYTE *rdata;

            hr = capture->GetBuffer(&rdata, &numsamples, &flags, nullptr, nullptr);
            if(FAILED(hr))
                ERR("Failed to get capture buffer: 0x%08lx\n", hr);
            else
            {
                if(self->ChannelConv)
                {
                    samples.resize(numsamples*2);
                    ChannelConverterInput(self->ChannelConv, rdata, samples.data(), numsamples);
                    rdata = reinterpret_cast<BYTE*>(samples.data());
                }

                ll_ringbuffer_data_t data[2];
                ll_ringbuffer_get_write_vector(self->Ring, data);

                size_t dstframes;
                if(self->SampleConv)
                {
                    const ALvoid *srcdata = rdata;
                    ALsizei srcframes = numsamples;

                    dstframes = SampleConverterInput(self->SampleConv,
                        &srcdata, &srcframes, data[0].buf, (ALsizei)minz(data[0].len, INT_MAX)
                    );
                    if(srcframes > 0 && dstframes == data[0].len && data[1].len > 0)
                    {
                        /* If some source samples remain, all of the first dest
                         * block was filled, and there's space in the second
                         * dest block, do another run for the second block.
                         */
                        dstframes += SampleConverterInput(self->SampleConv,
                            &srcdata, &srcframes, data[1].buf, (ALsizei)minz(data[1].len, INT_MAX)
                        );
                    }
                }
                else
                {
                    ALuint framesize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType,
                                                           device->AmbiOrder);
                    size_t len1 = minz(data[0].len, numsamples);
                    size_t len2 = minz(data[1].len, numsamples-len1);

                    memcpy(data[0].buf, rdata, len1*framesize);
                    if(len2 > 0)
                        memcpy(data[1].buf, rdata+len1*framesize, len2*framesize);
                    dstframes = len1 + len2;
                }

                ll_ringbuffer_write_advance(self->Ring, dstframes);

                hr = capture->ReleaseBuffer(numsamples);
                if(FAILED(hr)) ERR("Failed to release capture buffer: 0x%08lx\n", hr);
            }
        }

        if(FAILED(hr))
        {
            V0(device->Backend,lock)();
            aluHandleDisconnect(device, "Failed to capture samples: 0x%08lx", hr);
            V0(device->Backend,unlock)();
            break;
        }

        res = WaitForSingleObjectEx(self->NotifyEvent, 2000, FALSE);
        if(res != WAIT_OBJECT_0)
            ERR("WaitForSingleObjectEx error: 0x%lx\n", res);
    }

    CoUninitialize();
    return 0;
}


static ALCenum ALCwasapiCapture_open(ALCwasapiCapture *self, const ALCchar *deviceName)
{
    HRESULT hr{S_OK};

    self->NotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    self->MsgEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(self->NotifyEvent == nullptr || self->MsgEvent == nullptr)
    {
        ERR("Failed to create message events: %lu\n", GetLastError());
        hr = E_FAIL;
    }

    if(SUCCEEDED(hr))
    {
        if(deviceName)
        {
            if(CaptureDevices.empty())
            {
                ThreadRequest req{ self->MsgEvent, 0 };
                if(PostThreadMessage(ThreadID, WM_USER_Enumerate, (WPARAM)&req, CAPTURE_DEVICE_PROBE))
                    (void)WaitForResponse(&req);
            }

            hr = E_FAIL;
            auto iter = std::find_if(CaptureDevices.cbegin(), CaptureDevices.cend(),
                [deviceName](const DevMap &entry) -> bool
                { return entry.name == deviceName || entry.endpoint_guid == deviceName; }
            );
            if(iter == CaptureDevices.cend())
            {
                int len;
                if((len=MultiByteToWideChar(CP_UTF8, 0, deviceName, -1, nullptr, 0)) > 0)
                {
                    std::vector<WCHAR> wname(len);
                    MultiByteToWideChar(CP_UTF8, 0, deviceName, -1, wname.data(), len);
                    iter = std::find_if(CaptureDevices.cbegin(), CaptureDevices.cend(),
                        [&wname](const DevMap &entry) -> bool
                        { return entry.devid == wname.data(); }
                    );
                }
            }
            if(iter == CaptureDevices.cend())
                WARN("Failed to find device name matching \"%s\"\n", deviceName);
            else
            {
                ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
                self->devid = iter->devid;
                alstr_copy_range(&device->DeviceName, &*iter->name.cbegin(), &*iter->name.cend());
                hr = S_OK;
            }
        }
    }

    if(SUCCEEDED(hr))
    {
        ThreadRequest req{ self->MsgEvent, 0 };

        hr = E_FAIL;
        if(PostThreadMessage(ThreadID, WM_USER_OpenDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
            hr = WaitForResponse(&req);
        else
            ERR("Failed to post thread message: %lu\n", GetLastError());
    }

    if(FAILED(hr))
    {
        if(self->NotifyEvent != nullptr)
            CloseHandle(self->NotifyEvent);
        self->NotifyEvent = nullptr;
        if(self->MsgEvent != nullptr)
            CloseHandle(self->MsgEvent);
        self->MsgEvent = nullptr;

        self->devid.clear();

        ERR("Device init failed: 0x%08lx\n", hr);
        return ALC_INVALID_VALUE;
    }
    else
    {
        ThreadRequest req{ self->MsgEvent, 0 };

        hr = E_FAIL;
        if(PostThreadMessage(ThreadID, WM_USER_ResetDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
            hr = WaitForResponse(&req);
        else
            ERR("Failed to post thread message: %lu\n", GetLastError());

        if(FAILED(hr))
        {
            if(hr == E_OUTOFMEMORY)
               return ALC_OUT_OF_MEMORY;
            return ALC_INVALID_VALUE;
        }
    }

    return ALC_NO_ERROR;
}

static HRESULT ALCwasapiCapture_openProxy(ALCwasapiCapture *self)
{
    ALCdevice *device{STATIC_CAST(ALCbackend, self)->mDevice};

    void *ptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IMMDeviceEnumerator, &ptr);
    if(SUCCEEDED(hr))
    {
        auto Enumerator = reinterpret_cast<IMMDeviceEnumerator*>(ptr);
        if(self->devid.empty())
            hr = Enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &self->mmdev);
        else
            hr = Enumerator->GetDevice(self->devid.c_str(), &self->mmdev);
        Enumerator->Release();
    }
    if(SUCCEEDED(hr))
        hr = self->mmdev->Activate(IID_IAudioClient, CLSCTX_INPROC_SERVER, nullptr, &ptr);
    if(SUCCEEDED(hr))
    {
        self->client = reinterpret_cast<IAudioClient*>(ptr);
        if(alstr_empty(device->DeviceName))
        {
            std::string devname;
            std::tie(devname, std::ignore) = get_device_name_and_guid(self->mmdev);
            alstr_copy_range(&device->DeviceName, &*devname.cbegin(), &*devname.cend());
        }
    }

    if(FAILED(hr))
    {
        if(self->mmdev)
            self->mmdev->Release();
        self->mmdev = nullptr;
    }

    return hr;
}


static void ALCwasapiCapture_closeProxy(ALCwasapiCapture *self)
{
    if(self->client)
        self->client->Release();
    self->client = nullptr;

    if(self->mmdev)
        self->mmdev->Release();
    self->mmdev = nullptr;
}


static HRESULT ALCwasapiCapture_resetProxy(ALCwasapiCapture *self)
{
    ALCdevice *device{STATIC_CAST(ALCbackend, self)->mDevice};

    IAudioClient *client{self->client};
    self->client = nullptr;
    if(client) client->Release();
    client = nullptr;

    void *ptr;
    HRESULT hr{self->mmdev->Activate(IID_IAudioClient, CLSCTX_INPROC_SERVER, nullptr, &ptr)};
    if(FAILED(hr))
    {
        ERR("Failed to reactivate audio client: 0x%08lx\n", hr);
        return hr;
    }
    client = self->client = reinterpret_cast<IAudioClient*>(ptr);

    REFERENCE_TIME buf_time{ScaleCeil(device->UpdateSize*device->NumUpdates, REFTIME_PER_SEC,
                                      device->Frequency)};
    // Make sure buffer is at least 100ms in size
    buf_time = maxu64(buf_time, REFTIME_PER_SEC/10);
    device->UpdateSize = (ALuint)ScaleCeil(buf_time, device->Frequency, REFTIME_PER_SEC) /
                         device->NumUpdates;

    WAVEFORMATEXTENSIBLE OutputType;
    OutputType.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    switch(device->FmtChans)
    {
        case DevFmtMono:
            OutputType.Format.nChannels = 1;
            OutputType.dwChannelMask = MONO;
            break;
        case DevFmtStereo:
            OutputType.Format.nChannels = 2;
            OutputType.dwChannelMask = STEREO;
            break;
        case DevFmtQuad:
            OutputType.Format.nChannels = 4;
            OutputType.dwChannelMask = QUAD;
            break;
        case DevFmtX51:
            OutputType.Format.nChannels = 6;
            OutputType.dwChannelMask = X5DOT1;
            break;
        case DevFmtX51Rear:
            OutputType.Format.nChannels = 6;
            OutputType.dwChannelMask = X5DOT1REAR;
            break;
        case DevFmtX61:
            OutputType.Format.nChannels = 7;
            OutputType.dwChannelMask = X6DOT1;
            break;
        case DevFmtX71:
            OutputType.Format.nChannels = 8;
            OutputType.dwChannelMask = X7DOT1;
            break;

        case DevFmtAmbi3D:
            return E_FAIL;
    }
    switch(device->FmtType)
    {
        /* NOTE: Signedness doesn't matter, the converter will handle it. */
        case DevFmtByte:
        case DevFmtUByte:
            OutputType.Format.wBitsPerSample = 8;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtShort:
        case DevFmtUShort:
            OutputType.Format.wBitsPerSample = 16;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtInt:
        case DevFmtUInt:
            OutputType.Format.wBitsPerSample = 32;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            break;
        case DevFmtFloat:
            OutputType.Format.wBitsPerSample = 32;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            break;
    }
    OutputType.Samples.wValidBitsPerSample = OutputType.Format.wBitsPerSample;
    OutputType.Format.nSamplesPerSec = device->Frequency;

    OutputType.Format.nBlockAlign = OutputType.Format.nChannels *
                                    OutputType.Format.wBitsPerSample / 8;
    OutputType.Format.nAvgBytesPerSec = OutputType.Format.nSamplesPerSec *
                                        OutputType.Format.nBlockAlign;
    OutputType.Format.cbSize = sizeof(OutputType) - sizeof(OutputType.Format);

    WAVEFORMATEX *wfx;
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &OutputType.Format, &wfx);
    if(FAILED(hr))
    {
        ERR("Failed to check format support: 0x%08lx\n", hr);
        return hr;
    }

    DestroySampleConverter(&self->SampleConv);
    DestroyChannelConverter(&self->ChannelConv);

    if(wfx != nullptr)
    {
        if(!(wfx->nChannels == OutputType.Format.nChannels ||
             (wfx->nChannels == 1 && OutputType.Format.nChannels == 2) ||
             (wfx->nChannels == 2 && OutputType.Format.nChannels == 1)))
        {
            ERR("Failed to get matching format, wanted: %s %s %uhz, got: %d channel%s %d-bit %luhz\n",
                DevFmtChannelsString(device->FmtChans), DevFmtTypeString(device->FmtType),
                device->Frequency, wfx->nChannels, (wfx->nChannels==1)?"":"s", wfx->wBitsPerSample,
                wfx->nSamplesPerSec);
            CoTaskMemFree(wfx);
            return E_FAIL;
        }

        if(!MakeExtensible(&OutputType, wfx))
        {
            CoTaskMemFree(wfx);
            return E_FAIL;
        }
        CoTaskMemFree(wfx);
        wfx = nullptr;
    }

    enum DevFmtType srcType;
    if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
    {
        if(OutputType.Format.wBitsPerSample == 8)
            srcType = DevFmtUByte;
        else if(OutputType.Format.wBitsPerSample == 16)
            srcType = DevFmtShort;
        else if(OutputType.Format.wBitsPerSample == 32)
            srcType = DevFmtInt;
        else
        {
            ERR("Unhandled integer bit depth: %d\n", OutputType.Format.wBitsPerSample);
            return E_FAIL;
        }
    }
    else if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
    {
        if(OutputType.Format.wBitsPerSample == 32)
            srcType = DevFmtFloat;
        else
        {
            ERR("Unhandled float bit depth: %d\n", OutputType.Format.wBitsPerSample);
            return E_FAIL;
        }
    }
    else
    {
        ERR("Unhandled format sub-type\n");
        return E_FAIL;
    }

    if(device->FmtChans == DevFmtMono && OutputType.Format.nChannels == 2)
    {
        self->ChannelConv = CreateChannelConverter(srcType, DevFmtStereo, device->FmtChans);
        if(!self->ChannelConv)
        {
            ERR("Failed to create %s stereo-to-mono converter\n", DevFmtTypeString(srcType));
            return E_FAIL;
        }
        TRACE("Created %s stereo-to-mono converter\n", DevFmtTypeString(srcType));
        /* The channel converter always outputs float, so change the input type
         * for the resampler/type-converter.
         */
        srcType = DevFmtFloat;
    }
    else if(device->FmtChans == DevFmtStereo && OutputType.Format.nChannels == 1)
    {
        self->ChannelConv = CreateChannelConverter(srcType, DevFmtMono, device->FmtChans);
        if(!self->ChannelConv)
        {
            ERR("Failed to create %s mono-to-stereo converter\n", DevFmtTypeString(srcType));
            return E_FAIL;
        }
        TRACE("Created %s mono-to-stereo converter\n", DevFmtTypeString(srcType));
        srcType = DevFmtFloat;
    }

    if(device->Frequency != OutputType.Format.nSamplesPerSec || device->FmtType != srcType)
    {
        self->SampleConv = CreateSampleConverter(
            srcType, device->FmtType, ChannelsFromDevFmt(device->FmtChans, device->AmbiOrder),
            OutputType.Format.nSamplesPerSec, device->Frequency
        );
        if(!self->SampleConv)
        {
            ERR("Failed to create converter for %s format, dst: %s %uhz, src: %s %luhz\n",
                DevFmtChannelsString(device->FmtChans), DevFmtTypeString(device->FmtType),
                device->Frequency, DevFmtTypeString(srcType), OutputType.Format.nSamplesPerSec);
            return E_FAIL;
        }
        TRACE("Created converter for %s format, dst: %s %uhz, src: %s %luhz\n",
              DevFmtChannelsString(device->FmtChans), DevFmtTypeString(device->FmtType),
              device->Frequency, DevFmtTypeString(srcType), OutputType.Format.nSamplesPerSec);
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            buf_time, 0, &OutputType.Format, nullptr);
    if(FAILED(hr))
    {
        ERR("Failed to initialize audio client: 0x%08lx\n", hr);
        return hr;
    }

    UINT32 buffer_len;
    hr = client->GetBufferSize(&buffer_len);
    if(FAILED(hr))
    {
        ERR("Failed to get buffer size: 0x%08lx\n", hr);
        return hr;
    }

    buffer_len = maxu(device->UpdateSize*device->NumUpdates, buffer_len);
    ll_ringbuffer_free(self->Ring);
    self->Ring = ll_ringbuffer_create(buffer_len,
        FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder),
        false
    );
    if(!self->Ring)
    {
        ERR("Failed to allocate capture ring buffer\n");
        return E_OUTOFMEMORY;
    }

    hr = client->SetEventHandle(self->NotifyEvent);
    if(FAILED(hr))
    {
        ERR("Failed to set event handle: 0x%08lx\n", hr);
        return hr;
    }

    return hr;
}


static ALCboolean ALCwasapiCapture_start(ALCwasapiCapture *self)
{
    ThreadRequest req{ self->MsgEvent, 0 };
    HRESULT hr{E_FAIL};

    if(PostThreadMessage(ThreadID, WM_USER_StartDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
        hr = WaitForResponse(&req);

    return SUCCEEDED(hr) ? ALC_TRUE : ALC_FALSE;
}

static HRESULT ALCwasapiCapture_startProxy(ALCwasapiCapture *self)
{
    ResetEvent(self->NotifyEvent);

    IAudioClient *client{self->client};
    HRESULT hr{client->Start()};
    if(FAILED(hr))
    {
        ERR("Failed to start audio client: 0x%08lx\n", hr);
        return hr;
    }

    void *ptr;
    hr = client->GetService(IID_IAudioCaptureClient, &ptr);
    if(SUCCEEDED(hr))
    {
        self->capture = reinterpret_cast<IAudioCaptureClient*>(ptr);
        try {
            self->killNow.store(AL_FALSE, std::memory_order_release);
            self->thread = std::thread(ALCwasapiCapture_recordProc, self);
        }
        catch(...) {
            self->capture->Release();
            self->capture = nullptr;
            ERR("Failed to start thread\n");
            hr = E_FAIL;
        }
    }

    if(FAILED(hr))
    {
        client->Stop();
        client->Reset();
    }

    return hr;
}


static void ALCwasapiCapture_stop(ALCwasapiCapture *self)
{
    ThreadRequest req{ self->MsgEvent, 0 };
    if(PostThreadMessage(ThreadID, WM_USER_StopDevice, (WPARAM)&req, (LPARAM)STATIC_CAST(ALCwasapiProxy, self)))
        (void)WaitForResponse(&req);
}

static void ALCwasapiCapture_stopProxy(ALCwasapiCapture *self)
{
    if(!self->capture || !self->thread.joinable())
        return;

    self->killNow.store(AL_TRUE);
    self->thread.join();

    self->capture->Release();
    self->capture = nullptr;
    self->client->Stop();
    self->client->Reset();
}


static ALuint ALCwasapiCapture_availableSamples(ALCwasapiCapture *self)
{
    return (ALuint)ll_ringbuffer_read_space(self->Ring);
}

static ALCenum ALCwasapiCapture_captureSamples(ALCwasapiCapture *self, ALCvoid *buffer, ALCuint samples)
{
    if(ALCwasapiCapture_availableSamples(self) < samples)
        return ALC_INVALID_VALUE;
    ll_ringbuffer_read(self->Ring, reinterpret_cast<char*>(buffer), samples);
    return ALC_NO_ERROR;
}


typedef struct ALCwasapiBackendFactory {
    DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCwasapiBackendFactory;
#define ALCWASAPIBACKENDFACTORY_INITIALIZER { GET_VTABLE2(ALCwasapiBackendFactory, ALCbackendFactory) }

static ALCboolean ALCwasapiBackendFactory_init(ALCwasapiBackendFactory *self);
static void ALCwasapiBackendFactory_deinit(ALCwasapiBackendFactory *self);
static ALCboolean ALCwasapiBackendFactory_querySupport(ALCwasapiBackendFactory *self, ALCbackend_Type type);
static void ALCwasapiBackendFactory_probe(ALCwasapiBackendFactory *self, enum DevProbe type, al_string *outnames);
static ALCbackend* ALCwasapiBackendFactory_createBackend(ALCwasapiBackendFactory *self, ALCdevice *device, ALCbackend_Type type);

DEFINE_ALCBACKENDFACTORY_VTABLE(ALCwasapiBackendFactory);


static ALCboolean ALCwasapiBackendFactory_init(ALCwasapiBackendFactory* UNUSED(self))
{
    static HRESULT InitResult;

    if(!ThreadHdl)
    {
        ThreadRequest req;
        InitResult = E_FAIL;

        req.FinishedEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(req.FinishedEvt == nullptr)
            ERR("Failed to create event: %lu\n", GetLastError());
        else
        {
            ThreadHdl = CreateThread(nullptr, 0, ALCwasapiProxy_messageHandler, &req, 0, &ThreadID);
            if(ThreadHdl != nullptr)
                InitResult = WaitForResponse(&req);
            CloseHandle(req.FinishedEvt);
        }
    }

    return SUCCEEDED(InitResult) ? ALC_TRUE : ALC_FALSE;
}

static void ALCwasapiBackendFactory_deinit(ALCwasapiBackendFactory* UNUSED(self))
{
    PlaybackDevices.clear();
    CaptureDevices.clear();

    if(ThreadHdl)
    {
        TRACE("Sending WM_QUIT to Thread %04lx\n", ThreadID);
        PostThreadMessage(ThreadID, WM_QUIT, 0, 0);
        CloseHandle(ThreadHdl);
        ThreadHdl = nullptr;
    }
}

static ALCboolean ALCwasapiBackendFactory_querySupport(ALCwasapiBackendFactory* UNUSED(self), ALCbackend_Type type)
{
    if(type == ALCbackend_Playback || type == ALCbackend_Capture)
        return ALC_TRUE;
    return ALC_FALSE;
}

static void ALCwasapiBackendFactory_probe(ALCwasapiBackendFactory* UNUSED(self), enum DevProbe type, al_string *outnames)
{
    ThreadRequest req{ nullptr, 0 };

    req.FinishedEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(req.FinishedEvt == nullptr)
        ERR("Failed to create event: %lu\n", GetLastError());
    else
    {
        auto add_device = [outnames](const DevMap &entry) -> void
        {
            const char *name{entry.name.c_str()};
            size_t namelen{entry.name.length()};
            /* +1 to also append the null char (to ensure a null-separated list
             * and double-null terminated list).
             */
            alstr_append_range(outnames, name, name + namelen+1);
        };
        HRESULT hr = E_FAIL;
        if(PostThreadMessage(ThreadID, WM_USER_Enumerate, (WPARAM)&req, type))
            hr = WaitForResponse(&req);
        if(SUCCEEDED(hr)) switch(type)
        {
        case ALL_DEVICE_PROBE:
            std::for_each(PlaybackDevices.cbegin(), PlaybackDevices.cend(), add_device);
            break;

        case CAPTURE_DEVICE_PROBE:
            std::for_each(CaptureDevices.cbegin(), CaptureDevices.cend(), add_device);
            break;
        }
        CloseHandle(req.FinishedEvt);
        req.FinishedEvt = nullptr;
    }
}

static ALCbackend* ALCwasapiBackendFactory_createBackend(ALCwasapiBackendFactory* UNUSED(self), ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCwasapiPlayback *backend;
        NEW_OBJ(backend, ALCwasapiPlayback)(device);
        if(!backend) return nullptr;
        return STATIC_CAST(ALCbackend, backend);
    }
    if(type == ALCbackend_Capture)
    {
        ALCwasapiCapture *backend;
        NEW_OBJ(backend, ALCwasapiCapture)(device);
        if(!backend) return nullptr;
        return STATIC_CAST(ALCbackend, backend);
    }

    return nullptr;
}


ALCbackendFactory *ALCwasapiBackendFactory_getFactory(void)
{
    static ALCwasapiBackendFactory factory{ALCWASAPIBACKENDFACTORY_INITIALIZER};
    return STATIC_CAST(ALCbackendFactory, &factory);
}
