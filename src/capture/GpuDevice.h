#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <string>

namespace litecap {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel };

GpuVendor VendorFromId(UINT vendorId);
// Erhöht die GPU-Planungspriorität des Prozesses (einmalig; Fehler werden ignoriert).
void RaiseGpuSchedulingPriority();
const char* VendorName(GpuVendor v);

// D3D11-Device, das Capture, Farbkonvertierung und Hardware-Encoder gemeinsam nutzen.
// Gemeinsames Device = keine Kopie zwischen GPUs und kein Umweg über den RAM.
class GpuDevice {
 public:
  // preferred = Unknown → Adapter, an dem der Ziel-Monitor hängt (bzw. der leistungsstärkste).
  // Wirft winrt::hresult_error bei Fehlern.
  void Create(GpuVendor preferred, HMONITOR targetMonitor);

  ID3D11Device* Device() const { return device_.get(); }
  ID3D11DeviceContext* Context() const { return context_.get(); }
  ID3D11Multithread* Multithread() const { return multithread_.get(); }
  const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& WinRtDevice() const {
    return winrtDevice_;
  }
  GpuVendor Vendor() const { return vendor_; }
  const std::wstring& AdapterName() const { return adapterName_; }
  LUID AdapterLuid() const { return luid_; }

  // true, wenn das Gerät verloren ging (Treiber-Reset, GPU entfernt)
  bool IsLost() const;

 private:
  winrt::com_ptr<IDXGIAdapter1> PickAdapter(GpuVendor preferred, HMONITOR targetMonitor);

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11Multithread> multithread_;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice_{nullptr};
  GpuVendor vendor_ = GpuVendor::Unknown;
  std::wstring adapterName_;
  LUID luid_{};
};

// RAII-Sperre für den Immediate Context, der von mehreren Threads (Capture-Callback,
// Encoder, FFmpeg) benutzt wird.
class GpuLock {
 public:
  explicit GpuLock(ID3D11Multithread* mt) : mt_(mt) { mt_->Enter(); }
  ~GpuLock() { mt_->Leave(); }
  GpuLock(const GpuLock&) = delete;
  GpuLock& operator=(const GpuLock&) = delete;

 private:
  ID3D11Multithread* mt_;
};

} // namespace litecap
