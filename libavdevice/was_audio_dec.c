#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>

#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"

typedef struct WASData {
    AVClass *class;
    int  sample_rate;
    int  channels;
    int  frame_size;
    int  block_size;
    int loopback;
    int record_start;

    IMMDevice *device;
    IAudioClient *audio_client;
    IAudioCaptureClient *capture_client;
    IMMDeviceEnumerator* enumerator;
    IMMDeviceCollection* collection;

    EDataFlow dir;
    ERole role;
    HANDLE capture_samples_ready_event;
} WASData;

#define GOTO_FAIL_IF_ERROR(hr, function) \
  if (FAILED(hr)) { \
    av_log(s, AV_LOG_ERROR, #function " failed, hr = 0x%08x\n", hr); \
    ret = AVERROR_EXTERNAL; \
    goto fail; \
  } \

#define WAS_DEFAULT_CODEC_ID AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE)

DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient, 0xC8ADBD64, 0xE71E, 0x48a0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

static int16_t channels_priority_list[3] = {2, 1, 4};

static int refresh_was_device(AVFormatContext *s, 
                              IMMDeviceEnumerator **out_enumerator, 
                              IMMDeviceCollection **out_collection) {
    HRESULT hr = S_OK;
    WASData *pd = s->priv_data;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDeviceCollection* collection = NULL;
    int ret = 0;
    pd->dir = pd->loopback ? eRender : eCapture;
    pd->role = pd->loopback ? eConsole : eCommunications;

    // get an enumerator
    hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator, 
        NULL, CLSCTX_ALL, 
        &IID_IMMDeviceEnumerator,
        (void**)&enumerator
    );
    GOTO_FAIL_IF_ERROR(hr, "CoCreateInstance");

    // get all the active render endpoints
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(
        enumerator, 
        pd->dir, 
        DEVICE_STATE_ACTIVE, 
        &collection);
    GOTO_FAIL_IF_ERROR(hr, "IMMDeviceEnumerator_EnumAudioEndpoints");

    *out_enumerator = enumerator;
    *out_collection = collection;
    return 0;

fail:
  if (enumerator) {
    IMMDeviceEnumerator_Release(enumerator);
  }
  if (collection) {
    IMMDeviceCollection_Release(collection);
  }
  return ret;
}

static int32_t get_device_name(AVFormatContext *s, IMMDevice* device, LPWSTR buffer, int buffer_len) {
  static const WCHAR default_device_name[] = L"<Device not available>";
  HRESULT hr = E_FAIL;
  IPropertyStore* props = NULL;
  PROPVARIANT var_name;

  if (device != NULL) {
      hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &props);
      if (FAILED(hr)) {
          av_log(s, AV_LOG_ERROR, "IMMDevice_OpenPropertyStore failed: hr = 0x%08x\n", hr);
      }
  }

  // Initialize container for property value.
  PropVariantInit(&var_name);

  if (SUCCEEDED(hr)) {
    // Get the endpoint device's friendly-name property.
    hr = IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &var_name);
    if (FAILED(hr)) {
       av_log(s, AV_LOG_ERROR, "IPropertyStore_GetValue failed: hr = 0x%08x\n", hr);
    }
  }

  if ((SUCCEEDED(hr)) && (VT_EMPTY == var_name.vt)) {
    hr = E_FAIL;
    av_log(s, AV_LOG_ERROR, "IPropertyStore_GetValue returned no value: hr = 0x%08x\n", hr);
  }

  if ((SUCCEEDED(hr)) && (VT_LPWSTR != var_name.vt)) {
    // The returned value is not a wide null terminated string.
    hr = E_UNEXPECTED;
    av_log(s, AV_LOG_ERROR, "IPropertyStore::GetValue returned unexpected type, hr = 0x%08x\n", hr);
  }

  if (SUCCEEDED(hr) && (var_name.pwszVal != NULL)) {
    // Copy the valid device name to the provided ouput buffer.
    wcsncpy_s(buffer, buffer_len, var_name.pwszVal, _TRUNCATE);
  } else {
    // Failed to find the device name.
    wcsncpy_s(buffer, buffer_len, default_device_name, _TRUNCATE);
  }

  PropVariantClear(&var_name);
  IPropertyStore_Release(props);
  props = NULL;
  return 0;
}

static int32_t get_device_id(IMMDevice* device, LPWSTR buffer, int buffer_len) {
  static const WCHAR default_id[] = L"<Device not available>";
  HRESULT hr = E_FAIL;
  LPWSTR id = NULL;

  if (device != NULL) {
    hr = IMMDevice_GetId(device, &id);
  }

  if (hr == S_OK) {
    // Found the device ID.
    wcsncpy_s(buffer, buffer_len, id, _TRUNCATE);
  } else {
    // Failed to find the device ID.
    wcsncpy_s(buffer, buffer_len, default_id, _TRUNCATE);
  }

  CoTaskMemFree(id);
  return 0;
}

static void free_device_info(AVDeviceInfo *device) {
    av_freep(&device->device_description);
    av_freep(&device->device_name);
    av_free(device);
}

static AVDeviceInfo *new_and_add_device_info(AVDeviceInfoList *device_list) {
    AVDeviceInfo *new_device = NULL;
    int ret = 0;
    new_device = av_mallocz(sizeof(AVDeviceInfo));
    if (!new_device) {
        goto fail; 
    }

    new_device->device_description = av_realloc(NULL, MAX_PATH);
    new_device->device_name = av_realloc(NULL, MAX_PATH);
    if (!new_device->device_description || !new_device->device_name) {
        goto fail;
    }
    memset(new_device->device_description, 0, MAX_PATH);
    memset(new_device->device_name, 0, MAX_PATH);

    if ((ret = av_dynarray_add_nofree(&device_list->devices,
                                      &device_list->nb_devices, new_device)) < 0) {
        goto fail;
    }
    return new_device;

fail:
  free_device_info(new_device);
  return NULL;
}

static int was_get_device_list(AVFormatContext *s, AVDeviceInfoList *device_list)
{
    WASData *pd = s->priv_data;
    HRESULT hr = S_OK;
    IMMDevice *pMMDevice = NULL;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDeviceCollection* collection = NULL;

    int ret = 0;
    UINT count = 0;
    WCHAR szDeviceName[MAX_PATH] = {0};
    WCHAR szDefaultDeviceID[MAX_PATH] = {0};
    const int bufferLen = sizeof(szDeviceName) / sizeof(szDeviceName)[0];

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    device_list->nb_devices = 0;
    device_list->devices = NULL;
    device_list->default_device = -1;

    ret = refresh_was_device(s, &enumerator, &collection);
    if (ret != 0)
      return ret;
  
    hr = IMMDeviceCollection_GetCount(collection, &count);
    GOTO_FAIL_IF_ERROR(hr, "IMMDeviceCollection_GetCount");

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, pd->dir, pd->role, &pMMDevice);
    GOTO_FAIL_IF_ERROR(hr, "IMMDeviceCollection_GetCount");
    get_device_id(pMMDevice, szDefaultDeviceID, bufferLen);
    if (pMMDevice) {
        IMMDevice_Release(pMMDevice);
        pMMDevice = NULL;
    }
  
    for (UINT i = 0; i < count; i++) {
        // get the "n"th device
        hr = IMMDeviceCollection_Item(collection, i, &pMMDevice);
        GOTO_FAIL_IF_ERROR(hr, "IMMDeviceCollection_Item");

        AVDeviceInfo* new_device = new_and_add_device_info(device_list);
        if (new_device == NULL) {
          ret = AVERROR(ENOMEM);
          goto fail;
        }

        ret = get_device_name(s, pMMDevice, szDeviceName, bufferLen);
        if (ret == 0) {
            // Convert the endpoint device's friendly-name to UTF-8
            if (WideCharToMultiByte(CP_UTF8, 0, szDeviceName, -1, new_device->device_description,
                                    MAX_PATH, NULL, NULL) == 0) {
                av_log(s, "WideCharToMultiByte(CP_UTF8) failed with error code %u\n", GetLastError());
            }
        }

        ret = get_device_id(pMMDevice, szDeviceName, bufferLen);
        if (ret == 0) {
            // check if default device.
            if (wcsncmp(szDefaultDeviceID, szDeviceName, bufferLen) == 0) {
                // Found a match.
                device_list->default_device = i;
            }
            // Convert the endpoint device's friendly-name to UTF-8
            if (WideCharToMultiByte(CP_UTF8, 0, szDeviceName, -1, new_device->device_name,
                                    MAX_PATH, NULL, NULL) == 0) {
                av_log(s, "WideCharToMultiByte(CP_UTF8) failed with error code %u\n", GetLastError());
            }
        }
  
        if (pMMDevice) {
            IMMDevice_Release(pMMDevice);
            pMMDevice = NULL;
        }
    }

fail:
    if (enumerator) {
      IMMDeviceEnumerator_Release(enumerator);
    }
    if (collection) {
      IMMDeviceCollection_Release(collection);
    }
    if (pMMDevice) {
        IMMDevice_Release(pMMDevice);
    }
    return ret;
}

static IMMDevice *get_device_by_id(AVFormatContext *s, IMMDeviceCollection* collection, const char* id) {
    HRESULT hr = S_OK;
    IMMDevice *pMMDevice = NULL;

    int ret = 0;
    UINT count = 0;
    UINT i = 0;
    WCHAR szDeviceName[MAX_PATH];
    char szDeviceNameUtf8[MAX_PATH];
    const int bufferLen = sizeof(szDeviceName) / sizeof(szDeviceName)[0];

    hr = IMMDeviceCollection_GetCount(collection, &count);
    GOTO_FAIL_IF_ERROR(hr, "IMMDeviceCollection_GetCount");
  
    for (i = 0; i < count; i++) {
        // get the "n"th device
        hr = IMMDeviceCollection_Item(collection, i, &pMMDevice);
        GOTO_FAIL_IF_ERROR(hr, "IMMDeviceCollection_Item");

        ret = get_device_id(pMMDevice, szDeviceName, bufferLen);
        if (ret == 0) {
            // Convert the endpoint device's friendly-name to UTF-8
            if (WideCharToMultiByte(CP_UTF8, 0, szDeviceName, -1, szDeviceNameUtf8,
                                    MAX_PATH, NULL, NULL) == 0) {
                av_log(s, AV_LOG_ERROR, "WideCharToMultiByte(CP_UTF8) failed with error code %d\n", GetLastError());
            }
            // found it
            av_log(s, AV_LOG_ERROR, "%s\n", szDeviceNameUtf8);
            if (strncmp(szDeviceNameUtf8, id, MAX_PATH) == 0) {
                return pMMDevice;
            }
        }
  
        if (pMMDevice) {
            IMMDevice_Release(pMMDevice);
            pMMDevice = NULL;
        }
    }

fail:
    if (pMMDevice) {
        IMMDevice_Release(pMMDevice);
    }
    return NULL;
}

static av_cold int was_read_header(AVFormatContext *s)
{
  HRESULT hr = S_OK;
  WASData *pd = s->priv_data;
  WAVEFORMATEX* input_waveformatex = NULL;
  WAVEFORMATEXTENSIBLE waveformat_extensible;
  WAVEFORMATEX* waveformatex_closest_match = NULL;
  IMMDevice *device = NULL;
  IAudioClient *audio_client = NULL;
  IAudioCaptureClient *capture_client = NULL;
  IMMDeviceEnumerator* enumerator = NULL;
  IMMDeviceCollection* collection = NULL;
  HANDLE capture_samples_ready_event = NULL;
  char *device_id = NULL;
  int ret = 0;
  UINT buffer_frame_count = 0;
  AVStream *st = NULL;
  enum AVCodecID codec_id =
    s->audio_codec_id == AV_CODEC_ID_NONE ? WAS_DEFAULT_CODEC_ID : s->audio_codec_id;

  st = avformat_new_stream(s, NULL);
  if (!st) {
      av_log(s, AV_LOG_ERROR, "Cannot add stream\n");
      return AVERROR(ENOMEM);
  }

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

  capture_samples_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);

  ret = refresh_was_device(s, &enumerator, &collection);
  if (ret != 0)
    return ret;

  device_id = s->filename;

  device = get_device_by_id(s, collection, device_id);
  if (device == NULL) {
    av_log(s, AV_LOG_ERROR, "Can't find device %s\n", device_id);
    ret = AVERROR_EXTERNAL;
    goto fail;
  }

  hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                              (void**)&audio_client);
  GOTO_FAIL_IF_ERROR(hr, "IMMDevice_Activate");

  // Retrieve the stream format that the audio engine uses for its internal
  // processing (mixing) of shared-mode streams.
  hr = IAudioClient_GetMixFormat(audio_client, &input_waveformatex);
  if (SUCCEEDED(hr)) {
    av_log(s, AV_LOG_DEBUG, "Audio Engine's current capturing mix format\n");
    // format type
    av_log(s, AV_LOG_DEBUG, "wFormatTag     : 0x%08x (%d)\n", 
                            input_waveformatex->wFormatTag, 
                            input_waveformatex->wFormatTag);
    // number of channels (i.e. mono, stereo...)
    av_log(s, AV_LOG_DEBUG, "nChannels      : %d\n", input_waveformatex->nChannels);
    // sample rate
    av_log(s, AV_LOG_DEBUG, "nSamplesPerSec : %d\n", input_waveformatex->nSamplesPerSec);
    // for buffer estimation
    av_log(s, AV_LOG_DEBUG, "nAvgBytesPerSec: %d\n", input_waveformatex->nAvgBytesPerSec);
    // block size of data
    av_log(s, AV_LOG_DEBUG, "nBlockAlign    : %d\n", input_waveformatex->nBlockAlign);
    // number of bits per sample of mono data
    av_log(s, AV_LOG_DEBUG, "wBitsPerSample : %d\n", input_waveformatex->wBitsPerSample);
    av_log(s, AV_LOG_DEBUG, "cbSize         : %d\n", input_waveformatex->cbSize);
  }

  // Set wave format
  waveformat_extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  waveformat_extensible.Format.wBitsPerSample = 16;
  waveformat_extensible.Format.cbSize = 22;
  waveformat_extensible.dwChannelMask = 0;
  waveformat_extensible.Samples.wValidBitsPerSample = waveformat_extensible.Format.wBitsPerSample;
  waveformat_extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

  const int freqs[6] = {48000, 44100, 16000, 96000, 32000, 8000};
  hr = S_FALSE;

  // Iterate over frequencies and channels, in order of priority
  for (unsigned int freq = 0; freq < sizeof(freqs) / sizeof(freqs[0]); freq++) {
    for (unsigned int chan = 0;
         chan < sizeof(channels_priority_list) / sizeof(channels_priority_list[0]);
         chan++) {
      waveformat_extensible.Format.nChannels = channels_priority_list[chan];
      waveformat_extensible.Format.nSamplesPerSec = freqs[freq];
      waveformat_extensible.Format.nBlockAlign =
          waveformat_extensible.Format.nChannels * waveformat_extensible.Format.wBitsPerSample / 8;
      waveformat_extensible.Format.nAvgBytesPerSec =
          waveformat_extensible.Format.nSamplesPerSec * waveformat_extensible.Format.nBlockAlign;
      // If the method succeeds and the audio endpoint device supports the
      // specified stream format, it returns S_OK. If the method succeeds and
      // provides a closest match to the specified format, it returns S_FALSE.
      hr = IAudioClient_IsFormatSupported(audio_client, 
                                          AUDCLNT_SHAREMODE_SHARED, 
                                          (WAVEFORMATEX*)&waveformat_extensible, 
                                          &waveformatex_closest_match);
      if (hr == S_OK) {
        break;
      } else {
        if (waveformatex_closest_match) {
          CoTaskMemFree(waveformatex_closest_match);
          waveformatex_closest_match = NULL;
        }
      }
    }
    if (hr == S_OK)
      break;
  }
  if (FAILED(hr)) {
    av_log(s, AV_LOG_ERROR, "Can't find property frequency and channels.\n");
    goto fail;
  }

  pd->frame_size = waveformat_extensible.Format.nBlockAlign;
  pd->sample_rate = waveformat_extensible.Format.nSamplesPerSec;
  pd->block_size = waveformat_extensible.Format.nSamplesPerSec / 100;
  pd->channels = waveformat_extensible.Format.nChannels;

  av_log(s, AV_LOG_DEBUG, "VoE selected this capturing format:\n");
  av_log(s, AV_LOG_DEBUG, "wFormatTag        : 0x%08x (%d)\n", 
          waveformat_extensible.Format.wFormatTag, 
          waveformat_extensible.Format.wFormatTag);
  av_log(s, AV_LOG_DEBUG, "nChannels         : %d\n", waveformat_extensible.Format.nChannels);
  av_log(s, AV_LOG_DEBUG, "nSamplesPerSec    : %d\n", waveformat_extensible.Format.nSamplesPerSec);
  av_log(s, AV_LOG_DEBUG, "nAvgBytesPerSec   : %d\n", waveformat_extensible.Format.nAvgBytesPerSec);
  av_log(s, AV_LOG_DEBUG, "nBlockAlign       : %d\n", waveformat_extensible.Format.nBlockAlign);
  av_log(s, AV_LOG_DEBUG, "wBitsPerSample    : %d\n", waveformat_extensible.Format.wBitsPerSample);
  av_log(s, AV_LOG_DEBUG, "cbSize            : %d\n", waveformat_extensible.Format.cbSize);
  av_log(s, AV_LOG_DEBUG, "Additional settings:\n");
  av_log(s, AV_LOG_DEBUG, "_recAudioFrameSize: %d\n", pd->frame_size);
  av_log(s, AV_LOG_DEBUG, "_recBlockSize     : %d\n", pd->block_size);
  av_log(s, AV_LOG_DEBUG, "_recChannels      : %d\n", pd->channels);

  DWORD flags =
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK |  // processing of the audio buffer by
                                           // the client will be event driven
          AUDCLNT_STREAMFLAGS_NOPERSIST;   // volume and mute settings for an
                                           // audio session will not persist
                                           // across system restarts
  if (pd->loopback) {
    flags |= AUDCLNT_STREAMFLAGS_LOOPBACK; // enables audio loopback
  }

  // Create a capturing stream.
  hr = IAudioClient_Initialize(
      audio_client,
      AUDCLNT_SHAREMODE_SHARED,  // share Audio Engine with other applications
      flags,
      0,                    // required for event-driven shared mode
      0,                    // periodicity
      (WAVEFORMATEX*)&waveformat_extensible,  // selected wave format
      NULL);                // session GUID

  GOTO_FAIL_IF_ERROR(hr, "IAudioClient_Initialize");

  hr = IAudioClient_GetBufferSize(audio_client, &buffer_frame_count);
  if (SUCCEEDED(hr)) {
    av_log(s, AV_LOG_DEBUG, "Buffer size => %u (<=> %u bytes)",   
        buffer_frame_count, 
        buffer_frame_count * pd->frame_size);
  }


  // Set the event handle that the system signals when an audio buffer is ready
  // to be processed by the client.
  hr = IAudioClient_SetEventHandle(audio_client, capture_samples_ready_event);
  GOTO_FAIL_IF_ERROR(hr, "IAudioClient_SetEventHandle");

  // Get an IAudioCaptureClient interface.
  hr = IAudioClient_GetService(audio_client, &IID_IAudioCaptureClient,
                                (void**)&capture_client);
  GOTO_FAIL_IF_ERROR(hr, "IAudioClient_GetService");

  CoTaskMemFree(input_waveformatex);
  CoTaskMemFree(waveformatex_closest_match);

  pd->device = device;
  pd->audio_client = audio_client;
  pd->capture_client = capture_client;
  pd->enumerator = enumerator;
  pd->collection = collection;
  pd->dir = pd->loopback ? eRender : eCapture;
  pd->role = pd->loopback ? eConsole : eCommunications;
  pd->capture_samples_ready_event = capture_samples_ready_event;
  pd->record_start = 0;

  /* take real parameters */
  st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
  st->codecpar->codec_id    = codec_id;
  st->codecpar->sample_rate = pd->sample_rate;
  st->codecpar->channels    = pd->channels;

  return 0;

fail:
  if (device) {
      IMMDevice_Release(device);
  }
  if (enumerator) {
      IMMDeviceEnumerator_Release(enumerator);
  }
  if (collection) {
      IMMDeviceCollection_Release(collection);
  }
  if (audio_client) {
      IAudioClient_Release(audio_client);
  }
  if (capture_client) {
      IAudioCaptureClient_Release(capture_client);
  }
  if (capture_samples_ready_event) {
      CloseHandle(capture_samples_ready_event);
  }
  if (input_waveformatex) {
      CoTaskMemFree(input_waveformatex);
  }
  if (waveformatex_closest_match) {
      CoTaskMemFree(waveformatex_closest_match);
  }
  return ret;
}

static int was_read_packet(AVFormatContext *s, AVPacket *pkt) 
{
  WASData *pd  = s->priv_data;
  int ret = 0;
  HRESULT hr = S_OK;
  HANDLE wait_array[1] = {pd->capture_samples_ready_event};
  int record_start = pd->record_start;
  IAudioClient *audio_client = pd->audio_client;
  IAudioCaptureClient *capture_client = pd->capture_client;
  int dw_milliseconds = 500;
  BYTE* data = 0;
  UINT32 frames_available = 0;
  DWORD flags = 0;
  UINT64 record_time = 0;
  UINT64 record_position = 0;
  DWORD wait_result = 0;
  size_t read_length;

  if (!record_start) {
      hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
      if (FAILED(hr)) {
        av_log(s, AV_LOG_ERROR, "CoInitializeEx failed, hr = 0x%08x", hr); \
        return AVERROR_EXTERNAL;
      }
      hr = IAudioClient_Start(audio_client);
      GOTO_FAIL_IF_ERROR(hr, "IAudioClient_Start");
      record_start = 1;
      pd->record_start = record_start;
  }

  while(record_start) {
      // get audio data
      wait_result = WaitForMultipleObjects(1, wait_array, FALSE, dw_milliseconds);
      switch (wait_result) {
        case WAIT_OBJECT_0 + 0:  // capture_samples_ready_event
          break;
        case WAIT_TIMEOUT:  // timeout notification
          av_log(s, AV_LOG_ERROR, "capture event timed out after %d milliseconds\n", dw_milliseconds);
          ret = AVERROR_EXTERNAL;
          goto fail;
        default:  // unexpected error
          av_log(s, AV_LOG_ERROR, "unknown wait termination on capture side\n");
          ret = AVERROR_EXTERNAL;
          goto fail;
      } //wait_result

      //  Find out how much capture data is available
      //
      hr = IAudioCaptureClient_GetBuffer(
          capture_client,
          &data,            // packet which is ready to be read by used
          &frames_available,  // #frames in the captured packet (can be zero)
          &flags,            // support flags (check)
          &record_position,    // device position of first audio frame in data packet
          &record_time);  // value of performance counter at the time of recording the first audio frame
      GOTO_FAIL_IF_ERROR(hr, "IAudioCaptureClient_GetBuffer");

      if (AUDCLNT_S_BUFFER_EMPTY == hr) {
        // Buffer was empty => start waiting for a new capture notification
        // event
        continue;
      }

      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        // Treat all of the data in the packet as silence and ignore the
        // actual data values.
        av_log(s, AV_LOG_WARNING, "AUDCLNT_BUFFERFLAGS_SILENT\n");
        data = NULL;
      }

      read_length = frames_available * pd->frame_size;
      if (av_new_packet(pkt, read_length) < 0) {
          ret = AVERROR(ENOMEM);
          goto fail;
      }

      if (data) {
        CopyMemory(pkt->data, data, read_length);
      } else {
        ZeroMemory(pkt->data, read_length);
      }

      // Release the capture buffer
      //
      hr = IAudioCaptureClient_ReleaseBuffer(capture_client, frames_available);
      if (FAILED(hr)) {
        av_log(s, AV_LOG_WARNING, "IAudioCaptureClient_ReleaseBuffer failed, hr = 0x%08x\n", hr);
      }
      break;
  }
  return 0;

fail:
  pd->record_start = 0;
  if (audio_client) {
    IAudioClient_Stop(audio_client);
    IAudioClient_Reset(audio_client);
  }

  return ret;
}

static av_cold int was_close(AVFormatContext *s)
{
  WASData *pd  = s->priv_data;

  pd->record_start = 0;
  if (pd->audio_client) {
    IAudioClient_Stop(pd->audio_client);
    IAudioClient_Reset(pd->audio_client);
  }

  if (pd->device) {
      IMMDevice_Release(pd->device);
      pd->device = NULL;
  }
  if (pd->enumerator) {
      IMMDeviceEnumerator_Release(pd->enumerator);
      pd->enumerator = NULL;
  }
  if (pd->collection) {
      IMMDeviceCollection_Release(pd->collection);
      pd->collection = NULL;
  }
  if (pd->audio_client) {
      IAudioClient_Release(pd->audio_client);
      pd->audio_client = NULL;
  }
  if (pd->capture_client) {
      IAudioCaptureClient_Release(pd->capture_client);
      pd->capture_client = NULL;
  }
  if (pd->capture_samples_ready_event) {
      CloseHandle(pd->capture_samples_ready_event);
      pd->capture_samples_ready_event = NULL;
  }
  return 0;
}

#define OFFSET(a) offsetof(WASData, a)
#define D AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "loopback", "use loopback device or not",  OFFSET(loopback), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, D },
    { NULL },
};

static const AVClass was_demuxer_class = {
    .class_name     = "WAS demuxer",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_was_demuxer = {
    .name           = "WAS",
    .long_name      = NULL_IF_CONFIG_SMALL("WAS audio input"),
    .priv_data_size = sizeof(WASData),
    .read_header    = was_read_header,
    .read_packet    = was_read_packet,
    .read_close     = was_close,
    .get_device_list = was_get_device_list,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &was_demuxer_class,
};