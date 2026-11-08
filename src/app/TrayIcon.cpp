#include "app/TrayIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "util/Log.h"

namespace litecap {

namespace {

// Bewusst uID statt GUID: Eine GUID bindet Windows an den exe-Pfad, was beim Verschieben
// der exe (oder Debug-/Release-Builds nebeneinander) das Anlegen des Icons verhindert.
constexpr UINT kTrayId = 1;

struct Rgb { uint8_t r, g, b; };

// Zeichnet ein kantengeglättetes "Linsen"-Icon: farbiger Ring + Punkt in der Mitte.
HICON MakeIcon(int size, Rgb ring, Rgb dot) {
  BITMAPV5HEADER bi = {};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;  // top-down
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color) return nullptr;

  auto* px = static_cast<uint32_t*>(bits);
  const float c = (size - 1) / 2.0f;
  const float rOuter = size * 0.47f, rInner = size * 0.32f, rDot = size * 0.20f;
  auto cover = [](float d, float r) { return std::clamp(r - d + 0.5f, 0.0f, 1.0f); };

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      float d = std::hypot(x - c, y - c);
      float aRing = cover(d, rOuter) * (1.0f - cover(d, rInner));
      float aDot = cover(d, rDot);
      float a = std::min(1.0f, aRing + aDot);
      Rgb col = aDot > 0 ? dot : ring;
      // Vormultipliziertes Alpha
      auto ch = [&](uint8_t v) { return static_cast<uint32_t>(v * a + 0.5f); };
      px[y * size + x] = (static_cast<uint32_t>(a * 255 + 0.5f) << 24) | (ch(col.r) << 16) |
                         (ch(col.g) << 8) | ch(col.b);
    }
  }

  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii = {};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

} // namespace

TrayIcon::~TrayIcon() {
  Destroy();
  for (HICON& i : icons_)
    if (i) DestroyIcon(i), i = nullptr;
}

void TrayIcon::LoadIcons() {
  if (icons_[0]) return;
  UINT dpi = GetDpiForWindow(hwnd_);
  int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96);
  icons_[static_cast<int>(TrayState::Idle)] = MakeIcon(size, {150, 150, 150}, {150, 150, 150});
  icons_[static_cast<int>(TrayState::Replay)] = MakeIcon(size, {46, 204, 113}, {46, 204, 113});
  icons_[static_cast<int>(TrayState::Recording)] = MakeIcon(size, {231, 76, 60}, {231, 76, 60});
  icons_[static_cast<int>(TrayState::Error)] = MakeIcon(size, {241, 196, 15}, {241, 196, 15});
}

NOTIFYICONDATAW TrayIcon::BaseData() const {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  return nid;
}

bool TrayIcon::Create(HWND hwnd, UINT callbackMsg) {
  hwnd_ = hwnd;
  callbackMsg_ = callbackMsg;
  LoadIcons();

  NOTIFYICONDATAW nid = BaseData();
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = callbackMsg_;
  nid.hIcon = icons_[static_cast<int>(state_)];
  wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);

  if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
    log::Error("Tray-Icon konnte nicht angelegt werden");
    return false;
  }
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
  added_ = true;
  return true;
}

void TrayIcon::Recreate() {
  added_ = false;
  Create(hwnd_, callbackMsg_);
}

void TrayIcon::Destroy() {
  if (!added_) return;
  NOTIFYICONDATAW nid = BaseData();
  Shell_NotifyIconW(NIM_DELETE, &nid);
  added_ = false;
}

void TrayIcon::SetState(TrayState state, const std::wstring& tooltip) {
  state_ = state;
  tooltip_ = tooltip;
  if (!added_) return;
  NOTIFYICONDATAW nid = BaseData();
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.hIcon = icons_[static_cast<int>(state)];
  wcsncpy_s(nid.szTip, tooltip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::Notify(const std::wstring& title, const std::wstring& text, bool isError,
                      bool sound) {
  if (!added_) return;
  NOTIFYICONDATAW nid = BaseData();
  nid.uFlags = NIF_INFO;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  nid.dwInfoFlags = (isError ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
  if (!sound) nid.dwInfoFlags |= NIIF_NOSOUND;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

} // namespace litecap
