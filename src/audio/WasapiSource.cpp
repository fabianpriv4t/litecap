#include "audio/WasapiSource.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <vector>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 200'000;  // 20 ms in 100-ns-Einheiten
constexpr int64_t kTolerance = WasapiSource::kSampleRate / 500;  // 2 ms Zeitstempel-Jitter
constexpr int64_t kSilenceLag = WasapiSource::kSampleRate / 10;  // Stille erst nach 100 ms

// Meldet Wechsel des Standardgeräts (z. B. Bluetooth-Kopfhörer verbunden).
struct DeviceWatcher : winrt::implements<DeviceWatcher, IMMNotificationClient> {
  DeviceWatcher(EDataFlow flow, HANDLE event) : flow_(flow), event_(event) {}
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == flow_ && role == eConsole) SetEvent(event_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }
  EDataFlow flow_;
  HANDLE event_;
};

} // namespace

void WasapiSource::Start(Kind kind, const std::wstring& deviceId, int64_t originQpc,
                         DataFn onData) {
  Stop();
  kind_ = kind;
  deviceId_ = deviceId;
  originQpc_ = originQpc;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  qpcFreq_ = f.QuadPart;
  onData_ = std::move(onData);
  position_ = 0;
  loggedFailure_ = false;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  deviceChanged_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  thread_ = std::thread([this] { Run(); });
}

void WasapiSource::Stop() {
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (stopEvent_) CloseHandle(stopEvent_), stopEvent_ = nullptr;
  if (deviceChanged_) CloseHandle(deviceChanged_), deviceChanged_ = nullptr;
}

int64_t WasapiSource::NowSample() const {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (now.QuadPart - originQpc_) * kSampleRate / qpcFreq_;
}

void WasapiSource::PadSilenceUntilNow() {
  int64_t target = NowSample() - kSilenceLag;
  if (target > position_) {
    onData_(nullptr, static_cast<size_t>(target - position_));
    position_ = target;
  }
}

void WasapiSource::Deliver(const float* data, size_t frames, int64_t start) {
  if (start > position_ + kTolerance) {
    // Lücke (z. B. Ton setzte aus) → mit Stille füllen
    onData_(nullptr, static_cast<size_t>(start - position_));
    position_ = start;
  } else if (start < position_ - kTolerance) {
    // Überlappt bereits Geliefertes (nach Stillefüllung) → Anfang abschneiden
    int64_t skip = std::min<int64_t>(position_ - start, static_cast<int64_t>(frames));
    frames -= static_cast<size_t>(skip);
    if (data) data += skip * kChannels;
  }
  if (frames == 0) return;
  onData_(data, frames);
  position_ += static_cast<int64_t>(frames);
}

void WasapiSource::Run() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  winrt::com_ptr<DeviceWatcher> watcher;
  try {
    enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    if (deviceId_.empty()) {
      watcher = winrt::make_self<DeviceWatcher>(kind_ == Kind::Loopback ? eRender : eCapture,
                                                deviceChanged_);
      enumerator->RegisterEndpointNotificationCallback(watcher.get());
    }
  } catch (const winrt::hresult_error& e) {
    log::Error("{}: Audio-Geräte nicht verfügbar: {}", Name(), WideToUtf8(e.message()));
  }

  for (;;) {
    bool stop = false;
    try {
      if (enumerator) stop = CaptureLoop();
    } catch (const winrt::hresult_error& e) {
      if (!loggedFailure_) {
        log::Warn("{}: {} – neuer Versuch alle 2 s, bis dahin Stille", Name(),
                  WideToUtf8(e.message()));
        loggedFailure_ = true;
      }
    }
    if (stop) break;
    // Gerät weg/gewechselt: kurz warten, währenddessen Stille liefern (Spur bleibt synchron)
    bool stopRequested = false;
    for (int i = 0; i < 40 && !stopRequested; ++i) {
      stopRequested = WaitForSingleObject(stopEvent_, 50) == WAIT_OBJECT_0;
      PadSilenceUntilNow();
    }
    if (stopRequested) break;
  }

  if (watcher) enumerator->UnregisterEndpointNotificationCallback(watcher.get());
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// Liefert true bei regulärem Stopp, false wenn neu initialisiert werden soll.
bool WasapiSource::CaptureLoop() {
  auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
  winrt::com_ptr<IMMDevice> device;
  if (!deviceId_.empty()) {
    winrt::check_hresult(enumerator->GetDevice(deviceId_.c_str(), device.put()));
  } else {
    winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(
        kind_ == Kind::Loopback ? eRender : eCapture, eConsole, device.put()));
  }

  winrt::com_ptr<IAudioClient> client;
  winrt::check_hresult(
      device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));

  WAVEFORMATEX wf = {};
  wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  wf.nChannels = kChannels;
  wf.nSamplesPerSec = kSampleRate;
  wf.wBitsPerSample = 32;
  wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

  DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (kind_ == Kind::Loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  winrt::check_hresult(
      client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &wf, nullptr));

  HANDLE dataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  winrt::check_hresult(client->SetEventHandle(dataEvent));
  winrt::com_ptr<IAudioCaptureClient> capture;
  winrt::check_hresult(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void()));
  winrt::check_hresult(client->Start());

  log::Info("{} aktiv: {}", Name(), [&] {
    LPWSTR id = nullptr;
    device->GetId(&id);
    std::string s = id ? WideToUtf8(id) : "?";
    CoTaskMemFree(id);
    return s;
  }());
  loggedFailure_ = false;

  const int64_t origin100ns = originQpc_ * 10'000'000 / qpcFreq_;
  HANDLE waits[3] = {stopEvent_, deviceChanged_, dataEvent};
  bool result = false;
  for (;;) {
    // 50 ms Timeout: Loopback liefert bei Stille keine Pakete → dann selbst Stille einfügen
    DWORD w = WaitForMultipleObjects(3, waits, FALSE, 50);
    if (w == WAIT_OBJECT_0) {
      result = true;
      break;
    }
    if (w == WAIT_OBJECT_0 + 1) {
      log::Info("{}: Standardgerät gewechselt", Name());
      break;
    }

    HRESULT hr = S_OK;
    UINT32 packet = 0;
    while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD bufFlags = 0;
      UINT64 qpc100ns = 0;
      hr = capture->GetBuffer(&data, &frames, &bufFlags, nullptr, &qpc100ns);
      if (FAILED(hr)) break;
      int64_t start = (bufFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                          ? position_
                          : (static_cast<int64_t>(qpc100ns) - origin100ns) * kSampleRate / 10'000'000;
      const bool silent = (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
      if (start + static_cast<int64_t>(frames) > 0)  // Daten vor Session-Beginn ignorieren
        Deliver(silent ? nullptr : reinterpret_cast<const float*>(data), frames, start);
      capture->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) {
      // AUDCLNT_E_DEVICE_INVALIDATED: Gerät entfernt o. Ä.
      log::Warn("{}: Gerät verloren ({})", Name(), HResultMessage(hr));
      break;
    }
    PadSilenceUntilNow();
  }
  client->Stop();
  CloseHandle(dataEvent);
  return result;
}

} // namespace litecap
