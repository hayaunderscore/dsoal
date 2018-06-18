/* DirectSound COM interface
 *
 * Copyright 2009 Maarten Lankhorst
 *
 * Some code taken from the original dsound-openal implementation
 *    Copyright 2007-2009 Chris Robinson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define CONST_VTABLE
#include <stdarg.h>
#include <string.h>

#define INITGUID
#include "windows.h"
#include "dsound.h"
#include "mmsystem.h"
#include "ks.h"
#include <devpropdef.h>

#include "dsound_private.h"


DEFINE_DEVPROPKEY(DEVPKEY_Device_FriendlyName, 0xa45c254e,0xdf1c,0x4efd,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0, 14);

DEFINE_GUID(CLSID_DirectSoundPrivate,0x11ab3ec0,0x25ec,0x11d1,0xa4,0xd8,0x00,0xc0,0x4f,0xc2,0x8a,0xca);

DEFINE_GUID(DSPROPSETID_DirectSoundDevice,0x84624f82,0x25ec,0x11d1,0xa4,0xd8,0x00,0xc0,0x4f,0xc2,0x8a,0xca);

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

#ifndef DS_INCOMPLETE
#define DS_INCOMPLETE                   ((HRESULT)0x08780020)
#endif

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 3
#endif


/* TODO: when bufferlost is set, return from all calls except initialize with
 * DSERR_BUFFERLOST
 */
static const IDirectSoundBuffer8Vtbl DS8Buffer_Vtbl;
static const IDirectSound3DBufferVtbl DS8Buffer3d_Vtbl;
static const IDirectSoundNotifyVtbl DS8BufferNot_Vtbl;
static const IKsPropertySetVtbl DS8BufferProp_Vtbl;


static inline DS8Buffer *impl_from_IDirectSoundBuffer8(IDirectSoundBuffer8 *iface)
{
    return CONTAINING_RECORD(iface, DS8Buffer, IDirectSoundBuffer8_iface);
}

static inline DS8Buffer *impl_from_IDirectSoundBuffer(IDirectSoundBuffer *iface)
{
    return CONTAINING_RECORD(iface, DS8Buffer, IDirectSoundBuffer8_iface);
}

static inline DS8Buffer *impl_from_IDirectSound3DBuffer(IDirectSound3DBuffer *iface)
{
    return CONTAINING_RECORD(iface, DS8Buffer, IDirectSound3DBuffer_iface);
}

static inline DS8Buffer *impl_from_IDirectSoundNotify(IDirectSoundNotify *iface)
{
    return CONTAINING_RECORD(iface, DS8Buffer, IDirectSoundNotify_iface);
}

static inline DS8Buffer *impl_from_IKsPropertySet(IKsPropertySet *iface)
{
    return CONTAINING_RECORD(iface, DS8Buffer, IKsPropertySet_iface);
}


/* Should be called with critsect held and context set.. */
static void DS8Buffer_addnotify(DS8Buffer *buf)
{
    DS8Primary *prim = buf->primary;
    DS8Buffer **list;
    DWORD i;

    list = prim->notifies;
    for(i = 0; i < prim->nnotifies; ++i)
    {
        if(buf == list[i])
        {
            ERR("Buffer %p already in notification list\n", buf);
            return;
        }
    }
    if(prim->nnotifies == prim->sizenotifies)
    {
        list = HeapReAlloc(GetProcessHeap(), 0, list, (prim->nnotifies + 1) * sizeof(*list));
        if(!list) return;
        prim->sizenotifies++;
    }
    list[prim->nnotifies++] = buf;
    prim->notifies = list;
}


static const char *get_fmtstr_PCM(const DS8Primary *prim, const WAVEFORMATEX *format, WAVEFORMATEXTENSIBLE *out)
{
    out->Format = *format;
    out->Format.cbSize = 0;

    if(out->Format.nChannels != 1 && out->Format.nChannels != 2 &&
       !prim->SupportedExt[EXT_MCFORMATS])
    {
        WARN("Multi-channel not available\n");
        return NULL;
    }

    if(format->wBitsPerSample == 8)
    {
        switch(format->nChannels)
        {
        case 1: return "AL_FORMAT_MONO8";
        case 2: return "AL_FORMAT_STEREO8";
        case 4: return "AL_FORMAT_QUAD8";
        case 6: return "AL_FORMAT_51CHN8";
        case 7: return "AL_FORMAT_61CHN8";
        case 8: return "AL_FORMAT_71CHN8";
        }
    }
    else if(format->wBitsPerSample == 16)
    {
        switch(format->nChannels)
        {
        case 1: return "AL_FORMAT_MONO16";
        case 2: return "AL_FORMAT_STEREO16";
        case 4: return "AL_FORMAT_QUAD16";
        case 6: return "AL_FORMAT_51CHN16";
        case 7: return "AL_FORMAT_61CHN16";
        case 8: return "AL_FORMAT_71CHN16";
        }
    }

    FIXME("Could not get OpenAL format (%d-bit, %d channels)\n",
          format->wBitsPerSample, format->nChannels);
    return NULL;
}

static const char *get_fmtstr_FLOAT(const DS8Primary *prim, const WAVEFORMATEX *format, WAVEFORMATEXTENSIBLE *out)
{
    out->Format = *format;
    out->Format.cbSize = 0;

    if(out->Format.nChannels != 1 && out->Format.nChannels != 2 &&
       !prim->SupportedExt[EXT_MCFORMATS])
    {
        WARN("Multi-channel not available\n");
        return NULL;
    }

    if(format->wBitsPerSample == 32 && prim->SupportedExt[EXT_FLOAT32])
    {
        switch(format->nChannels)
        {
        case 1: return "AL_FORMAT_MONO_FLOAT32";
        case 2: return "AL_FORMAT_STEREO_FLOAT32";
        case 4: return "AL_FORMAT_QUAD32";
        case 6: return "AL_FORMAT_51CHN32";
        case 7: return "AL_FORMAT_61CHN32";
        case 8: return "AL_FORMAT_71CHN32";
        }
    }

    FIXME("Could not get OpenAL format (%d-bit, %d channels)\n",
          format->wBitsPerSample, format->nChannels);
    return NULL;
}

/* Speaker configs */
#define MONO SPEAKER_FRONT_CENTER
#define STEREO (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT)
#define REAR (SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define QUAD (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X5DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X6DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_CENTER|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)

static const char *get_fmtstr_EXT(const DS8Primary *prim, const WAVEFORMATEX *format, WAVEFORMATEXTENSIBLE *out)
{
    *out = *CONTAINING_RECORD(format, const WAVEFORMATEXTENSIBLE, Format);
    out->Format.cbSize = sizeof(*out) - sizeof(out->Format);

    if(!out->Samples.wValidBitsPerSample)
        out->Samples.wValidBitsPerSample = out->Format.wBitsPerSample;
    else if(out->Samples.wValidBitsPerSample != out->Format.wBitsPerSample)
    {
        FIXME("Padded samples not supported (%u of %u)\n", out->Samples.wValidBitsPerSample, out->Format.wBitsPerSample);
        return NULL;
    }

    if(out->dwChannelMask != MONO && out->dwChannelMask != STEREO &&
       !prim->SupportedExt[EXT_MCFORMATS])
    {
        WARN("Multi-channel not available\n");
        return NULL;
    }

    if(IsEqualGUID(&out->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
    {
        if(out->Samples.wValidBitsPerSample == 8)
        {
            switch(out->dwChannelMask)
            {
            case   MONO: return "AL_FORMAT_MONO8";
            case STEREO: return "AL_FORMAT_STEREO8";
            case   REAR: return "AL_FORMAT_REAR8";
            case   QUAD: return "AL_FORMAT_QUAD8";
            case X5DOT1: return "AL_FORMAT_51CHN8";
            case X6DOT1: return "AL_FORMAT_61CHN8";
            case X7DOT1: return "AL_FORMAT_71CHN8";
            }
        }
        else if(out->Samples.wValidBitsPerSample == 16)
        {
            switch(out->dwChannelMask)
            {
            case   MONO: return "AL_FORMAT_MONO16";
            case STEREO: return "AL_FORMAT_STEREO16";
            case   REAR: return "AL_FORMAT_REAR16";
            case   QUAD: return "AL_FORMAT_QUAD16";
            case X5DOT1: return "AL_FORMAT_51CHN16";
            case X6DOT1: return "AL_FORMAT_61CHN16";
            case X7DOT1: return "AL_FORMAT_71CHN16";
            }
        }

        FIXME("Could not get OpenAL PCM format (%d-bit, channelmask %#lx)\n",
              out->Samples.wValidBitsPerSample, out->dwChannelMask);
        return NULL;
    }
    else if(IsEqualGUID(&out->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            prim->SupportedExt[EXT_FLOAT32])
    {
        if(out->Samples.wValidBitsPerSample == 32)
        {
            switch(out->dwChannelMask)
            {
            case   MONO: return "AL_FORMAT_MONO_FLOAT32";
            case STEREO: return "AL_FORMAT_STEREO_FLOAT32";
            case   REAR: return "AL_FORMAT_REAR32";
            case   QUAD: return "AL_FORMAT_QUAD32";
            case X5DOT1: return "AL_FORMAT_51CHN32";
            case X6DOT1: return "AL_FORMAT_61CHN32";
            case X7DOT1: return "AL_FORMAT_71CHN32";
            }
        }
        else
        {
            WARN("Invalid float bits: %u\n", out->Samples.wValidBitsPerSample);
            return NULL;
        }

        FIXME("Could not get OpenAL float format (%d-bit, channelmask %#lx)\n",
              out->Samples.wValidBitsPerSample, out->dwChannelMask);
        return NULL;
    }
    else if(!IsEqualGUID(&out->SubFormat, &GUID_NULL))
        ERR("Unhandled extensible format: %s\n", debugstr_guid(&out->SubFormat));
    return NULL;
}

static void DS8Data_Release(DS8Data *This);
static HRESULT DS8Data_Create(DS8Data **ppv, const DSBUFFERDESC *desc, DS8Primary *prim)
{
    HRESULT hr = DSERR_INVALIDPARAM;
    const WAVEFORMATEX *format;
    const char *fmt_str = NULL;
    DS8Data *pBuffer;
    DWORD buf_size;

    format = desc->lpwfxFormat;
    TRACE("Requested buffer format:\n"
          "    FormatTag      = 0x%04x\n"
          "    Channels       = %d\n"
          "    SamplesPerSec  = %lu\n"
          "    AvgBytesPerSec = %lu\n"
          "    BlockAlign     = %d\n"
          "    BitsPerSample  = %d\n",
          format->wFormatTag, format->nChannels,
          format->nSamplesPerSec, format->nAvgBytesPerSec,
          format->nBlockAlign, format->wBitsPerSample);

    if(format->nSamplesPerSec < DSBFREQUENCY_MIN || format->nSamplesPerSec > DSBFREQUENCY_MAX)
    {
        WARN("Invalid SamplesPerSec specified\n");
        return DSERR_INVALIDPARAM;
    }
    if(format->nBlockAlign <= 0)
    {
        WARN("Invalid BlockAlign specified\n");
        return DSERR_INVALIDPARAM;
    }

    if((format->wBitsPerSample%8) != 0)
    {
        WARN("Invalid BitsPerSample specified\n");
        return DSERR_INVALIDPARAM;
    }
    if(format->nBlockAlign != format->nChannels*format->wBitsPerSample/8)
    {
        WARN("Incorrect BlockAlign specified\n");
        return DSERR_INVALIDPARAM;
    }
    if(format->nAvgBytesPerSec != format->nBlockAlign*format->nSamplesPerSec)
    {
        WARN("Incorrect AvgBytesPerSec specified\n");
        return DSERR_INVALIDPARAM;
    }

    if((desc->dwFlags&(DSBCAPS_LOCSOFTWARE|DSBCAPS_LOCHARDWARE)) == (DSBCAPS_LOCSOFTWARE|DSBCAPS_LOCHARDWARE))
    {
        WARN("Hardware and software location requested\n");
        return DSERR_INVALIDPARAM;
    }

    buf_size  = desc->dwBufferBytes + format->nBlockAlign - 1;
    buf_size -= buf_size%format->nBlockAlign;
    if(buf_size < DSBSIZE_MIN) return DSERR_BUFFERTOOSMALL;
    if(buf_size > DSBSIZE_MAX) return DSERR_INVALIDPARAM;

    /* Generate a new buffer. Supporting the DSBCAPS_LOC* flags properly
     * will need the EAX-RAM extension. Currently, we just tell the app it
     * gets what it wanted. */
    if(!prim->SupportedExt[SOFTX_MAP_BUFFER])
        pBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pBuffer)+buf_size);
    else
        pBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pBuffer));
    if(!pBuffer) return E_OUTOFMEMORY;
    pBuffer->ref = 1;
    pBuffer->primary = prim;

    pBuffer->dsbflags = desc->dwFlags;
    if(!(pBuffer->dsbflags&(DSBCAPS_LOCSOFTWARE|DSBCAPS_LOCHARDWARE|DSBCAPS_LOCDEFER)))
        pBuffer->dsbflags |= DSBCAPS_LOCHARDWARE;
    pBuffer->buf_size = buf_size;

    if(format->wFormatTag == WAVE_FORMAT_PCM)
        fmt_str = get_fmtstr_PCM(prim, format, &pBuffer->format);
    else if(format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        fmt_str = get_fmtstr_FLOAT(prim, format, &pBuffer->format);
    else if(format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *wfe;

        if(format->cbSize != sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX) &&
            format->cbSize != sizeof(WAVEFORMATEXTENSIBLE))
            goto fail;

        wfe = CONTAINING_RECORD(format, const WAVEFORMATEXTENSIBLE, Format);
        TRACE("Extensible values:\n"
                "    Samples     = %d\n"
                "    ChannelMask = 0x%lx\n"
                "    SubFormat   = %s\n",
                wfe->Samples.wReserved, wfe->dwChannelMask,
                debugstr_guid(&wfe->SubFormat));

        fmt_str = get_fmtstr_EXT(prim, format, &pBuffer->format);
    }
    else
        ERR("Unhandled formattag 0x%04x\n", format->wFormatTag);
    if(!fmt_str) goto fail;

    pBuffer->buf_format = alGetEnumValue(fmt_str);
    if(alGetError() != AL_NO_ERROR || pBuffer->buf_format == 0 ||
       pBuffer->buf_format == -1)
    {
        WARN("Could not get OpenAL format from %s\n", fmt_str);
        goto fail;
    }

    hr = E_OUTOFMEMORY;
    if(!prim->SupportedExt[SOFTX_MAP_BUFFER])
    {
        pBuffer->data = (BYTE*)(pBuffer+1);

        alGenBuffers(1, &pBuffer->bid);
        checkALError();
    }
    else
    {
        const ALbitfieldSOFT map_bits = AL_MAP_READ_BIT_SOFT | AL_MAP_WRITE_BIT_SOFT |
                                        AL_MAP_PERSISTENT_BIT_SOFT;
        alGenBuffers(1, &pBuffer->bid);
        prim->ExtAL->BufferStorageSOFT(pBuffer->bid, pBuffer->buf_format, NULL, pBuffer->buf_size,
                                       pBuffer->format.Format.nSamplesPerSec, map_bits);
        pBuffer->data = prim->ExtAL->MapBufferSOFT(pBuffer->bid, 0, pBuffer->buf_size, map_bits);
        checkALError();

        if(!pBuffer->data) goto fail;
    }

    *ppv = pBuffer;
    return S_OK;

fail:
    DS8Data_Release(pBuffer);
    return hr;
}

static void DS8Data_AddRef(DS8Data *data)
{
    InterlockedIncrement(&data->ref);
}

/* This function is always called with the device lock held */
static void DS8Data_Release(DS8Data *This)
{
    if(InterlockedDecrement(&This->ref)) return;

    TRACE("Deleting %p\n", This);
    if(This->bid)
    {
        DS8Primary *prim = This->primary;
        if(prim->SupportedExt[SOFTX_MAP_BUFFER])
            prim->ExtAL->UnmapBufferSOFT(This->bid);
        alDeleteBuffers(1, &This->bid);
        checkALError();
    }
    HeapFree(GetProcessHeap(), 0, This);
}


HRESULT DS8Buffer_Create(DS8Buffer **ppv, DS8Primary *prim, IDirectSoundBuffer *orig, BOOL prim_emu)
{
    DS8Buffer *This = NULL;
    HRESULT hr;
    DWORD i;

    *ppv = NULL;
    EnterCriticalSection(prim->crst);
    if(prim_emu)
    {
        This = &prim->writable_buf;
        memset(This, 0, sizeof(*This));
    }
    else for(i = 0;i < prim->NumBufferGroups;++i)
    {
        if(prim->BufferGroups[i].FreeBuffers)
        {
            int idx = CTZ64(prim->BufferGroups[i].FreeBuffers);
            This = prim->BufferGroups[i].Buffers + idx;
            memset(This, 0, sizeof(*This));
            prim->BufferGroups[i].FreeBuffers &= ~(U64(1) << idx);
            break;
        }
    }
    LeaveCriticalSection(prim->crst);
    if(!This)
    {
        WARN("Out of buffers\n");
        return DSERR_ALLOCATED;
    }

    This->IDirectSoundBuffer8_iface.lpVtbl = &DS8Buffer_Vtbl;
    This->IDirectSound3DBuffer_iface.lpVtbl = &DS8Buffer3d_Vtbl;
    This->IDirectSoundNotify_iface.lpVtbl = &DS8BufferNot_Vtbl;
    This->IKsPropertySet_iface.lpVtbl = &DS8BufferProp_Vtbl;

    This->primary = prim;
    This->ctx = prim->ctx;
    This->ExtAL = prim->ExtAL;
    This->crst = prim->crst;
    This->ref = This->all_ref = 1;

    if(orig)
    {
        DS8Buffer *org = impl_from_IDirectSoundBuffer(orig);
        hr = DSERR_BUFFERLOST;
        if(org->bufferlost)
            goto fail;
        DS8Data_AddRef(org->buffer);
        This->buffer = org->buffer;
    }

    /* Disable until initialized.. */
    This->ds3dmode = DS3DMODE_DISABLE;

    *ppv = This;
    return DS_OK;

fail:
    DS8Buffer_Destroy(This);
    return hr;
}

void DS8Buffer_Destroy(DS8Buffer *This)
{
    DS8Primary *prim = This->primary;
    DWORD i;

    if(!prim) return;
    TRACE("Destroying %p\n", This);

    EnterCriticalSection(prim->crst);
    /* Remove from list, if in list */
    for(i = 0;i < prim->nnotifies;++i)
    {
        if(This == prim->notifies[i])
        {
            prim->notifies[i] = prim->notifies[--prim->nnotifies];
            break;
        }
    }

    setALContext(This->ctx);
    if(This->source)
    {
        alSourceStop(This->source);
        alSourcei(This->source, AL_BUFFER, 0);
        checkALError();

        prim->sources[prim->parent->share->nsources++] = This->source;
        This->source = 0;
    }
    if(This->stream_bids[0])
        alDeleteBuffers(QBUFFERS, This->stream_bids);

    if(This->buffer)
        DS8Data_Release(This->buffer);

    popALContext();

    HeapFree(GetProcessHeap(), 0, This->notify);

    for(i = 0;i < prim->NumBufferGroups;++i)
    {
        DWORD_PTR idx = This - prim->BufferGroups[i].Buffers;
        if(idx < 64)
        {
            prim->BufferGroups[i].FreeBuffers |= U64(1) << idx;
            This = NULL;
            break;
        }
    }
    LeaveCriticalSection(prim->crst);
}


static HRESULT WINAPI DS8Buffer_QueryInterface(IDirectSoundBuffer8 *iface, REFIID riid, void **ppv)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);

    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    *ppv = NULL;
    if(IsEqualIID(riid, &IID_IUnknown))
        *ppv = &This->IDirectSoundBuffer8_iface;
    else if(IsEqualIID(riid, &IID_IDirectSoundBuffer))
        *ppv = &This->IDirectSoundBuffer8_iface;
    else if(IsEqualIID(riid, &IID_IDirectSoundBuffer8))
    {
        if(This->primary->parent->is_8)
            *ppv = &This->IDirectSoundBuffer8_iface;
    }
    else if(IsEqualIID(riid, &IID_IDirectSound3DBuffer))
    {
        if((This->buffer->dsbflags&DSBCAPS_CTRL3D))
            *ppv = &This->IDirectSound3DBuffer_iface;
    }
    else if(IsEqualIID(riid, &IID_IDirectSoundNotify))
    {
        if((This->buffer->dsbflags&DSBCAPS_CTRLPOSITIONNOTIFY))
            *ppv = &This->IDirectSoundNotify_iface;
    }
    else if(IsEqualIID(riid, &IID_IKsPropertySet))
        *ppv = &This->IKsPropertySet_iface;
    else
        FIXME("Unhandled GUID: %s\n", debugstr_guid(riid));

    if(*ppv)
    {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI DS8Buffer_AddRef(IDirectSoundBuffer8 *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    LONG ret;

    InterlockedIncrement(&This->all_ref);
    ret = InterlockedIncrement(&This->ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8Buffer_Release(IDirectSoundBuffer8 *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->ref);
    TRACE("new refcount %ld\n", ret);
    if(InterlockedDecrement(&This->all_ref) == 0)
        DS8Buffer_Destroy(This);

    return ret;
}

static HRESULT WINAPI DS8Buffer_GetCaps(IDirectSoundBuffer8 *iface, DSBCAPS *caps)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);

    TRACE("(%p)->(%p)\n", iface, caps);

    if(!caps || caps->dwSize < sizeof(*caps))
    {
        WARN("Invalid DSBCAPS (%p, %lu)\n", caps, (caps ? caps->dwSize : 0));
        return DSERR_INVALIDPARAM;
    }

    caps->dwFlags = This->buffer->dsbflags;
    caps->dwBufferBytes = This->buffer->buf_size;
    caps->dwUnlockTransferRate = 4096;
    caps->dwPlayCpuOverhead = 0;
    return S_OK;
}

HRESULT WINAPI DS8Buffer_GetCurrentPosition(IDirectSoundBuffer8 *iface, DWORD *playpos, DWORD *curpos)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    ALsizei writecursor, pos;
    DS8Data *data;

    TRACE("(%p)->(%p, %p)\n", iface, playpos, curpos);

    data = This->buffer;
    if(This->segsize != 0)
    {
        ALint queued = QBUFFERS;
        ALint status = 0;
        ALint ofs = 0;

        EnterCriticalSection(This->crst);

        setALContext(This->ctx);
        alGetSourcei(This->source, AL_BUFFERS_QUEUED, &queued);
        alGetSourcei(This->source, AL_BYTE_OFFSET, &ofs);
        alGetSourcei(This->source, AL_SOURCE_STATE, &status);
        checkALError();
        popALContext();

        if(status == AL_STOPPED)
            pos = This->segsize*queued + This->queue_base;
        else
            pos = ofs + This->queue_base;
        if(pos >= data->buf_size)
        {
            if(This->islooping)
                pos %= data->buf_size;
            else if(This->isplaying)
            {
                pos = data->buf_size;
                alSourceStop(This->source);
                alSourcei(This->source, AL_BUFFER, 0);
                This->curidx = 0;
                This->isplaying = FALSE;
            }
        }
        if(This->isplaying)
            writecursor = (This->segsize*QBUFFERS + pos) % data->buf_size;
        else
            writecursor = pos % data->buf_size;

        LeaveCriticalSection(This->crst);
    }
    else
    {
        const WAVEFORMATEX *format = &data->format.Format;
        ALint status = 0;
        ALint ofs = 0;

        setALContext(This->ctx);
        alGetSourcei(This->source, AL_BYTE_OFFSET, &ofs);
        alGetSourcei(This->source, AL_SOURCE_STATE, &status);
        checkALError();
        popALContext();

        /* AL_STOPPED means the source naturally reached its end, where
         * DirectSound's position should be at the end (OpenAL reports a 0
         * position). The Stop method correlates to pausing, which would put
         * the source into an AL_PAUSED state and hold its current position.
         */
        pos = (status == AL_STOPPED) ? data->buf_size : ofs;
        if(status == AL_PLAYING)
        {
            writecursor = format->nSamplesPerSec / This->primary->refresh;
            writecursor *= format->nBlockAlign;
        }
        else
            writecursor = 0;
        writecursor = (writecursor + pos) % data->buf_size;
    }
    TRACE("%p Play pos = %u, write pos = %u\n", This, pos, writecursor);

    if(pos > data->buf_size)
    {
        ERR("playpos > buf_size\n");
        pos %= data->buf_size;
    }
    if(writecursor >= data->buf_size)
    {
        ERR("writepos >= buf_size\n");
        writecursor %= data->buf_size;
    }

    if(playpos) *playpos = pos;
    if(curpos)  *curpos = writecursor;

    return S_OK;
}

static HRESULT WINAPI DS8Buffer_GetFormat(IDirectSoundBuffer8 *iface, WAVEFORMATEX *wfx, DWORD allocated, DWORD *written)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr = S_OK;
    UINT size;

    TRACE("(%p)->(%p, %lu, %p)\n", iface, wfx, allocated, written);

    if(!wfx && !written)
    {
        WARN("Cannot report format or format size\n");
        return DSERR_INVALIDPARAM;
    }

    size = sizeof(This->buffer->format.Format) + This->buffer->format.Format.cbSize;
    if(wfx)
    {
        if(allocated < size)
            hr = DSERR_INVALIDPARAM;
        else
            memcpy(wfx, &This->buffer->format.Format, size);
    }
    if(written)
        *written = size;

    return hr;
}

static HRESULT WINAPI DS8Buffer_GetVolume(IDirectSoundBuffer8 *iface, LONG *vol)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr;

    TRACE("(%p)->(%p)\n", iface, vol);

    if(!vol)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    hr = DSERR_CONTROLUNAVAIL;
    if(!(This->buffer->dsbflags&DSBCAPS_CTRLVOLUME))
        WARN("Volume control not set\n");
    else
    {
        ALfloat gain = 1.0f;

        setALContext(This->ctx);
        alGetSourcef(This->source, AL_GAIN, &gain);
        checkALError();
        popALContext();

        *vol = clampI(gain_to_mB(gain), DSBVOLUME_MIN, DSBVOLUME_MAX);
        hr = DS_OK;
    }

    return hr;
}

static HRESULT WINAPI DS8Buffer_GetPan(IDirectSoundBuffer8 *iface, LONG *pan)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr;

    TRACE("(%p)->(%p)\n", iface, pan);

    if(!pan)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    hr = DSERR_CONTROLUNAVAIL;
    if(!(This->buffer->dsbflags&DSBCAPS_CTRLPAN))
        WARN("Panning control not set\n");
    else
    {
        if((This->buffer->dsbflags&DSBCAPS_CTRL3D))
            *pan = 0;
        else
        {
            ALfloat pos[3];

            setALContext(This->ctx);
            alGetSourcefv(This->source, AL_POSITION, pos);
            checkALError();
            popALContext();

            *pan = clampI((LONG)((pos[0]+0.5f)*(DSBPAN_RIGHT-DSBPAN_LEFT) + 0.5f) + DSBPAN_LEFT,
                          DSBPAN_LEFT, DSBPAN_RIGHT);
        }
        hr = DS_OK;
    }

    return hr;
}

static HRESULT WINAPI DS8Buffer_GetFrequency(IDirectSoundBuffer8 *iface, DWORD *freq)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr;

    TRACE("(%p)->(%p)\n", iface, freq);

    if(!freq)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    hr = DSERR_CONTROLUNAVAIL;
    if(!(This->buffer->dsbflags&DSBCAPS_CTRLFREQUENCY))
        WARN("Frequency control not set\n");
    else
    {
        ALfloat pitch = 1.0f;

        setALContext(This->ctx);
        alGetSourcefv(This->source, AL_PITCH, &pitch);
        checkALError();
        popALContext();

        *freq = (DWORD)(This->buffer->format.Format.nSamplesPerSec * pitch);
        hr = DS_OK;
    }

    return hr;
}

HRESULT WINAPI DS8Buffer_GetStatus(IDirectSoundBuffer8 *iface, DWORD *status)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    ALint state, looping;

    TRACE("(%p)->(%p)\n", iface, status);

    if(!status)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }
    *status = 0;

    if(This->segsize == 0)
    {
        setALContext(This->ctx);
        alGetSourcei(This->source, AL_SOURCE_STATE, &state);
        alGetSourcei(This->source, AL_LOOPING, &looping);
        checkALError();
        popALContext();
    }
    else
    {
        EnterCriticalSection(This->crst);
        state = This->isplaying ? AL_PLAYING : AL_PAUSED;
        looping = This->islooping;
        LeaveCriticalSection(This->crst);
    }

    if((This->buffer->dsbflags&DSBCAPS_LOCDEFER))
    {
        if((This->buffer->dsbflags&DSBCAPS_LOCSOFTWARE))
            *status |= DSBSTATUS_LOCSOFTWARE;
        else if((This->buffer->dsbflags&DSBCAPS_LOCHARDWARE))
            *status |= DSBSTATUS_LOCHARDWARE;
    }
    if(state == AL_PLAYING)
        *status |= DSBSTATUS_PLAYING | (looping ? DSBSTATUS_LOOPING : 0);

    TRACE("%p status = 0x%08lx\n", This, *status);
    return S_OK;
}

HRESULT WINAPI DS8Buffer_Initialize(IDirectSoundBuffer8 *iface, IDirectSound *ds, const DSBUFFERDESC *desc)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    DS3DBUFFER *ds3dbuffer;
    DS8Primary *prim;
    DS8Data *data;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", iface, ds, desc);

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    hr = DSERR_ALREADYINITIALIZED;
    if(This->source) goto out;

    if(!This->buffer)
    {
        hr = DSERR_INVALIDPARAM;
        if(!desc)
        {
            WARN("Missing DSound buffer description\n");
            goto out;
        }
        if(!desc->lpwfxFormat)
        {
            WARN("Missing buffer format (%p)\n", This);
            goto out;
        }
        if((desc->dwFlags&DSBCAPS_CTRL3D) && desc->lpwfxFormat->nChannels != 1)
        {
            if(This->primary->parent->is_8)
            {
                /* DirectSoundBuffer8 objects aren't allowed non-mono 3D
                 * buffers */
                WARN("Can't create multi-channel 3D buffers\n");
                goto out;
            }
            else
            {
                static int once = 0;
                if(!once++)
                    ERR("Multi-channel 3D sounds are not spatialized\n");
            }
        }
        if((desc->dwFlags&DSBCAPS_CTRLPAN) && desc->lpwfxFormat->nChannels != 1)
        {
            static int once = 0;
            if(!once++)
                ERR("Panning for multi-channel buffers is not supported\n");
        }

        hr = DS8Data_Create(&This->buffer, desc, This->primary);
        if(FAILED(hr)) goto out;

        data = This->buffer;
        if(data->format.Format.wBitsPerSample == 8)
            memset(data->data, 0x80, data->buf_size);
        else
            memset(data->data, 0x00, data->buf_size);
    }

    prim = This->primary;
    data = This->buffer;
    if(!(data->dsbflags&DSBCAPS_STATIC) && !prim->SupportedExt[SOFTX_MAP_BUFFER])
    {
        This->segsize = (data->format.Format.nAvgBytesPerSec+prim->refresh-1) / prim->refresh;
        This->segsize = clampI(This->segsize, data->format.Format.nBlockAlign, 2048);
        This->segsize += data->format.Format.nBlockAlign - 1;
        This->segsize -= This->segsize%data->format.Format.nBlockAlign;

        alGenBuffers(QBUFFERS, This->stream_bids);
        checkALError();
    }

    hr = DSERR_ALLOCATED;
    if(!prim->parent->share->nsources)
        goto out;

    This->source = prim->sources[--(prim->parent->share->nsources)];
    alSourceRewind(This->source);
    alSourcef(This->source, AL_GAIN, 1.0f);
    alSourcef(This->source, AL_PITCH, 1.0f);
    checkALError();

    ds3dbuffer = &This->params;
    ds3dbuffer->dwSize = sizeof(This->params);
    ds3dbuffer->vPosition.x = 0.0f;
    ds3dbuffer->vPosition.y = 0.0f;
    ds3dbuffer->vPosition.z = 0.0f;
    ds3dbuffer->vVelocity.x = 0.0f;
    ds3dbuffer->vVelocity.y = 0.0f;
    ds3dbuffer->vVelocity.z = 0.0f;
    ds3dbuffer->dwInsideConeAngle = DS3D_DEFAULTCONEANGLE;
    ds3dbuffer->dwOutsideConeAngle = DS3D_DEFAULTCONEANGLE;
    ds3dbuffer->vConeOrientation.x = 0.0f;
    ds3dbuffer->vConeOrientation.y = 0.0f;
    ds3dbuffer->vConeOrientation.z = 1.0f;
    ds3dbuffer->lConeOutsideVolume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
    ds3dbuffer->flMinDistance = DS3D_DEFAULTMINDISTANCE;
    ds3dbuffer->flMaxDistance = DS3D_DEFAULTMAXDISTANCE;
    ds3dbuffer->dwMode = DS3DMODE_NORMAL;

    if((data->dsbflags&DSBCAPS_CTRL3D))
    {
        union BufferParamFlags dirty = { 0 };

        if(prim->auxslot != 0)
            alSource3i(This->source, AL_AUXILIARY_SEND_FILTER, prim->auxslot, 0, AL_FILTER_NULL);

        dirty.bit.pos = 1;
        dirty.bit.vel = 1;
        dirty.bit.cone_angles = 1;
        dirty.bit.cone_orient = 1;
        dirty.bit.cone_outsidevolume = 1;
        dirty.bit.min_distance = 1;
        dirty.bit.max_distance = 1;
        dirty.bit.mode = 1;
        DS8Buffer_SetParams(This, ds3dbuffer, dirty.flags);
        checkALError();
    }
    else
    {
        ALuint source = This->source;

        if(prim->auxslot != 0)
        {
            /* Simple hack to make reverb affect non-3D sounds too */
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, prim->auxslot, 0, AL_FILTER_NULL);
            /*alSource3i(source, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);*/
        }

        /* Non-3D sources aren't distance attenuated */
        This->ds3dmode = DS3DMODE_DISABLE;
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, -1.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
        alSourcef(source, AL_CONE_OUTER_GAIN, 1.0f);
        alSourcef(source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(source, AL_MAX_DISTANCE, 1000.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(source, AL_CONE_INNER_ANGLE, 360);
        alSourcei(source, AL_CONE_OUTER_ANGLE, 360);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        checkALError();
    }
    hr = S_OK;

out:
    popALContext();
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Buffer_Lock(IDirectSoundBuffer8 *iface, DWORD ofs, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    DWORD remain;

    TRACE("(%p)->(%lu, %lu, %p, %p, %p, %p, 0x%lx)\n", This, ofs, bytes, ptr1, len1, ptr2, len2, flags);

    if(!ptr1 || !len1)
    {
        WARN("Invalid pointer/len %p %p\n", ptr1, len1);
        return DSERR_INVALIDPARAM;
    }

    *ptr1 = NULL;
    *len1 = 0;
    if(ptr2) *ptr2 = NULL;
    if(len2) *len2 = 0;

    if((flags&DSBLOCK_FROMWRITECURSOR))
        DS8Buffer_GetCurrentPosition(iface, NULL, &ofs);
    else if(ofs >= (DWORD)This->buffer->buf_size)
    {
        WARN("Invalid ofs %lu\n", ofs);
        return DSERR_INVALIDPARAM;
    }
    if((flags&DSBLOCK_ENTIREBUFFER))
        bytes = This->buffer->buf_size;
    else if(bytes > (DWORD)This->buffer->buf_size)
    {
        WARN("Invalid size %lu\n", bytes);
        return DSERR_INVALIDPARAM;
    }

    if(InterlockedExchange(&This->buffer->locked, TRUE) == TRUE)
    {
        WARN("Already locked\n");
        return DSERR_INVALIDPARAM;
    }

    *ptr1 = This->buffer->data + ofs;
    if(bytes >= (DWORD)This->buffer->buf_size-ofs)
    {
        *len1 = This->buffer->buf_size - ofs;
        remain = bytes - *len1;
    }
    else
    {
        *len1 = bytes;
        remain = 0;
    }

    if(ptr2 && len2 && remain)
    {
        *ptr2 = This->buffer->data;
        *len2 = remain;
    }

    return DS_OK;
}

static HRESULT WINAPI DS8Buffer_Play(IDirectSoundBuffer8 *iface, DWORD res1, DWORD prio, DWORD flags)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    ALint state = AL_STOPPED;
    DS8Data *data;
    HRESULT hr;

    TRACE("(%p)->(%lu, %lu, %lu)\n", iface, res1, prio, flags);

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    hr = DSERR_BUFFERLOST;
    if(This->bufferlost)
    {
        WARN("Buffer %p lost\n", This);
        goto out;
    }

    data = This->buffer;
    if((data->dsbflags&DSBCAPS_LOCDEFER))
    {
        if(!(data->dsbflags&(DSBCAPS_LOCHARDWARE|DSBCAPS_LOCSOFTWARE)))
        {
            if(flags & DSBPLAY_LOCSOFTWARE)
                data->dsbflags |= DSBCAPS_LOCSOFTWARE;
            else
                data->dsbflags |= DSBCAPS_LOCHARDWARE;
        }
    }
    else if(prio)
    {
        ERR("Invalid priority set for non-deferred buffer %p, %lu!\n", This->buffer, prio);
        hr = DSERR_INVALIDPARAM;
        goto out;
    }

    if(This->segsize != 0)
    {
        This->islooping = !!(flags&DSBPLAY_LOOPING);
        if(This->isplaying) state = AL_PLAYING;
    }
    else
    {
        alSourcei(This->source, AL_LOOPING, (flags&DSBPLAY_LOOPING) ? AL_TRUE : AL_FALSE);
        alGetSourcei(This->source, AL_SOURCE_STATE, &state);
    }
    checkALError();

    hr = S_OK;
    if(state == AL_PLAYING)
        goto out;

    if(This->segsize == 0)
    {
        if(state != AL_PAUSED)
            alSourcei(This->source, AL_BUFFER, data->bid);
        alSourcePlay(This->source);
    }
    else
    {
        alSourceRewind(This->source);
        alSourcei(This->source, AL_BUFFER, 0);
        This->queue_base = This->data_offset % data->buf_size;
        This->curidx = 0;
    }
    if(alGetError() != AL_NO_ERROR)
    {
        ERR("Couldn't start source\n");
        alSourcei(This->source, AL_BUFFER, 0);
        checkALError();
        hr = DSERR_GENERIC;
        goto out;
    }
    This->isplaying = TRUE;
    This->playflags = flags;

    if(This->nnotify)
        DS8Buffer_addnotify(This);

out:
    popALContext();
    LeaveCriticalSection(This->crst);
    return hr;
}

static HRESULT WINAPI DS8Buffer_SetCurrentPosition(IDirectSoundBuffer8 *iface, DWORD pos)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    DS8Data *data;

    TRACE("(%p)->(%lu)\n", iface, pos);

    data = This->buffer;
    if(pos >= (DWORD)data->buf_size)
        return DSERR_INVALIDPARAM;
    pos -= pos%data->format.Format.nBlockAlign;

    EnterCriticalSection(This->crst);

    if(This->segsize != 0)
    {
        if(This->isplaying)
        {
            setALContext(This->ctx);
            /* Perform a flush, so the next timer update will restart at the
             * proper position */
            alSourceRewind(This->source);
            alSourcei(This->source, AL_BUFFER, 0);
            checkALError();
            popALContext();
        }
        This->queue_base = This->data_offset = pos;
        This->curidx = 0;
    }
    else
    {
        setALContext(This->ctx);
        alSourcei(This->source, AL_BYTE_OFFSET, pos);
        popALContext();
    }
    This->lastpos = pos;

    LeaveCriticalSection(This->crst);
    return DS_OK;
}

static HRESULT WINAPI DS8Buffer_SetFormat(IDirectSoundBuffer8 *iface, const WAVEFORMATEX *wfx)
{
    /* This call only works on primary buffers */
    WARN("(%p)->(%p)\n", iface, wfx);
    return DSERR_INVALIDCALL;
}

static HRESULT WINAPI DS8Buffer_SetVolume(IDirectSoundBuffer8 *iface, LONG vol)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%ld)\n", iface, vol);

    if(vol > DSBVOLUME_MAX || vol < DSBVOLUME_MIN)
    {
        WARN("Invalid volume (%ld)\n", vol);
        return DSERR_INVALIDPARAM;
    }

    if(!(This->buffer->dsbflags&DSBCAPS_CTRLVOLUME))
        hr = DSERR_CONTROLUNAVAIL;
    if(SUCCEEDED(hr))
    {
        ALfloat fvol = mB_to_gain(vol);
        setALContext(This->ctx);
        alSourcef(This->source, AL_GAIN, fvol);
        popALContext();
    }

    return hr;
}

static HRESULT WINAPI DS8Buffer_SetPan(IDirectSoundBuffer8 *iface, LONG pan)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%ld)\n", iface, pan);

    if(pan > DSBPAN_RIGHT || pan < DSBPAN_LEFT)
    {
        WARN("invalid parameter: pan = %ld\n", pan);
        return DSERR_INVALIDPARAM;
    }

    if(!(This->buffer->dsbflags&DSBCAPS_CTRLPAN))
        hr = DSERR_CONTROLUNAVAIL;
    else
    {
        if(!(This->buffer->dsbflags&DSBCAPS_CTRL3D))
        {
            ALfloat pos[3];
            pos[0] = (ALfloat)(pan-DSBPAN_LEFT)/(ALfloat)(DSBPAN_RIGHT-DSBPAN_LEFT) - 0.5f;
            pos[1] = 0.0f;
            /* NOTE: Strict movement along the X plane can cause the sound to
             * jump between left and right sharply. Using a curved path helps
             * smooth it out.
             */
            pos[2] = -sqrtf(1.0f - pos[0]*pos[0]);

            setALContext(This->ctx);
            alSourcefv(This->source, AL_POSITION, pos);
            checkALError();
            popALContext();
        }
    }

    return hr;
}

static HRESULT WINAPI DS8Buffer_SetFrequency(IDirectSoundBuffer8 *iface, DWORD freq)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%lu)\n", iface, freq);

    if(freq < DSBFREQUENCY_MIN || freq > DSBFREQUENCY_MAX)
    {
        WARN("invalid parameter: freq = %lu\n", freq);
        return DSERR_INVALIDPARAM;
    }

    if(!(This->buffer->dsbflags&DSBCAPS_CTRLFREQUENCY))
        hr = DSERR_CONTROLUNAVAIL;
    else
    {
        ALfloat pitch = 1.0f;
        if(freq != DSBFREQUENCY_ORIGINAL)
            pitch = freq / (ALfloat)This->buffer->format.Format.nSamplesPerSec;

        setALContext(This->ctx);
        alSourcef(This->source, AL_PITCH, pitch);
        checkALError();
        popALContext();
    }

    return hr;
}

static HRESULT WINAPI DS8Buffer_Stop(IDirectSoundBuffer8 *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);

    TRACE("(%p)->()\n", iface);

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    alSourcePause(This->source);
    checkALError();

    This->isplaying = FALSE;
    DS8Primary_triggernots(This->primary);

    popALContext();
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Buffer_Unlock(IDirectSoundBuffer8 *iface, void *ptr1, DWORD len1, void *ptr2, DWORD len2)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    DS8Data *buf = This->buffer;
    DWORD bufsize = buf->buf_size;
    DWORD_PTR ofs1, ofs2;
    DWORD_PTR boundary = (DWORD_PTR)buf->data;
    HRESULT hr;

    TRACE("(%p)->(%p, %lu, %p, %lu)\n", iface, ptr1, len1, ptr2, len2);

    if(InterlockedExchange(&This->buffer->locked, FALSE) == FALSE)
    {
        WARN("Not locked\n");
        return DSERR_INVALIDPARAM;
    }

    hr = DSERR_INVALIDPARAM;
    /* Make sure offset is between boundary and boundary + bufsize */
    ofs1 = (DWORD_PTR)ptr1;
    ofs2 = (DWORD_PTR)ptr2;
    if(ofs1 < boundary || (ofs2 && ofs2 != boundary))
        goto out;
    ofs1 -= boundary;
    ofs2 = 0;
    if(bufsize-ofs1 < len1 || len2 > ofs1)
        goto out;
    if(!ptr2)
        len2 = 0;

    hr = DS_OK;
    if(!len1 && !len2)
        goto out;

    if(This->primary->SupportedExt[SOFTX_MAP_BUFFER])
    {
        setALContext(This->ctx);
        This->ExtAL->FlushMappedBufferSOFT(buf->bid, 0, buf->buf_size);
        checkALError();
        popALContext();
    }
    else if(This->segsize == 0)
    {
        setALContext(This->ctx);
        alBufferData(buf->bid, buf->buf_format, buf->data, buf->buf_size,
                     buf->format.Format.nSamplesPerSec);
        checkALError();
        popALContext();
    }

out:
    if(hr != S_OK)
        WARN("Invalid parameters (0x%lx,%lu) (%p,%lu,%p,%lu)\n", boundary, bufsize, ptr1, len1, ptr2, len2);
    return hr;
}

static HRESULT WINAPI DS8Buffer_Restore(IDirectSoundBuffer8 *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", iface);

    EnterCriticalSection(This->crst);
    if(This->primary->parent->prio_level < DSSCL_WRITEPRIMARY ||
       iface == This->primary->write_emu)
    {
        This->bufferlost = 0;
        hr = S_OK;
    }
    else
        hr = DSERR_BUFFERLOST;
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Buffer_SetFX(IDirectSoundBuffer8 *iface, DWORD fxcount, DSEFFECTDESC *desc, DWORD *rescodes)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);
    ALenum state = AL_INITIAL;
    DS8Data *data;
    HRESULT hr;
    DWORD i;

    TRACE("(%p)->(%lu, %p, %p)\n", This, fxcount, desc, rescodes);

    data = This->buffer;
    if(!(data->dsbflags&DSBCAPS_CTRLFX))
    {
        WARN("FX control not set\n");
        return DSERR_CONTROLUNAVAIL;
    }

    if(data->locked)
    {
        WARN("Buffer is locked\n");
        return DSERR_INVALIDCALL;
    }

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    alGetSourcei(This->source, AL_SOURCE_STATE, &state);
    checkALError();
    if(This->segsize != 0 && state != AL_PLAYING)
        state = This->isplaying ? AL_PLAYING : AL_PAUSED;
    if(state == AL_PLAYING)
    {
        WARN("Buffer is playing\n");
        hr = DSERR_INVALIDCALL;
        goto done;
    }

    hr = DSERR_INVALIDPARAM;
    if(fxcount == 0)
    {
        if(desc || rescodes)
        {
            WARN("Non-NULL desc and/or result pointer specified with no effects.\n");
            goto done;
        }

        /* No effects; we can handle that */
        hr = DS_OK;
        goto done;
    }

    if(!desc || !rescodes)
    {
        WARN("NULL desc and/or result pointer specified.\n");
        goto done;
    }

    /* We don't (currently) handle DSound effects */
    for(i = 0;i < fxcount;++i)
    {
        FIXME("Cannot handle effect: %s\n", debugstr_guid(&desc[i].guidDSFXClass));
        rescodes[i] = DSFXR_FAILED;
    }
    hr = DS_INCOMPLETE;

done:
    popALContext();
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Buffer_AcquireResources(IDirectSoundBuffer8 *iface, DWORD flags, DWORD fxcount, DWORD *rescodes)
{
    DS8Buffer *This = impl_from_IDirectSoundBuffer8(iface);

    TRACE("(%p)->(%lu, %lu, %p)\n", This, flags, fxcount, rescodes);

    /* effects aren't supported at the moment.. */
    if(fxcount != 0 || rescodes)
    {
        WARN("Non-zero effect count and/or result pointer specified with no effects.\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    if((This->buffer->dsbflags&DSBCAPS_LOCDEFER))
    {
        This->buffer->dsbflags &= ~(DSBCAPS_LOCSOFTWARE|DSBCAPS_LOCHARDWARE);
        if((flags&DSBPLAY_LOCSOFTWARE))
            This->buffer->dsbflags |= DSBCAPS_LOCSOFTWARE;
        else
            This->buffer->dsbflags |= DSBCAPS_LOCHARDWARE;
    }
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Buffer_GetObjectInPath(IDirectSoundBuffer8 *iface, REFGUID guid, DWORD idx, REFGUID rguidiface, void **ppv)
{
    FIXME("(%p)->(%s, %lu, %s, %p) : stub!\n", iface, debugstr_guid(guid), idx, debugstr_guid(rguidiface), ppv);
    return E_NOTIMPL;
}

static const IDirectSoundBuffer8Vtbl DS8Buffer_Vtbl = {
    DS8Buffer_QueryInterface,
    DS8Buffer_AddRef,
    DS8Buffer_Release,
    DS8Buffer_GetCaps,
    DS8Buffer_GetCurrentPosition,
    DS8Buffer_GetFormat,
    DS8Buffer_GetVolume,
    DS8Buffer_GetPan,
    DS8Buffer_GetFrequency,
    DS8Buffer_GetStatus,
    DS8Buffer_Initialize,
    DS8Buffer_Lock,
    DS8Buffer_Play,
    DS8Buffer_SetCurrentPosition,
    DS8Buffer_SetFormat,
    DS8Buffer_SetVolume,
    DS8Buffer_SetPan,
    DS8Buffer_SetFrequency,
    DS8Buffer_Stop,
    DS8Buffer_Unlock,
    DS8Buffer_Restore,
    DS8Buffer_SetFX,
    DS8Buffer_AcquireResources,
    DS8Buffer_GetObjectInPath
};


void DS8Buffer_SetParams(DS8Buffer *This, const DS3DBUFFER *params, LONG flags)
{
    const ALuint source = This->source;
    union BufferParamFlags dirty = { flags };

    if(dirty.bit.pos)
        alSource3f(source, AL_POSITION, params->vPosition.x, params->vPosition.y,
                                       -params->vPosition.z);
    if(dirty.bit.vel)
        alSource3f(source, AL_VELOCITY, params->vVelocity.x, params->vVelocity.y,
                                       -params->vVelocity.z);
    if(dirty.bit.cone_angles)
    {
        alSourcei(source, AL_CONE_INNER_ANGLE, params->dwInsideConeAngle);
        alSourcei(source, AL_CONE_OUTER_ANGLE, params->dwOutsideConeAngle);
    }
    if(dirty.bit.cone_orient)
        alSource3f(source, AL_DIRECTION, params->vConeOrientation.x,
                                         params->vConeOrientation.y,
                                        -params->vConeOrientation.z);
    if(dirty.bit.cone_outsidevolume)
        alSourcef(source, AL_CONE_OUTER_GAIN, mB_to_gain(params->lConeOutsideVolume));
    if(dirty.bit.min_distance)
        alSourcef(source, AL_REFERENCE_DISTANCE, params->flMinDistance);
    if(dirty.bit.max_distance)
        alSourcef(source, AL_MAX_DISTANCE, params->flMaxDistance);
    if(dirty.bit.mode)
    {
        This->ds3dmode = params->dwMode;
        alSourcei(source, AL_SOURCE_RELATIVE, (params->dwMode!=DS3DMODE_NORMAL) ?
                                              AL_TRUE : AL_FALSE);
        alSourcef(source, AL_ROLLOFF_FACTOR, (params->dwMode==DS3DMODE_DISABLE) ?
                                             0.0f : This->primary->rollofffactor);
    }
}

static HRESULT WINAPI DS8Buffer3D_QueryInterface(IDirectSound3DBuffer *iface, REFIID riid, void **ppv)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    return DS8Buffer_QueryInterface(&This->IDirectSoundBuffer8_iface, riid, ppv);
}

static ULONG WINAPI DS8Buffer3D_AddRef(IDirectSound3DBuffer *iface)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    LONG ret;

    InterlockedIncrement(&This->all_ref);
    ret = InterlockedIncrement(&This->ds3d_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8Buffer3D_Release(IDirectSound3DBuffer *iface)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->ds3d_ref);
    TRACE("new refcount %ld\n", ret);
    if(InterlockedDecrement(&This->all_ref) == 0)
        DS8Buffer_Destroy(This);

    return ret;
}

static HRESULT WINAPI DS8Buffer3D_GetConeAngles(IDirectSound3DBuffer *iface, DWORD *pdwInsideConeAngle, DWORD *pdwOutsideConeAngle)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALint inangle, outangle;

    TRACE("(%p)->(%p, %p)\n", This, pdwInsideConeAngle, pdwOutsideConeAngle);
    if(!pdwInsideConeAngle || !pdwOutsideConeAngle)
    {
        WARN("Invalid pointers (%p, %p)\n", pdwInsideConeAngle, pdwOutsideConeAngle);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    alGetSourcei(This->source, AL_CONE_INNER_ANGLE, &inangle);
    alGetSourcei(This->source, AL_CONE_OUTER_ANGLE, &outangle);
    checkALError();

    popALContext();
    LeaveCriticalSection(This->crst);

    *pdwInsideConeAngle = inangle;
    *pdwOutsideConeAngle = outangle;
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetConeOrientation(IDirectSound3DBuffer *iface, D3DVECTOR *orient)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat dir[3];

    TRACE("(%p)->(%p)\n", This, orient);
    if(!orient)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcefv(This->source, AL_DIRECTION, dir);
    checkALError();
    popALContext();

    orient->x =  dir[0];
    orient->y =  dir[1];
    orient->z = -dir[2];
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetConeOutsideVolume(IDirectSound3DBuffer *iface, LONG *vol)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat gain;

    TRACE("(%p)->(%p)\n", This, vol);
    if(!vol)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcef(This->source, AL_CONE_OUTER_GAIN, &gain);
    checkALError();
    popALContext();

    *vol = clampI(gain_to_mB(gain), DSBVOLUME_MIN, DSBVOLUME_MAX);
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE *maxdist)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat dist;

    TRACE("(%p)->(%p)\n", This, maxdist);
    if(!maxdist)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcef(This->source, AL_MAX_DISTANCE, &dist);
    checkALError();
    popALContext();

    *maxdist = dist;
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE *mindist)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat dist;

    TRACE("(%p)->(%p)\n", This, mindist);
    if(!mindist)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcef(This->source, AL_REFERENCE_DISTANCE, &dist);
    checkALError();
    popALContext();

    *mindist = dist;
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetMode(IDirectSound3DBuffer *iface, DWORD *mode)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%p)\n", This, mode);
    if(!mode)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    *mode = This->ds3dmode;
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetPosition(IDirectSound3DBuffer *iface, D3DVECTOR *pos)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat alpos[3];

    TRACE("(%p)->(%p)\n", This, pos);
    if(!pos)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcefv(This->source, AL_POSITION, alpos);
    checkALError();
    popALContext();

    pos->x =  alpos[0];
    pos->y =  alpos[1];
    pos->z = -alpos[2];
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetVelocity(IDirectSound3DBuffer *iface, D3DVECTOR *vel)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    ALfloat alvel[3];

    TRACE("(%p)->(%p)\n", This, vel);
    if(!vel)
    {
        WARN("Invalid pointer\n");
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetSourcefv(This->source, AL_VELOCITY, alvel);
    checkALError();
    popALContext();

    vel->x =  alvel[0];
    vel->y =  alvel[1];
    vel->z = -alvel[2];
    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_GetAllParameters(IDirectSound3DBuffer *iface, DS3DBUFFER *ds3dbuffer)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%p)\n", iface, ds3dbuffer);

    if(!ds3dbuffer || ds3dbuffer->dwSize < sizeof(*ds3dbuffer))
    {
        WARN("Invalid parameters %p %lu\n", ds3dbuffer, ds3dbuffer ? ds3dbuffer->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);

    DS8Buffer3D_GetPosition(iface, &ds3dbuffer->vPosition);
    DS8Buffer3D_GetVelocity(iface, &ds3dbuffer->vVelocity);
    DS8Buffer3D_GetConeAngles(iface, &ds3dbuffer->dwInsideConeAngle, &ds3dbuffer->dwOutsideConeAngle);
    DS8Buffer3D_GetConeOrientation(iface, &ds3dbuffer->vConeOrientation);
    DS8Buffer3D_GetConeOutsideVolume(iface, &ds3dbuffer->lConeOutsideVolume);
    DS8Buffer3D_GetMinDistance(iface, &ds3dbuffer->flMinDistance);
    DS8Buffer3D_GetMaxDistance(iface, &ds3dbuffer->flMaxDistance);
    DS8Buffer3D_GetMode(iface, &ds3dbuffer->dwMode);

    popALContext();
    LeaveCriticalSection(This->crst);

    return DS_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetConeAngles(IDirectSound3DBuffer *iface, DWORD dwInsideConeAngle, DWORD dwOutsideConeAngle, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%lu, %lu, %lu)\n", This, dwInsideConeAngle, dwOutsideConeAngle, apply);
    if(dwInsideConeAngle > DS3D_MAXCONEANGLE ||
       dwOutsideConeAngle > DS3D_MAXCONEANGLE)
    {
        WARN("Invalid cone angles (%lu, %lu)\n", dwInsideConeAngle, dwOutsideConeAngle);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    if(apply == DS3D_DEFERRED)
    {
        This->params.dwInsideConeAngle = dwInsideConeAngle;
        This->params.dwOutsideConeAngle = dwOutsideConeAngle;
        This->dirty.bit.cone_angles = 1;
    }
    else
    {
        setALContext(This->ctx);
        alSourcei(This->source, AL_CONE_INNER_ANGLE, dwInsideConeAngle);
        alSourcei(This->source, AL_CONE_OUTER_ANGLE, dwOutsideConeAngle);
        checkALError();
        popALContext();
    }
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetConeOrientation(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%f, %f, %f, %lu)\n", This, x, y, z, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vConeOrientation.x = x;
        This->params.vConeOrientation.y = y;
        This->params.vConeOrientation.z = z;
        This->dirty.bit.cone_orient = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSource3f(This->source, AL_DIRECTION, x, y, -z);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetConeOutsideVolume(IDirectSound3DBuffer *iface, LONG vol, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%ld, %lu)\n", This, vol, apply);
    if(vol < DSBVOLUME_MIN || vol > DSBVOLUME_MAX)
    {
        WARN("Invalid volume (%ld)\n", vol);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.lConeOutsideVolume = vol;
        This->dirty.bit.cone_outsidevolume = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSourcef(This->source, AL_CONE_OUTER_GAIN, mB_to_gain(vol));
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE maxdist, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%f, %lu)\n", This, maxdist, apply);
    if(maxdist < 0.0f)
    {
        WARN("Invalid max distance (%f)\n", maxdist);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.flMaxDistance = maxdist;
        This->dirty.bit.max_distance = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSourcef(This->source, AL_MAX_DISTANCE, maxdist);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE mindist, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%f, %lu)\n", This, mindist, apply);
    if(mindist < 0.0f)
    {
        WARN("Invalid min distance (%f)\n", mindist);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.flMinDistance = mindist;
        This->dirty.bit.min_distance = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSourcef(This->source, AL_REFERENCE_DISTANCE, mindist);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetMode(IDirectSound3DBuffer *iface, DWORD mode, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%lu, %lu)\n", This, mode, apply);
    if(mode != DS3DMODE_NORMAL && mode != DS3DMODE_HEADRELATIVE &&
       mode != DS3DMODE_DISABLE)
    {
        WARN("Invalid mode (%lu)\n", mode);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    if(apply == DS3D_DEFERRED)
    {
        This->params.dwMode = mode;
        This->dirty.bit.mode = 1;
    }
    else
    {
        setALContext(This->ctx);
        alSourcei(This->source, AL_SOURCE_RELATIVE,
                  (mode != DS3DMODE_NORMAL) ? AL_TRUE : AL_FALSE);
        alSourcef(This->source, AL_ROLLOFF_FACTOR,
                  (mode == DS3DMODE_DISABLE) ? 0.0f : This->primary->rollofffactor);
        This->ds3dmode = mode;
        checkALError();
        popALContext();
    }
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetPosition(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%f, %f, %f, %lu)\n", This, x, y, z, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vPosition.x = x;
        This->params.vPosition.y = y;
        This->params.vPosition.z = z;
        This->dirty.bit.pos = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSource3f(This->source, AL_POSITION, x, y, -z);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetVelocity(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);

    TRACE("(%p)->(%f, %f, %f, %lu)\n", This, x, y, z, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vVelocity.x = x;
        This->params.vVelocity.y = y;
        This->params.vVelocity.z = z;
        This->dirty.bit.vel = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSource3f(This->source, AL_VELOCITY, x, y, -z);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Buffer3D_SetAllParameters(IDirectSound3DBuffer *iface, const DS3DBUFFER *ds3dbuffer, DWORD apply)
{
    DS8Buffer *This = impl_from_IDirectSound3DBuffer(iface);
    TRACE("(%p)->(%p, %lu)\n", This, ds3dbuffer, apply);

    if(!ds3dbuffer || ds3dbuffer->dwSize < sizeof(*ds3dbuffer))
    {
        WARN("Invalid DS3DBUFFER (%p, %lu)\n", ds3dbuffer, ds3dbuffer ? ds3dbuffer->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    if(ds3dbuffer->dwInsideConeAngle > DS3D_MAXCONEANGLE ||
       ds3dbuffer->dwOutsideConeAngle > DS3D_MAXCONEANGLE)
    {
        WARN("Invalid cone angles (%lu, %lu)\n",
             ds3dbuffer->dwInsideConeAngle, ds3dbuffer->dwOutsideConeAngle);
        return DSERR_INVALIDPARAM;
    }

    if(ds3dbuffer->lConeOutsideVolume > DSBVOLUME_MAX ||
       ds3dbuffer->lConeOutsideVolume < DSBVOLUME_MIN)
    {
        WARN("Invalid cone outside volume (%ld)\n", ds3dbuffer->lConeOutsideVolume);
        return DSERR_INVALIDPARAM;
    }

    if(ds3dbuffer->flMaxDistance < 0.0f)
    {
        WARN("Invalid max distance (%f)\n", ds3dbuffer->flMaxDistance);
        return DSERR_INVALIDPARAM;
    }

    if(ds3dbuffer->flMinDistance < 0.0f)
    {
        WARN("Invalid min distance (%f)\n", ds3dbuffer->flMinDistance);
        return DSERR_INVALIDPARAM;
    }

    if(ds3dbuffer->dwMode != DS3DMODE_NORMAL &&
       ds3dbuffer->dwMode != DS3DMODE_HEADRELATIVE &&
       ds3dbuffer->dwMode != DS3DMODE_DISABLE)
    {
        WARN("Invalid mode (%lu)\n", ds3dbuffer->dwMode);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params = *ds3dbuffer;
        This->params.dwSize = sizeof(This->params);
        This->dirty.bit.pos = 1;
        This->dirty.bit.vel = 1;
        This->dirty.bit.cone_angles = 1;
        This->dirty.bit.cone_orient = 1;
        This->dirty.bit.cone_outsidevolume = 1;
        This->dirty.bit.min_distance = 1;
        This->dirty.bit.max_distance = 1;
        This->dirty.bit.mode = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        union BufferParamFlags dirty = { 0 };
        dirty.bit.pos = 1;
        dirty.bit.vel = 1;
        dirty.bit.cone_angles = 1;
        dirty.bit.cone_orient = 1;
        dirty.bit.cone_outsidevolume = 1;
        dirty.bit.min_distance = 1;
        dirty.bit.max_distance = 1;
        dirty.bit.mode = 1;

        EnterCriticalSection(This->crst);
        setALContext(This->ctx);
        DS8Buffer_SetParams(This, ds3dbuffer, dirty.flags);
        checkALError();
        popALContext();
        LeaveCriticalSection(This->crst);
    }

    return S_OK;
}

static const IDirectSound3DBufferVtbl DS8Buffer3d_Vtbl =
{
    DS8Buffer3D_QueryInterface,
    DS8Buffer3D_AddRef,
    DS8Buffer3D_Release,
    DS8Buffer3D_GetAllParameters,
    DS8Buffer3D_GetConeAngles,
    DS8Buffer3D_GetConeOrientation,
    DS8Buffer3D_GetConeOutsideVolume,
    DS8Buffer3D_GetMaxDistance,
    DS8Buffer3D_GetMinDistance,
    DS8Buffer3D_GetMode,
    DS8Buffer3D_GetPosition,
    DS8Buffer3D_GetVelocity,
    DS8Buffer3D_SetAllParameters,
    DS8Buffer3D_SetConeAngles,
    DS8Buffer3D_SetConeOrientation,
    DS8Buffer3D_SetConeOutsideVolume,
    DS8Buffer3D_SetMaxDistance,
    DS8Buffer3D_SetMinDistance,
    DS8Buffer3D_SetMode,
    DS8Buffer3D_SetPosition,
    DS8Buffer3D_SetVelocity
};


static HRESULT WINAPI DS8BufferNot_QueryInterface(IDirectSoundNotify *iface, REFIID riid, void **ppv)
{
    DS8Buffer *This = impl_from_IDirectSoundNotify(iface);
    return DS8Buffer_QueryInterface(&This->IDirectSoundBuffer8_iface, riid, ppv);
}

static ULONG WINAPI DS8BufferNot_AddRef(IDirectSoundNotify *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundNotify(iface);
    LONG ret;

    InterlockedIncrement(&This->all_ref);
    ret = InterlockedIncrement(&This->not_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8BufferNot_Release(IDirectSoundNotify *iface)
{
    DS8Buffer *This = impl_from_IDirectSoundNotify(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->not_ref);
    TRACE("new refcount %ld\n", ret);
    if(InterlockedDecrement(&This->all_ref) == 0)
        DS8Buffer_Destroy(This);

    return ret;
}

static HRESULT WINAPI DS8BufferNot_SetNotificationPositions(IDirectSoundNotify *iface, DWORD count, const DSBPOSITIONNOTIFY *notifications)
{
    DS8Buffer *This = impl_from_IDirectSoundNotify(iface);
    DSBPOSITIONNOTIFY *nots;
    DWORD state;
    HRESULT hr;

    TRACE("(%p)->(%lu, %p))\n", iface, count, notifications);

    EnterCriticalSection(This->crst);
    hr = DSERR_INVALIDPARAM;
    if(count && !notifications)
        goto out;

    hr = DS8Buffer_GetStatus(&This->IDirectSoundBuffer8_iface, &state);
    if(FAILED(hr)) goto out;

    hr = DSERR_INVALIDCALL;
    if((state&DSBSTATUS_PLAYING))
        goto out;

    if(!count)
    {
        HeapFree(GetProcessHeap(), 0, This->notify);
        This->notify = 0;
        This->nnotify = 0;
        hr = S_OK;
    }
    else
    {
        DWORD i;

        hr = DSERR_INVALIDPARAM;
        for(i = 0;i < count;++i)
        {
            if(notifications[i].dwOffset >= (DWORD)This->buffer->buf_size &&
               notifications[i].dwOffset != (DWORD)DSBPN_OFFSETSTOP)
                goto out;
        }

        hr = E_OUTOFMEMORY;
        nots = HeapAlloc(GetProcessHeap(), 0, count*sizeof(*nots));
        if(!nots) goto out;
        memcpy(nots, notifications, count*sizeof(*nots));

        HeapFree(GetProcessHeap(), 0, This->notify);
        This->notify = nots;
        This->nnotify = count;

        hr = S_OK;
    }

out:
    LeaveCriticalSection(This->crst);
    return hr;
}

static const IDirectSoundNotifyVtbl DS8BufferNot_Vtbl =
{
    DS8BufferNot_QueryInterface,
    DS8BufferNot_AddRef,
    DS8BufferNot_Release,
    DS8BufferNot_SetNotificationPositions
};


static HRESULT WINAPI DS8BufferProp_QueryInterface(IKsPropertySet *iface, REFIID riid, void **ppv)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    return DS8Buffer_QueryInterface(&This->IDirectSoundBuffer8_iface, riid, ppv);
}

static ULONG WINAPI DS8BufferProp_AddRef(IKsPropertySet *iface)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    LONG ret;

    InterlockedIncrement(&This->all_ref);
    ret = InterlockedIncrement(&This->prop_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8BufferProp_Release(IKsPropertySet *iface)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->prop_ref);
    TRACE("new refcount %ld\n", ret);
    if(InterlockedDecrement(&This->all_ref) == 0)
        DS8Buffer_Destroy(This);

    return ret;
}

/* NOTE: Due to some apparent quirks in DSound, the listener properties are
         handled through secondary buffers. */
static HRESULT WINAPI DS8BufferProp_Get(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  LPVOID pInstanceData, ULONG cbInstanceData,
  LPVOID pPropData, ULONG cbPropData,
  ULONG *pcbReturned)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    HRESULT hr = E_PROP_ID_UNSUPPORTED;

    TRACE("(%p)->(%s, %lu, %p, %lu, %p, %lu, %p)\n", iface, debugstr_guid(guidPropSet),
          dwPropID, pInstanceData, cbInstanceData, pPropData, cbPropData, pcbReturned);

    if(!pcbReturned)
        return E_POINTER;
    *pcbReturned = 0;

#if 0
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_BufferProperties))
    {
    }
    else
#endif
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_ListenerProperties))
    {
        DS8Primary *prim = This->primary;

        EnterCriticalSection(This->crst);

        hr = DSERR_INVALIDPARAM;
        if(prim->effect == 0)
            hr = E_PROP_ID_UNSUPPORTED;
        else switch(dwPropID)
        {
        case DSPROPERTY_EAXLISTENER_ALLPARAMETERS:
            if(cbPropData >= sizeof(EAXLISTENERPROPERTIES))
            {
                union {
                    void *v;
                    EAXLISTENERPROPERTIES *props;
                } data = { pPropData };

                *data.props = prim->eax_prop;
                *pcbReturned = sizeof(EAXLISTENERPROPERTIES);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ROOM:
            if(cbPropData >= sizeof(LONG))
            {
                union {
                    void *v;
                    LONG *l;
                } data = { pPropData };

                *data.l = prim->eax_prop.lRoom;
                *pcbReturned = sizeof(LONG);
                hr = DS_OK;
            }
            break;
        case DSPROPERTY_EAXLISTENER_ROOMHF:
            if(cbPropData >= sizeof(LONG))
            {
                union {
                    void *v;
                    LONG *l;
                } data = { pPropData };

                *data.l = prim->eax_prop.lRoomHF;
                *pcbReturned = sizeof(LONG);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ROOMROLLOFFFACTOR:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    void *v;
                    FLOAT *fl;
                } data = { pPropData };

                *data.fl = prim->eax_prop.flRoomRolloffFactor;
                *pcbReturned = sizeof(FLOAT);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ENVIRONMENT:
            if(cbPropData >= sizeof(DWORD))
            {
                union {
                    void *v;
                    DWORD *dw;
                } data = { pPropData };

                *data.dw = prim->eax_prop.dwEnvironment;
                *pcbReturned = sizeof(DWORD);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    void *v;
                    FLOAT *fl;
                } data = { pPropData };

                *data.fl = prim->eax_prop.flEnvironmentSize;
                *pcbReturned = sizeof(FLOAT);
                hr = DS_OK;
            }
            break;
        case DSPROPERTY_EAXLISTENER_ENVIRONMENTDIFFUSION:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    void *v;
                    FLOAT *fl;
                } data = { pPropData };

                *data.fl = prim->eax_prop.flEnvironmentDiffusion;
                *pcbReturned = sizeof(FLOAT);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_AIRABSORPTIONHF:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    void *v;
                    FLOAT *fl;
                } data = { pPropData };

                *data.fl = prim->eax_prop.flAirAbsorptionHF;
                *pcbReturned = sizeof(FLOAT);
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_FLAGS:
            if(cbPropData >= sizeof(DWORD))
            {
                union {
                    void *v;
                    DWORD *dw;
                } data = { pPropData };

                *data.dw = prim->eax_prop.dwFlags;
                *pcbReturned = sizeof(DWORD);
                hr = DS_OK;
            }
            break;

        default:
            hr = E_PROP_ID_UNSUPPORTED;
            FIXME("Unhandled propid: 0x%08lx\n", dwPropID);
            break;
        }

        LeaveCriticalSection(This->crst);
    }
    else
        FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return hr;
}

static HRESULT WINAPI DS8BufferProp_Set(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  LPVOID pInstanceData, ULONG cbInstanceData,
  LPVOID pPropData, ULONG cbPropData)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    HRESULT hr = E_PROP_ID_UNSUPPORTED;

    TRACE("(%p)->(%s, %lu, %p, %lu, %p, %lu)\n", iface, debugstr_guid(guidPropSet),
          dwPropID, pInstanceData, cbInstanceData, pPropData, cbPropData);

#if 0
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_BufferProperties))
    {
    }
    else
#endif
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_ListenerProperties))
    {
        DS8Primary *prim = This->primary;
        DWORD propid = dwPropID & ~DSPROPERTY_EAXLISTENER_DEFERRED;
        BOOL immediate = !(dwPropID&DSPROPERTY_EAXLISTENER_DEFERRED);

        EnterCriticalSection(prim->crst);
        setALContext(prim->ctx);

        hr = DSERR_INVALIDPARAM;
        if(prim->effect == 0)
            hr = E_PROP_ID_UNSUPPORTED;
        else switch(propid)
        {
        case DSPROPERTY_EAXLISTENER_NONE: /* not setting any property, just applying */
            hr = DS_OK;
            break;

        case DSPROPERTY_EAXLISTENER_ALLPARAMETERS:
        do_allparams:
            if(cbPropData >= sizeof(EAXLISTENERPROPERTIES))
            {
                union {
                    const void *v;
                    const EAXLISTENERPROPERTIES *props;
                } data = { pPropData };

                /* FIXME: Need to validate property values... Ignore? Clamp? Error? */
                prim->eax_prop = *data.props;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_DENSITY,
                                     clampF(powf(data.props->flEnvironmentSize, 3.0f) / 16.0f,
                                            0.0f, 1.0f)
                                    );
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_DIFFUSION,
                                     data.props->flEnvironmentDiffusion);

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_GAIN,
                                     mB_to_gain(data.props->lRoom));
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_GAINHF,
                                     mB_to_gain(data.props->lRoomHF));

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_ROOM_ROLLOFF_FACTOR,
                                     data.props->flRoomRolloffFactor);

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_DECAY_TIME,
                                     data.props->flDecayTime);
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_DECAY_HFRATIO,
                                     data.props->flDecayHFRatio);

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_REFLECTIONS_GAIN,
                                     mB_to_gain(data.props->lReflections));
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_REFLECTIONS_DELAY,
                                     data.props->flReflectionsDelay);

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_LATE_REVERB_GAIN,
                                     mB_to_gain(data.props->lReverb));
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_LATE_REVERB_DELAY,
                                     data.props->flReverbDelay);

                prim->ExtAL->Effectf(prim->effect, AL_REVERB_AIR_ABSORPTION_GAINHF,
                                     mBF_to_gain(data.props->flAirAbsorptionHF));

                prim->ExtAL->Effecti(prim->effect, AL_REVERB_DECAY_HFLIMIT,
                                     (data.props->dwFlags&EAXLISTENERFLAGS_DECAYHFLIMIT) ?
                                     AL_TRUE : AL_FALSE);

                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ROOM:
            if(cbPropData >= sizeof(LONG))
            {
                union {
                    const void *v;
                    const LONG *l;
                } data = { pPropData };

                prim->eax_prop.lRoom = *data.l;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_GAIN,
                                     mB_to_gain(prim->eax_prop.lRoom));
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;
        case DSPROPERTY_EAXLISTENER_ROOMHF:
            if(cbPropData >= sizeof(LONG))
            {
                union {
                    const void *v;
                    const LONG *l;
                } data = { pPropData };

                prim->eax_prop.lRoomHF = *data.l;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_GAINHF,
                                     mB_to_gain(prim->eax_prop.lRoomHF));
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ROOMROLLOFFFACTOR:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    const void *v;
                    const FLOAT *fl;
                } data = { pPropData };

                prim->eax_prop.flRoomRolloffFactor = *data.fl;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_ROOM_ROLLOFF_FACTOR,
                                     prim->eax_prop.flRoomRolloffFactor);
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_ENVIRONMENT:
            if(cbPropData >= sizeof(DWORD))
            {
                union {
                    const void *v;
                    const DWORD *dw;
                } data = { pPropData };

                if(*data.dw <= EAX_MAX_ENVIRONMENT)
                {
                    /* Get the environment index's default and pass it down to
                     * ALLPARAMETERS */
                    propid = DSPROPERTY_EAXLISTENER_ALLPARAMETERS;
                    pPropData = (void*)&EnvironmentDefaults[*data.dw];
                    cbPropData = sizeof(EnvironmentDefaults[*data.dw]);
                    goto do_allparams;
                }
            }
            break;

        case DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    const void *v;
                    const FLOAT *fl;
                } data = { pPropData };

                if(*data.fl >= 1.0f && *data.fl <= 100.0f)
                {
                    float scale = (*data.fl)/prim->eax_prop.flEnvironmentSize;

                    prim->eax_prop.flEnvironmentSize = *data.fl;

                    if((prim->eax_prop.dwFlags&EAXLISTENERFLAGS_DECAYTIMESCALE))
                    {
                        prim->eax_prop.flDecayTime *= scale;
                        prim->eax_prop.flDecayTime = clampF(prim->eax_prop.flDecayTime, 0.1f, 20.0f);
                    }
                    if((prim->eax_prop.dwFlags&EAXLISTENERFLAGS_REFLECTIONSSCALE))
                    {
                        prim->eax_prop.lReflections -= gain_to_mB(scale);
                        prim->eax_prop.lReflections = clampI(prim->eax_prop.lReflections, -10000, 1000);
                    }
                    if((prim->eax_prop.dwFlags&EAXLISTENERFLAGS_REFLECTIONSDELAYSCALE))
                    {
                        prim->eax_prop.flReflectionsDelay *= scale;
                        prim->eax_prop.flReflectionsDelay = clampF(prim->eax_prop.flReflectionsDelay, 0.0f, 0.3f);
                    }
                    if((prim->eax_prop.dwFlags&EAXLISTENERFLAGS_REVERBSCALE))
                    {
                        prim->eax_prop.lReverb -= gain_to_mB(scale);
                        prim->eax_prop.lReverb = clampI(prim->eax_prop.lReverb, -10000, 2000);
                    }
                    if((prim->eax_prop.dwFlags&EAXLISTENERFLAGS_REVERBDELAYSCALE))
                    {
                        prim->eax_prop.flReverbDelay *= scale;
                        prim->eax_prop.flReverbDelay = clampF(prim->eax_prop.flReverbDelay, 0.0f, 0.1f);
                    }

                    /* Pass the updated environment properties down to ALLPARAMETERS */
                    propid = DSPROPERTY_EAXLISTENER_ALLPARAMETERS;
                    pPropData = (void*)&prim->eax_prop;
                    cbPropData = sizeof(prim->eax_prop);
                    goto do_allparams;
                }
            }
            break;
        case DSPROPERTY_EAXLISTENER_ENVIRONMENTDIFFUSION:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    const void *v;
                    const FLOAT *fl;
                } data = { pPropData };

                prim->eax_prop.flEnvironmentDiffusion = *data.fl;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_DIFFUSION,
                                     prim->eax_prop.flEnvironmentDiffusion);
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_AIRABSORPTIONHF:
            if(cbPropData >= sizeof(FLOAT))
            {
                union {
                    const void *v;
                    const FLOAT *fl;
                } data = { pPropData };

                prim->eax_prop.flAirAbsorptionHF = *data.fl;
                prim->ExtAL->Effectf(prim->effect, AL_REVERB_AIR_ABSORPTION_GAINHF,
                                     mBF_to_gain(prim->eax_prop.flAirAbsorptionHF));
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        case DSPROPERTY_EAXLISTENER_FLAGS:
            if(cbPropData >= sizeof(DWORD))
            {
                union {
                    const void *v;
                    const DWORD *dw;
                } data = { pPropData };

                prim->eax_prop.dwFlags = *data.dw;
                prim->ExtAL->Effecti(prim->effect, AL_REVERB_DECAY_HFLIMIT,
                                     (prim->eax_prop.dwFlags&EAXLISTENERFLAGS_DECAYHFLIMIT) ?
                                     AL_TRUE : AL_FALSE);
                checkALError();

                prim->dirty.bit.effect = 1;
                hr = DS_OK;
            }
            break;

        default:
            hr = E_PROP_ID_UNSUPPORTED;
            FIXME("Unhandled propid: 0x%08lx\n", propid);
            break;
        }

        if(hr == DS_OK && immediate)
            DS8Primary3D_CommitDeferredSettings(&prim->IDirectSound3DListener_iface);

        popALContext();
        LeaveCriticalSection(prim->crst);
    }
    else
        FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return hr;
}

static HRESULT WINAPI DS8BufferProp_QuerySupport(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  ULONG *pTypeSupport)
{
    DS8Buffer *This = impl_from_IKsPropertySet(iface);
    HRESULT hr = E_PROP_ID_UNSUPPORTED;

    TRACE("(%p)->(%s, %lu, %p)\n", iface, debugstr_guid(guidPropSet), dwPropID, pTypeSupport);

    if(!pTypeSupport)
        return E_POINTER;
    *pTypeSupport = 0;

#if 0
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_BufferProperties))
    {
    }
    else
#endif
    if(IsEqualIID(guidPropSet, &DSPROPSETID_EAX20_ListenerProperties))
    {
        DS8Primary *prim = This->primary;

        EnterCriticalSection(This->crst);

        if(prim->effect == 0)
            hr = E_PROP_ID_UNSUPPORTED;
        else if(dwPropID == DSPROPERTY_EAXLISTENER_NONE)
        {
            *pTypeSupport = KSPROPERTY_SUPPORT_SET;
            hr = DS_OK;
        }
        else if(dwPropID == DSPROPERTY_EAXLISTENER_ALLPARAMETERS ||
                dwPropID == DSPROPERTY_EAXLISTENER_ROOM ||
                dwPropID == DSPROPERTY_EAXLISTENER_ROOMHF ||
                dwPropID == DSPROPERTY_EAXLISTENER_ROOMROLLOFFFACTOR ||
                dwPropID == DSPROPERTY_EAXLISTENER_ENVIRONMENT ||
                dwPropID == DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE ||
                dwPropID == DSPROPERTY_EAXLISTENER_ENVIRONMENTDIFFUSION ||
                dwPropID == DSPROPERTY_EAXLISTENER_AIRABSORPTIONHF ||
                dwPropID == DSPROPERTY_EAXLISTENER_FLAGS)
        {
            *pTypeSupport = KSPROPERTY_SUPPORT_GET|KSPROPERTY_SUPPORT_SET;
            hr = DS_OK;
        }
        else
            FIXME("Unhandled propid: 0x%08lx\n", dwPropID);

        LeaveCriticalSection(This->crst);
    }
    else
        FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return hr;
}

static const IKsPropertySetVtbl DS8BufferProp_Vtbl =
{
    DS8BufferProp_QueryInterface,
    DS8BufferProp_AddRef,
    DS8BufferProp_Release,
    DS8BufferProp_Get,
    DS8BufferProp_Set,
    DS8BufferProp_QuerySupport
};
