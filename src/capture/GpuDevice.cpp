#include "capture/GpuDevice.h"

#include <windows.graphics.directx.direct3d11.interop.h>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

GpuVendor VendorFromId(UINT id) {
  switch (id) {
    case 0x10DE: return GpuVendor::Nvidia;
    case 0x1002:
    case 0x1022: return GpuVendor::Amd;
    case 0x8086: return GpuVendor::Intel;
    default: return GpuVendor::Unknown;
  }
}

const char* VendorName(GpuVendor v) {
  switch (v) {
    case GpuVendor::Nvidia: return "NVIDIA";
    case GpuVendor::Amd: return "AMD";
    case GpuVendor::Intel: return "Intel";
    default: return "unbekannt";
  }
}

// GPU-Planungspriorität des Prozesses (wie OBS). Ohne sie stehen unsere wenigen GPU-Befehle
// bei GPU-limitierten Spielen hinter mehreren Spiel-Frames an. "Realtime" benötigt
// Administratorrechte, "High" meist nicht.
void RaiseGpuSchedulingPriority() {
  using SetPrioFn = LONG(APIENTRY*)(HANDLE, int);
  static bool done = false;
  if (done) return;
  done = true;
  auto fn = reinterpret_cast<SetPrioFn>(
      GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
  if (!fn) return;
  constexpr int kHigh = 4, kRealtime = 5;  // D3DKMT_SCHEDULINGPRIORITYCLASS_*
  if (fn(GetCurrentProcess(), kRealtime) == 0) log::Info("GPU-Planungspriorität: Realtime");
  else if (fn(GetCurrentProcess(), kHigh) == 0) log::Info("GPU-Planungspriorität: High");
  else log::Info("GPU-Planungspriorität: Standard (nicht änderbar)");
}

winrt::com_ptr<IDXGIAdapter1> GpuDevice::PickAdapter(GpuVendor preferred, HMONITOR monitor) {
  winrt::com_ptr<IDXGIFactory6> factory;
  winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));

  winrt::com_ptr<IDXGIAdapter1> byVendor, byMonitor, first;
  winrt::com_ptr<IDXGIAdapter1> adapter;
  // Reihenfolge "High Performance": diskrete GPU vor iGPU
  for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(adapter.put())) == S_OK;
       ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      adapter = nullptr;
      continue;
    }
    if (!first) first = adapter;
    if (!byVendor && preferred != GpuVendor::Unknown && VendorFromId(desc.VendorId) == preferred)
      byVendor = adapter;
    if (!byMonitor && monitor) {
      winrt::com_ptr<IDXGIOutput> out;
      for (UINT o = 0; adapter->EnumOutputs(o, out.put()) == S_OK; ++o) {
        DXGI_OUTPUT_DESC od;
        out->GetDesc(&od);
        out = nullptr;
        if (od.Monitor == monitor) {
          byMonitor = adapter;
          break;
        }
      }
    }
    adapter = nullptr;
  }
  if (byVendor) return byVendor;
  if (byMonitor) return byMonitor;
  if (first) return first;
  throw winrt::hresult_error(DXGI_ERROR_NOT_FOUND, L"Keine Hardware-GPU gefunden");
}

void GpuDevice::Create(GpuVendor preferred, HMONITOR targetMonitor) {
  winrtDevice_ = nullptr;
  multithread_ = nullptr;
  context_ = nullptr;
  device_ = nullptr;
  auto adapter = PickAdapter(preferred, targetMonitor);
  DXGI_ADAPTER_DESC1 desc;
  adapter->GetDesc1(&desc);
  vendor_ = VendorFromId(desc.VendorId);
  adapterName_ = desc.Description;
  luid_ = desc.AdapterLuid;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                 device_.put(), nullptr, context_.put());
  if (FAILED(hr)) {
    // Ältere Treiber ohne Video-Support im Device → ohne Flag erneut (dann nur x264 möglich).
    log::Warn("D3D11CreateDevice mit VIDEO_SUPPORT fehlgeschlagen ({}), versuche ohne",
              HResultMessage(hr));
    flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           flags, levels, static_cast<UINT>(std::size(levels)),
                                           D3D11_SDK_VERSION, device_.put(), nullptr,
                                           context_.put()));
  }

  multithread_ = context_.as<ID3D11Multithread>();
  multithread_->SetMultithreadProtected(TRUE);

  // Unsere GPU-Arbeit (eine Kopie + Farbkonvertierung pro Frame) ist winzig, darf aber nicht
  // hinter einem GPU-limitierten Spiel warten, sonst fallen Frames aus. Positive Werte
  // erfordern Administratorrechte; ohne sie bleibt die Standardpriorität.
  auto dxgiDevice = device_.as<IDXGIDevice>();
  HRESULT prio = dxgiDevice->SetGPUThreadPriority(7);
  log::Info("GPU-Thread-Priorität: {}", SUCCEEDED(prio) ? "erhöht" : "Standard");
  RaiseGpuSchedulingPriority();
  winrt::com_ptr<::IInspectable> inspectable;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
  winrtDevice_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

  log::Info("GPU: {} ({})", WideToUtf8(adapterName_), VendorName(vendor_));
}

bool GpuDevice::IsLost() const {
  return device_ && device_->GetDeviceRemovedReason() != S_OK;
}

} // namespace litecap
