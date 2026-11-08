#include "tools/Commands.h"

#include <windows.h>
#include <mmdeviceapi.h>
// initguid.h vor dem PKEY-Header: definiert PKEY_Device_FriendlyName in dieser Datei
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winrt/base.h>

#include <nlohmann/json.hpp>

#include "capture/CaptureTarget.h"
#include "capture/GpuDevice.h"
#include "config/Config.h"
#include "encode/EncoderInfo.h"
#include "encode/VideoEncoder.h"
#include "upload/GoogleAuth.h"
#include "upload/Http.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

using nlohmann::json;

namespace {

void WriteStdout(const std::string& s) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

json Microphones() {
  json list = json::array();
  try {
    auto en = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    winrt::com_ptr<IMMDeviceCollection> coll;
    winrt::check_hresult(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, coll.put()));
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      winrt::com_ptr<IMMDevice> dev;
      if (FAILED(coll->Item(i, dev.put()))) continue;
      LPWSTR id = nullptr;
      dev->GetId(&id);
      winrt::com_ptr<IPropertyStore> props;
      std::string name = "?";
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
          name = WideToUtf8(v.pwszVal);
        PropVariantClear(&v);
      }
      list.push_back({{"id", id ? WideToUtf8(id) : ""}, {"name", name}});
      CoTaskMemFree(id);
    }
  } catch (const winrt::hresult_error&) {
  }
  return list;
}

// Probiert jeden Hardware-Encoder kurz aus (öffnen mit 1280×720, sofort wieder schließen).
json Encoders() {
  json list = json::array();
  GpuVendor vendors[] = {GpuVendor::Nvidia, GpuVendor::Amd, GpuVendor::Intel};
  Backend backends[] = {Backend::NVENC, Backend::AMF, Backend::QSV};
  for (int b = 0; b < 3; ++b) {
    GpuDevice gpu;
    try {
      gpu.Create(vendors[b], nullptr);
    } catch (...) {
      continue;
    }
    if (gpu.Vendor() != vendors[b]) continue;  // keine GPU dieses Herstellers
    for (Codec c : {Codec::H264, Codec::HEVC, Codec::AV1}) {
      VideoEncoderSettings s;
      s.width = 1280;
      s.height = 720;
      s.fps = 60;
      VideoEncoder enc;
      bool ok = true;
      try {
        enc.Open(gpu, {{c, backends[b], false}}, s);
      } catch (...) {
        ok = false;
      }
      list.push_back({{"codec", CodecName(c)},
                      {"encoder", BackendName(backends[b])},
                      {"gpu", WideToUtf8(gpu.AdapterName())},
                      {"available", ok}});
    }
  }
  list.push_back({{"codec", "h264"}, {"encoder", "x264"}, {"gpu", "CPU"}, {"available", true}});
  return list;
}

} // namespace

int RunListDevices() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  json out;
  out["monitors"] = json::array();
  int idx = 0;
  for (const auto& m : EnumerateMonitors()) {
    out["monitors"].push_back({{"index", idx++},
                               {"name", WideToUtf8(m.deviceName)},
                               {"width", m.rect.right - m.rect.left},
                               {"height", m.rect.bottom - m.rect.top},
                               {"primary", m.primary}});
  }
  out["windows"] = json::array();
  for (const auto& w : EnumerateWindows())
    out["windows"].push_back({{"title", WideToUtf8(w.title)}, {"exe", WideToUtf8(w.exeName)}});
  out["microphones"] = Microphones();
  out["encoders"] = Encoders();
  out["youtubeConnected"] = GoogleAuth::IsConnected();
  WriteStdout(out.dump(2));
  return 0;
}

int RunYouTubeLogin() {
  try {
    Config cfg = Config::Load(paths::ConfigFile());
    Http http;
    GoogleAuth auth(cfg.youtube.clientId);
    auth.Login(http);
    WriteStdout("OK\n");
    return 0;
  } catch (const std::exception& e) {
    log::Error("YouTube-Anmeldung: {}", e.what());
    WriteStdout(std::string("FEHLER: ") + e.what() + "\n");
    return 1;
  }
}

int RunYouTubeLogout() {
  try {
    Http http;
    GoogleAuth::Logout(http);
    WriteStdout("OK\n");
    return 0;
  } catch (const std::exception& e) {
    WriteStdout(std::string("FEHLER: ") + e.what() + "\n");
    return 1;
  }
}

} // namespace litecap
