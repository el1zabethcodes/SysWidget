// Taskbar overlay UI: borderless top-level window glued to the taskbar corner.
//
// Two presentation backends, both rendering with Direct2D/DirectWrite:
//  - "gdi"  (default): WS_EX_LAYERED + UpdateLayeredWindow with a D2D DC
//    render target into a 32bpp premultiplied DIB (TrafficMonitor approach,
//    works everywhere incl. Win10).
//  - "dcomp": D3D11 + DirectComposition + per-pixel-alpha swap chain
//    (modern path; experimental on some systems).
#pragma once
#include "common.h"
#include "content.h"
#include "settings.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <atomic>
#include <memory>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#endif

static std::wstring g_uiFailReason;

// ---------------------------------------------------------------------------
// Tiny COM pointer
// ---------------------------------------------------------------------------
template <typename T>
class Com {
 public:
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p_(o.Detach()) {}
  Com& operator=(Com&& o) noexcept {
    if (this != std::addressof(o)) {
      reset();
      p_ = o.Detach();
    }
    return *this;
  }
  T* Detach() {
    T* p = p_;
    p_ = nullptr;
    return p;
  }
  T** operator&() { reset(); return &p_; }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  operator T*() const { return p_; }
  void reset() {
    if (p_) { p_->Release(); p_ = nullptr; }
  }
  bool operator!() const { return p_ == nullptr; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

inline D2D1_COLOR_F ArgbToColor(uint32_t c) {
  return D2D1_COLOR_F{(float)((c >> 16) & 0xFF) / 255.f, (float)((c >> 8) & 0xFF) / 255.f,
                      (float)(c & 0xFF) / 255.f, (float)((c >> 24) & 0xFF) / 255.f};
}

inline UINT GetDpiOfWindowDyn(HWND h) {
  static UINT(WINAPI * pGetDpi)(HWND) = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) pGetDpi = (UINT(WINAPI*)(HWND))GetProcAddress(u32, "GetDpiForWindow");
  }
  if (pGetDpi) return pGetDpi(h);
  HDC dc = GetDC(h);
  UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(h, dc);
  return dpi ? dpi : 96;
}

// ---------------------------------------------------------------------------
// Z-order watchdog: the taskbar (also topmost) covers our card whenever it is
// activated; re-assert topmost instantly on foreground / z-order events.
static HWND g_raiseCard = nullptr, g_raiseTray = nullptr;

inline void CALLBACK RaiseHook(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObj, LONG idChild,
                               DWORD, DWORD) {
  if (!g_raiseCard || idObj != OBJID_WINDOW || idChild != 0) return;
  if (ev == EVENT_SYSTEM_FOREGROUND)
    PostMessageW(g_raiseCard, WM_APP_RAISE, 0, 0);
  else if (ev == EVENT_OBJECT_REORDER && hwnd == g_raiseTray)
    PostMessageW(g_raiseCard, WM_APP_RAISE, 0, 0);
}

// ---------------------------------------------------------------------------
struct LineView {
  std::wstring icon;
  std::wstring label;        // secondary color
  std::wstring value;        // primary color
  uint32_t valueColor = 0;   // 0 -> default primary
  bool hasBar = false;
  double bar = 0;
};

// Layered-window surface: DIB + memory DC, later bound to a D2D DC target.
struct LayeredSurface {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  void* bits = nullptr;
  UINT w = 0, h = 0;

  bool create(UINT width, UINT height) {
    destroy();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)width;
    bi.bmiHeader.biHeight = -(LONG)height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
      dc = CreateCompatibleDC(screen);
      SelectObject(dc, bmp);
    }
    ReleaseDC(nullptr, screen);
    if (!bmp || !dc) { destroy(); return false; }
    w = width;
    h = height;
    return true;
  }
  void destroy() {
    if (dc) { DeleteDC(dc); dc = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    bits = nullptr;
    w = h = 0;
  }
};

class WidgetUI {
 public:
  bool create(SharedState& st);
  int run();

 private:
  static LRESULT CALLBACK CardProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK FlyProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT cardHandler(UINT, WPARAM, LPARAM);
  LRESULT flyHandler(UINT, WPARAM, LPARAM);

  bool initBackend();
  bool makeFormats();
  bool attachSwapChain(bool flyout);  // dcomp only
  void ensureCardSurface();           // gdi only
  void ensureFlySurface();            // gdi only
  void presentCard();
  void presentFly();
  void glue(bool force);
  void renderCard();
  void renderFlyout();
  void scheduleRender() { dirtyRender_.store(true, std::memory_order_relaxed); }
  LineView buildLine(int idx, ULONGLONG now, bool withNote);
  float measure(IDWriteTextFormat* fmt, const std::wstring& text);
  void drawBar(ID2D1RenderTarget* rt, float x, float y, float w, float h, double v,
               D2D1_COLOR_F fill);
  void drawChart(ID2D1RenderTarget* rt, float x, float y, float w, float h,
                 const std::deque<double>& hist, double maxV, D2D1_COLOR_F line);
  void toggleFlyout(bool open);
  void trayMenu();
  HICON makeTrayIcon();
  void palette();
  bool useDcomp() const { return backend_ == L"dcomp"; }

  SharedState* st_ = nullptr;
  HWND card_ = nullptr;
  HWND fly_ = nullptr;
  UINT msgTaskbarCreated_ = 0;
  bool flyOpen_ = false;
  bool hoverCard_ = false;
  bool trackingMouse_ = false;
  bool trackingFly_ = false;
  std::atomic<bool> dirtyRender_{true};
  bool trayAdded_ = false;

  // ---- common graphics
  Com<ID2D1Factory1> d2f_;
  Com<IDWriteFactory> dwf_;
  Com<IDWriteTextFormat> fmtText_, fmtIcon_, fmtBig_, fmtSmall_, fmtTitle_;

  // ---- gdi backend (layered windows)
  Com<ID2D1DCRenderTarget> crc_;  // card target
  Com<ID2D1DCRenderTarget> frt_;  // flyout target
  Com<ID2D1SolidColorBrush> gbrush_;
  LayeredSurface cLayer_, fLayer_;

  // ---- dcomp backend
  Com<ID3D11Device> d3d_;
  Com<ID3D11DeviceContext> d3dCtx_;
  Com<IDXGIFactory2> dxgiF_;
  Com<ID2D1Device> d2dev_;
  Com<IDCompositionDevice> dcomp_;
  Com<ID2D1DeviceContext> cctx_, fctx_;
  Com<IDXGISwapChain1> csc_, fsc_;
  Com<ID2D1Bitmap1> cbmp_, fbmp_;
  Com<IDCompositionTarget> ctgt_, ftgt_;
  Com<IDCompositionVisual> cviz_, fviz_;
  Com<ID2D1SolidColorBrush> brush_;

  std::wstring backend_ = L"gdi";
  float scale_ = 1.0f;
  UINT cardW_ = 0, cardH_ = 0;
  UINT flyW_ = 0, flyH_ = 0;
  RECT cardRect_{};
  bool cardVisible_ = false;

  // palette
  uint32_t colPrimary_ = 0xECFFFFFF, colSecondary_ = 0x9EFFFFFF;
  uint32_t colAccent_ = 0xFF4CC2FF, colWarn_ = 0xFFFFD84D, colCrit_ = 0xFFFF5F5F;
  uint32_t colCardBg_ = 0x14FFFFFF, colCardStroke_ = 0x17FFFFFF, colTrack_ = 0x24FFFFFF;
  bool light_ = false;

  NOTIFYICONDATAW nid_{};
  HICON trayIcon_ = nullptr;
  SettingsUI settings_;
  HWINEVENTHOOK hookFg_ = nullptr, hookReorder_ = nullptr;

  bool trayAbove(HWND tray) const;
  static constexpr UINT TID_SAMPLE = 1, TID_ANIM = 2, TID_GLUE = 3;
};

// ---------------------------------------------------------------------------
// Tray icon generated at runtime (mini bar-chart glyph, Fluent accent color).
// ---------------------------------------------------------------------------
inline HICON WidgetUI::makeTrayIcon() {
  const int S = 32;
  std::vector<uint32_t> px((size_t)S * S, 0);
  uint32_t acc = colAccent_ | 0xFF000000u;
  auto bar = [&](int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; y++)
      for (int x = x0; x < x0 + w; x++) {
        int dxl = x - x0, dxr = x0 + w - 1 - x, dyt = y - y0, dyb = y0 + h - 1 - y;
        int m = std::min({dxl, dxr, dyt, dyb});
        if (m < 0) continue;
        if (m == 0 && ((dxl < 1 && dyt < 1) || (dxr < 1 && dyt < 1) ||
                       (dxl < 1 && dyb < 1) || (dxr < 1 && dyb < 1)))
          continue;  // 1px corner rounding
        px[(size_t)y * S + x] = acc;
      }
  };
  int baseline = 25;
  bar(6, baseline - 8, 5, 8);
  bar(14, baseline - 15, 5, 15);
  bar(22, baseline - 22, 5, 22);

  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = S;
  bi.bV5Height = -S;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (color && bits) memcpy(bits, px.data(), px.size() * 4);
  std::vector<BYTE> mask((size_t)S * S / 8, 0);
  HBITMAP hmask = CreateBitmap(S, S, 1, 1, mask.data());
  ReleaseDC(nullptr, dc);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = hmask;
  HICON icon = CreateIconIndirect(&ii);
  if (color) DeleteObject(color);
  if (hmask) DeleteObject(hmask);
  return icon;
}

inline void WidgetUI::palette() {
  light_ = IsLightTheme();
  colAccent_ = GetAccentArgb();
  int op = 8;
  if (st_) {
    AcquireSRWLockShared(&st_->lock);
    op = st_->cfg.bgOpacity;
    ReleaseSRWLockShared(&st_->lock);
  }
  uint32_t a = (uint32_t)(std::min(100, std::max(0, op)) * 255 / 100);
  if (light_) {
    colPrimary_ = 0xE6000000;
    colSecondary_ = 0x87000000;
    colCardBg_ = a << 24;  // black tint
    colCardStroke_ = 0x12000000;
    colTrack_ = 0x1E000000;
  } else {
    colPrimary_ = 0xECFFFFFF;
    colSecondary_ = 0x9EFFFFFF;
    colCardBg_ = (a << 24) | 0x00FFFFFF;  // white tint
    colCardStroke_ = 0x17FFFFFF;
    colTrack_ = 0x24FFFFFF;
  }
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------
inline bool WidgetUI::initBackend() {
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2f_);
  if (FAILED(hr)) {
    g_uiFailReason = WFmt(L"D2D1CreateFactory hr=0x%08lX", (unsigned long)hr);
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 (IUnknown**)&dwf_))) {
    g_uiFailReason = L"DWriteCreateFactory failed";
    return false;
  }
  if (!makeFormats()) {
    g_uiFailReason = L"text formats failed";
    return false;
  }

  if (!useDcomp()) {
    D2D1_RENDER_TARGET_PROPERTIES rtp{};
    rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    rtp.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
    if (FAILED(d2f_->CreateDCRenderTarget(&rtp, &crc_))) {
      g_uiFailReason = L"CreateDCRenderTarget(card) failed";
      return false;
    }
    if (FAILED(d2f_->CreateDCRenderTarget(&rtp, &frt_))) {
      g_uiFailReason = L"CreateDCRenderTarget(flyout) failed";
      return false;
    }
    if (FAILED(crc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &gbrush_))) {
      g_uiFailReason = L"brush failed";
      return false;
    }
    return true;
  }

  // ---- dcomp backend
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                         D3D11_SDK_VERSION, &d3d_, nullptr, &d3dCtx_);
  if (FAILED(hr))
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &d3d_, nullptr, &d3dCtx_);
  if (FAILED(hr)) {
    g_uiFailReason = WFmt(L"D3D11CreateDevice hr=0x%08lX", (unsigned long)hr);
    return false;
  }
  Com<IDXGIDevice> dxgiDev;
  if (FAILED(d3d_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
    g_uiFailReason = L"QI IDXGIDevice failed";
    return false;
  }
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&dxgiF_))) {
    g_uiFailReason = L"CreateDXGIFactory1 failed";
    return false;
  }
  if (FAILED(d2f_->CreateDevice(dxgiDev, &d2dev_))) {
    g_uiFailReason = L"D2D CreateDevice failed";
    return false;
  }
  if (FAILED(d2dev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &cctx_)) ||
      FAILED(d2dev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &fctx_))) {
    g_uiFailReason = L"D2D device contexts failed";
    return false;
  }
  if (FAILED(DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&dcomp_))) {
    g_uiFailReason = L"DCompositionCreateDevice failed";
    return false;
  }
  if (FAILED(dcomp_->CreateTargetForHwnd(card_, TRUE, &ctgt_))) {
    g_uiFailReason = L"CreateTargetForHwnd failed";
    return false;
  }
  if (FAILED(dcomp_->CreateVisual(&cviz_))) {
    g_uiFailReason = L"CreateVisual failed";
    return false;
  }
  ctgt_->SetRoot(cviz_);
  if (!attachSwapChain(false)) {
    if (g_uiFailReason.empty()) g_uiFailReason = L"attachSwapChain(card) failed";
    return false;
  }
  if (FAILED(cctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) {
    g_uiFailReason = L"brush failed";
    return false;
  }
  return true;
}

inline bool WidgetUI::makeFormats() {
  double fs = st_ ? st_->cfg.fontSize : 12.0;
  struct Helper {
    IDWriteFactory* f;
    bool mk(const wchar_t* const* names, float size, DWRITE_FONT_WEIGHT w,
            Com<IDWriteTextFormat>& out) {
      for (int i = 0; names[i]; i++) {
        Com<IDWriteTextFormat> fmt;
        if (SUCCEEDED(f->CreateTextFormat(names[i], nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt))) {
          fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
          out = std::move(fmt);
          return true;
        }
      }
      return false;
    }
  } h{dwf_};

  static const wchar_t* kTextNames[] = {L"Segoe UI Variable Text", L"Segoe UI", nullptr};
  static const wchar_t* kIconNames[] = {L"Segoe Fluent Icons", L"Segoe MDL2 Assets",
                                        L"Segoe UI Symbol", nullptr};
  Com<IDWriteTextFormat> text, icon, big, small, title;
  if (!h.mk(kTextNames, (float)fs * scale_, DWRITE_FONT_WEIGHT_NORMAL, text)) return false;
  if (!h.mk(kIconNames, 11.f * scale_, DWRITE_FONT_WEIGHT_NORMAL, icon)) return false;
  if (!h.mk(kTextNames, 24.f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD, big)) return false;
  if (!h.mk(kTextNames, 10.5f * scale_, DWRITE_FONT_WEIGHT_NORMAL, small)) return false;
  if (!h.mk(kTextNames, 13.f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD, title)) return false;
  fmtText_ = std::move(text);
  fmtIcon_ = std::move(icon);
  fmtBig_ = std::move(big);
  fmtSmall_ = std::move(small);
  fmtTitle_ = std::move(title);
  return true;
}

inline bool WidgetUI::attachSwapChain(bool flyout) {
  HWND hwnd = flyout ? fly_ : card_;
  UINT w = flyout ? flyW_ : cardW_;
  UINT h = flyout ? flyH_ : cardH_;
  if (!hwnd || w == 0 || h == 0) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = w;
  desc.Height = h;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc = {1, 0};
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.Flags = 0;

  Com<IDXGISwapChain1> sc;
  if (FAILED(dxgiF_->CreateSwapChainForComposition(d3d_, &desc, nullptr, &sc))) return false;
  Com<IDXGISurface> surf;
  if (FAILED(sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) return false;
  D2D1_BITMAP_PROPERTIES1 bp{};
  bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
  bp.dpiX = bp.dpiY = 96;
  bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
  Com<ID2D1Bitmap1> bmp;
  if (FAILED((flyout ? fctx_ : cctx_)->CreateBitmapFromDxgiSurface(surf, &bp, &bmp)))
    return false;

  if (flyout) {
    fsc_ = std::move(sc);
    fbmp_ = std::move(bmp);
    fctx_->SetTarget(fbmp_);
    if (!ftgt_) {
      if (FAILED(dcomp_->CreateTargetForHwnd(fly_, TRUE, &ftgt_))) return false;
      if (FAILED(dcomp_->CreateVisual(&fviz_))) return false;
      ftgt_->SetRoot(fviz_);
    }
    fviz_->SetContent(fsc_);
  } else {
    csc_ = std::move(sc);
    cbmp_ = std::move(bmp);
    cctx_->SetTarget(cbmp_);
    cviz_->SetContent(csc_);
  }
  dcomp_->Commit();
  return true;
}

inline void WidgetUI::ensureCardSurface() {
  UINT w = std::max<UINT>(1, cardW_), h = std::max<UINT>(1, cardH_);
  if (cLayer_.w == w && cLayer_.h == h) return;
  cLayer_.create(w, h);
  RECT rc{0, 0, (LONG)w, (LONG)h};
  crc_->BindDC(cLayer_.dc, &rc);
}

inline void WidgetUI::ensureFlySurface() {
  UINT w = std::max<UINT>(1, flyW_), h = std::max<UINT>(1, flyH_);
  if (fLayer_.w == w && fLayer_.h == h) return;
  fLayer_.create(w, h);
  RECT rc{0, 0, (LONG)w, (LONG)h};
  frt_->BindDC(fLayer_.dc, &rc);
}

inline void WidgetUI::presentCard() {
  if (useDcomp()) {
    if (csc_) {
      csc_->Present(1, 0);
      dcomp_->Commit();
    }
    return;
  }
  if (!cLayer_.dc || !cLayer_.w) return;
  HDC screen = GetDC(nullptr);
  POINT src{0, 0};
  SIZE size{(LONG)cLayer_.w, (LONG)cLayer_.h};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(card_, screen, nullptr, &size, cLayer_.dc, &src, 0, &bf, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

inline void WidgetUI::presentFly() {
  if (useDcomp()) {
    if (fsc_) {
      fsc_->Present(1, 0);
      dcomp_->Commit();
    }
    return;
  }
  if (!fLayer_.dc || !fLayer_.w || !fly_) return;
  HDC screen = GetDC(nullptr);
  POINT src{0, 0};
  SIZE size{(LONG)fLayer_.w, (LONG)fLayer_.h};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(fly_, screen, nullptr, &size, fLayer_.dc, &src, 0, &bf, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------------------
inline LineView WidgetUI::buildLine(int idx, ULONGLONG now, bool withNote) {
  LineView v;
  const LineSpec& L = st_->lines[idx];
  if (L.kind == LineKind::Hidden) return v;
  if (withNote && st_->note.active) {
    v.value = st_->note.text;
    v.valueColor = colAccent_ | 0xFF000000u;
    return v;
  }
  v.icon = L.icon;
  switch (L.kind) {
    case LineKind::Cpu:
      v.label = L"CPU";
      v.value = WFmt(L"%.0f%%", st_->stats.cpuPercent);
      v.hasBar = true;
      v.bar = st_->stats.cpuPercent / 100.0;
      v.valueColor = st_->stats.cpuPercent >= 90 ? colCrit_
                      : st_->stats.cpuPercent >= 70 ? colWarn_
                                                     : colPrimary_;
      break;
    case LineKind::Ram:
      v.label = L"MEM";
      v.value = WFmt(L"%.0f%%  %.1f/%.0fG", st_->stats.ramPercent, st_->stats.ramUsedGB,
                     st_->stats.ramTotalGB);
      v.hasBar = true;
      v.bar = st_->stats.ramPercent / 100.0;
      v.valueColor = st_->stats.ramPercent >= 90 ? colCrit_
                      : st_->stats.ramPercent >= 70 ? colWarn_
                                                     : colPrimary_;
      break;
    case LineKind::Text:
    case LineKind::Anim:
    case LineKind::Script: {
      std::wstring full = LineViewText(L, st_->stats, now);
      size_t sep = full.find(L" | ");
      if (sep != std::wstring::npos) {
        v.label = full.substr(0, sep);
        v.value = full.substr(sep + 3);
      } else {
        v.value = full;
      }
      if (L.kind == LineKind::Anim && !L.frames.empty()) v.valueColor = colAccent_ | 0xFF000000u;
      if (L.color) v.valueColor = L.color;
      if (L.barOverride >= 0) {
        v.hasBar = true;
        v.bar = L.barOverride;
      }
      break;
    }
  }
  return v;
}

inline float WidgetUI::measure(IDWriteTextFormat* fmt, const std::wstring& text) {
  if (text.empty() || !fmt) return 0;
  Com<IDWriteTextLayout> lay;
  if (FAILED(dwf_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 4096.f, 200.f, &lay)))
    return (float)text.size() * 7.f;
  DWRITE_TEXT_METRICS m{};
  lay->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

inline void WidgetUI::drawBar(ID2D1RenderTarget* rt, float x, float y, float w, float h,
                              double v, D2D1_COLOR_F fill) {
  ID2D1SolidColorBrush* br = useDcomp() ? brush_.get() : gbrush_.get();
  br->SetColor(ArgbToColor(colTrack_));
  rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), h / 2.f, h / 2.f),
                           br);
  if (v > 0.005) {
    float fw = (float)(w * std::min(1.0, std::max(0.0, v)));
    if (fw < h) fw = h;
    br->SetColor(fill);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + fw, y + h), h / 2.f, h / 2.f),
                             br);
  }
}

inline void WidgetUI::drawChart(ID2D1RenderTarget* rt, float x, float y, float w, float h,
                                const std::deque<double>& hist, double maxV,
                                D2D1_COLOR_F line) {
  ID2D1SolidColorBrush* br = useDcomp() ? brush_.get() : gbrush_.get();
  br->SetColor(ArgbToColor(colTrack_));
  rt->DrawLine(D2D1::Point2F(x, y + h + 0.5f), D2D1::Point2F(x + w, y + h + 0.5f), br, 1.f);
  if (hist.size() < 2) return;
  float step = w / (float)(hist.size() - 1);
  br->SetColor(line);
  auto yv = [&](double val) {
    float t = (float)(val / maxV);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return y + h - t * h;
  };
  for (size_t i = 1; i < hist.size(); i++)
    rt->DrawLine(D2D1::Point2F(x + (float)(i - 1) * step, yv(hist[i - 1])),
                 D2D1::Point2F(x + (float)i * step, yv(hist[i])), br, 1.5f);
}

// ---------------------------------------------------------------------------
inline void WidgetUI::renderCard() {
  if (cardW_ == 0 || cardH_ == 0) return;
  ID2D1RenderTarget* rt = nullptr;
  ID2D1SolidColorBrush* br = nullptr;
  if (useDcomp()) {
    if (!cctx_) return;
    rt = cctx_;
    br = brush_;
  } else {
    if (!crc_) return;
    ensureCardSurface();
    if (!cLayer_.dc) return;
    rt = crc_;
    br = gbrush_;
  }

  ULONGLONG now = GetTickCount64();
  LineView views[2];
  bool vis[2] = {false, false};
  std::wstring material;
  int firstVis = -1;
  {
    AcquireSRWLockShared(&st_->lock);
    material = st_->cfg.material;
    for (int i = 0; i < 2; i++) {
      vis[i] = st_->lines[i].kind != LineKind::Hidden;
      if (vis[i] && firstVis < 0) firstVis = i;
    }
    for (int i = 0; i < 2; i++)
      if (vis[i]) views[i] = buildLine(i, now, i == firstVis);
    ReleaseSRWLockShared(&st_->lock);
  }
  if (!vis[0] && !vis[1]) {
    if (cardVisible_) {
      ShowWindow(card_, SW_HIDE);
      cardVisible_ = false;
    }
    return;
  }

  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0, 0, 0, 0));

  float W = (float)cardW_, H = (float)cardH_;
  float corner = 6.f * scale_;
  if (material != L"flat") {
    uint32_t bg = colCardBg_;
    if (hoverCard_) {
      uint32_t al = (bg >> 24) + 10;
      if (al > 255) al = 255;
      bg = (al << 24) | (bg & 0xFFFFFF);
    }
    br->SetColor(ArgbToColor(bg));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), corner, corner), br);
    br->SetColor(ArgbToColor(colCardStroke_));
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), corner, corner), br, 1.f);
  }

  float padX = 10.f * scale_;
  float padY = 4.f * scale_;
  int nVis = (vis[0] ? 1 : 0) + (vis[1] ? 1 : 0);
  float lineH = (H - 2 * padY) / (float)(nVis > 0 ? nVis : 1);
  float iconSize = 11.f * scale_;
  int row = 0;

  for (int i = 0; i < 2; i++) {
    if (!vis[i]) continue;
    LineView& v = views[i];
    float yTop = padY + row * lineH;
    row++;
    float right = W - padX;
    float x = padX;

    if (!v.icon.empty()) {
      br->SetColor(ArgbToColor(v.valueColor ? v.valueColor : (colAccent_ | 0xFF000000u)));
      rt->DrawTextW(v.icon.c_str(), (UINT32)v.icon.size(), fmtIcon_,
                    D2D1::RectF(x, yTop, x + iconSize * 2.2f, yTop + lineH), br);
      x += measure(fmtIcon_, v.icon) + 6.f * scale_;
    }
    if (!v.label.empty()) {
      float lw = measure(fmtText_, v.label);
      br->SetColor(ArgbToColor(colSecondary_));
      rt->DrawTextW(v.label.c_str(), (UINT32)v.label.size(), fmtText_,
                    D2D1::RectF(x, yTop, x + lw + 2, yTop + lineH), br);
      x += lw + 7.f * scale_;
    }

    float barW = 28.f * scale_;
    float barH = 4.f * scale_;
    float barX = right - barW;
    float anchorRight = v.hasBar ? barX - 8.f * scale_ : right;

    if (!v.value.empty()) {
      float vw = measure(fmtText_, v.value);
      if (vw > anchorRight - x) vw = anchorRight - x;
      br->SetColor(ArgbToColor(v.valueColor ? v.valueColor : colPrimary_));
      rt->DrawTextW(v.value.c_str(), (UINT32)v.value.size(), fmtText_,
                    D2D1::RectF(anchorRight - vw, yTop, anchorRight, yTop + lineH), br);
    }
    if (v.hasBar)
      drawBar(rt, barX, yTop + lineH / 2.f - barH / 2.f, barW, barH, v.bar,
              ArgbToColor(v.valueColor ? v.valueColor : (colAccent_ | 0xFF000000u)));
  }

  rt->EndDraw();
  presentCard();
}

// ---------------------------------------------------------------------------
inline void WidgetUI::renderFlyout() {
  if (flyW_ == 0 || !fly_) return;
  ID2D1RenderTarget* rt = nullptr;
  ID2D1SolidColorBrush* br = nullptr;
  if (useDcomp()) {
    if (!fctx_) return;
    rt = fctx_;
    br = brush_;
  } else {
    if (!frt_) return;
    ensureFlySurface();
    if (!fLayer_.dc) return;
    rt = frt_;
    br = gbrush_;
  }

  LineView views[2];
  std::deque<double> cpu, ram;
  SysStats stats{};
  bool customLine[2] = {false, false};
  {
    AcquireSRWLockShared(&st_->lock);
    views[0] = buildLine(0, NowMs(), true);
    views[1] = buildLine(1, NowMs(), false);
    cpu = st_->histCpu;
    ram = st_->histRam;
    stats = st_->stats;
    for (int i = 0; i < 2; i++) {
      LineKind k = st_->lines[i].kind;
      customLine[i] = k == LineKind::Text || k == LineKind::Anim || k == LineKind::Script;
    }
    ReleaseSRWLockShared(&st_->lock);
  }

  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0, 0, 0, 0));

  float W = (float)flyW_;
  float H = (float)flyH_;
  float corner = 8.f * scale_;
  uint32_t panelBg = light_ ? 0xFFF6F6F6 : 0xF21F1F23;
  br->SetColor(ArgbToColor(panelBg));
  rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), corner, corner), br);
  br->SetColor(ArgbToColor(light_ ? 0x14000000 : 0x1FFFFFFF));
  rt->DrawRoundedRectangle(
      D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), corner, corner), br, 1.f);

  auto text = [&](const std::wstring& t, IDWriteTextFormat* f, const D2D1_RECT_F& rc,
                  uint32_t c) {
    br->SetColor(ArgbToColor(c));
    rt->DrawTextW(t.c_str(), (UINT32)t.size(), f, rc, br);
  };

  float pad = 16.f * scale_;
  float y = pad;

  text(L"Мониторинг системы", fmtTitle_, D2D1::RectF(pad, y, W - pad, y + 20 * scale_),
       colSecondary_);
  y += 24.f * scale_;

  struct Block {
    const wchar_t* name;
    double pct;
    const std::deque<double>* hist;
    uint32_t color;
  };
  auto blkColor = [](double pct, uint32_t prim, uint32_t warn, uint32_t crit) {
    return pct >= 90 ? crit : pct >= 70 ? warn : prim;
  };
  Block blocks[2] = {
      {L"ЦП", stats.cpuPercent, &cpu,
       blkColor(stats.cpuPercent, colPrimary_, colWarn_, colCrit_)},
      {L"Память", stats.ramPercent, &ram,
       blkColor(stats.ramPercent, colPrimary_, colWarn_, colCrit_)},
  };

  float chartH = 46.f * scale_;
  float rowH = 30.f * scale_;

  for (auto& b : blocks) {
    text(b.name, fmtText_, D2D1::RectF(pad, y, pad + 90 * scale_, y + rowH), colSecondary_);
    std::wstring val = WFmt(L"%.0f%%", b.pct);
    float vw = measure(fmtBig_, val);
    br->SetColor(ArgbToColor(b.color));
    rt->DrawTextW(val.c_str(), (UINT32)val.size(), fmtBig_,
                  D2D1::RectF(W - pad - vw, y - 2 * scale_, W - pad, y + rowH), br);
    y += rowH;
    drawChart(rt, pad, y, W - 2 * pad, chartH, *b.hist, 100.0,
              ArgbToColor(colAccent_ | 0xFF000000u));
    y += chartH + 12.f * scale_;
  }

  text(WFmt(L"%.1f из %.0f ГБ занято", stats.ramUsedGB, stats.ramTotalGB), fmtSmall_,
       D2D1::RectF(pad, y, W - pad, y + 16 * scale_), colSecondary_);
  y += 22.f * scale_;

  br->SetColor(ArgbToColor(light_ ? 0x14000000 : 0x1FFFFFFF));
  rt->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(W - pad, y), br, 1.f);
  y += 10.f * scale_;

  int shown = 0;
  for (int i = 0; i < 2; i++) {
    if (!customLine[i]) continue;
    if (views[i].value.empty() && views[i].label.empty()) continue;
    std::wstring row =
        views[i].label.empty() ? views[i].value : views[i].label + L": " + views[i].value;
    text(row, fmtText_, D2D1::RectF(pad, y, W - pad, y + 20 * scale_), colPrimary_);
    y += 20.f * scale_;
    if (++shown >= 3) break;
  }

  float fy = H - pad - 14 * scale_;
  text(L"\\\\.\\pipe\\SysWidget · MCP ready", fmtSmall_,
       D2D1::RectF(pad, fy, W - pad, fy + 16 * scale_), colSecondary_);

  rt->EndDraw();
  presentFly();
}

// ---------------------------------------------------------------------------
bool WidgetUI::trayAbove(HWND tray) const {
  HWND h = GetWindow(card_, GW_HWNDPREV);
  for (int i = 0; h && i < 64; i++) {
    if (h == tray) return true;
    h = GetWindow(h, GW_HWNDPREV);
  }
  return false;
}

inline void WidgetUI::glue(bool force) {
  HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!tray) {
    if (cardVisible_) { ShowWindow(card_, SW_HIDE); cardVisible_ = false; }
    if (flyOpen_) toggleFlyout(false);
    return;
  }
  g_raiseTray = tray;
  RECT tr{};
  GetWindowRect(tray, &tr);
  HMONITOR mon = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);

  UINT dpi = GetDpiOfWindowDyn(tray);
  float s = dpi / 96.f;
  if (s != scale_) {
    scale_ = s;
    makeFormats();
    force = true;
  }

  LONG tbW = tr.right - tr.left, tbH = tr.bottom - tr.top;
  bool horizontal = tbW >= tbH;

  RECT inter = tr;
  IntersectRect(&inter, &tr, &mi.rcMonitor);
  bool trayOnScreen =
      horizontal ? (inter.bottom - inter.top >= tbH / 2) : (inter.right - inter.left >= tbW / 2);
  if (!trayOnScreen) {
    if (cardVisible_) { ShowWindow(card_, SW_HIDE); cardVisible_ = false; }
    return;
  }

  UINT wantW;
  bool right;
  {
    AcquireSRWLockShared(&st_->lock);
    wantW = (UINT)(st_->cfg.width * scale_ + 0.5f);
    right = st_->cfg.align == L"right";
    ReleaseSRWLockShared(&st_->lock);
  }
  if (wantW < 96) wantW = 96;
  UINT wantH = horizontal ? (UINT)std::min(40.f * scale_, (float)tbH - 8.f * scale_)
                          : (UINT)std::min(40.f * scale_, (float)tbW - 8.f * scale_);
  if (wantH < 20) wantH = 20;

  int x, y;
  if (horizontal) {
    x = right ? tr.right - (int)(8 * scale_) - (int)wantW : tr.left + (int)(8 * scale_);
    y = tr.top + ((int)tbH - (int)wantH) / 2;
  } else {
    x = tr.left + ((int)tbW - (int)wantW) / 2;
    y = tr.top + (int)(8 * scale_);
  }

  RECT want{x, y, x + (int)wantW, y + (int)wantH};
  RECT cur{};
  bool haveCur = GetWindowRect(card_, &cur) != FALSE;
  // compare against the ACTUAL window rect so external shifts/DPI/monitor
  // changes are always corrected (adaptive positioning)
  bool moved = force || !haveCur || !EqualRect(&want, &cur) || !cardVisible_ ||
               wantW != cardW_ || wantH != cardH_;

  if (moved) {
    cardW_ = wantW;
    cardH_ = wantH;
    SetWindowPos(card_, HWND_TOPMOST, x, y, (int)wantW, (int)wantH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    GetWindowRect(card_, &cardRect_);
    cardVisible_ = true;
    if (useDcomp() && csc_ && wantW && wantH) {
      cctx_->SetTarget(nullptr);
      cbmp_.reset();
      if (SUCCEEDED(csc_->ResizeBuffers(0, wantW, wantH, DXGI_FORMAT_UNKNOWN, 0))) {
        Com<IDXGISurface> surf;
        if (SUCCEEDED(csc_->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) {
          D2D1_BITMAP_PROPERTIES1 bp{};
          bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
          bp.dpiX = bp.dpiY = 96;
          bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
          Com<ID2D1Bitmap1> bmp;
          if (SUCCEEDED(cctx_->CreateBitmapFromDxgiSurface(surf, &bp, &bmp))) {
            cbmp_ = std::move(bmp);
            cctx_->SetTarget(cbmp_);
          }
        }
      }
    }
    scheduleRender();
  }

  // raise above the taskbar only when it actually covers us (cheap z-order check)
  if (cardVisible_ && trayAbove(tray)) {
    SetWindowPos(card_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (flyOpen_ && fly_)
      SetWindowPos(fly_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  if (flyOpen_ && fly_ && moved) {
    RECT fr{};
    GetWindowRect(fly_, &fr);
    int fy = cardRect_.top - (fr.bottom - fr.top) - (int)(8 * scale_);
    if (fy < mi.rcWork.top) fy = cardRect_.bottom + (int)(8 * scale_);
    SetWindowPos(fly_, HWND_TOPMOST, cardRect_.left, fy, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

// ---------------------------------------------------------------------------
inline void WidgetUI::toggleFlyout(bool open) {
  if (open == flyOpen_) return;
  if (open) {
    if (!fly_) {
      WNDCLASSW wc{};
      wc.lpfnWndProc = FlyProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"SysWidgetFly";
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      RegisterClassW(&wc);
      DWORD exFly = useDcomp() ? (WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW |
                                  WS_EX_NOACTIVATE | WS_EX_TOPMOST)
                               : (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                  WS_EX_TOPMOST);
      fly_ = CreateWindowExW(exFly, L"SysWidgetFly", L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, GetModuleHandleW(nullptr), this);
      if (!fly_) return;
      SetWindowLongPtrW(fly_, GWLP_USERDATA, (LONG_PTR)this);
    }
    flyW_ = (UINT)(300 * scale_ + 0.5f);
    flyH_ = (UINT)(332 * scale_ + 0.5f);
    if (useDcomp()) {
      if (fsc_) {
        fctx_->SetTarget(nullptr);
        fbmp_.reset();
        fsc_.reset();
      }
      if (!attachSwapChain(true)) return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(card_, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    int fy = cardRect_.top - (int)flyH_ - (int)(8 * scale_);
    if (fy < mi.rcWork.top) fy = cardRect_.bottom + (int)(8 * scale_);
    SetWindowPos(fly_, HWND_TOPMOST, cardRect_.left, fy, (int)flyW_, (int)flyH_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    flyOpen_ = true;
    renderFlyout();
  } else {
    if (fly_) ShowWindow(fly_, SW_HIDE);
    flyOpen_ = false;
    trackingFly_ = false;
  }
}

// ---------------------------------------------------------------------------
inline void WidgetUI::trayMenu() {
  SetForegroundWindow(card_);
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1006, L"Настройки…");
  AppendMenuW(m, MF_STRING, 1001, flyOpen_ ? L"Скрыть сводку" : L"Сводка (графики)");
  AppendMenuW(m, MF_STRING, 1002, L"Сбросить строки (CPU/RAM)");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), 1003,
              L"Автозапуск вместе с Windows");
  AppendMenuW(m, MF_STRING, 1004, L"Открыть config.json");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 1005, L"Выход");
  POINT pt;
  GetCursorPos(&pt);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0,
                           card_, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case 1006:
      settings_.open(st_, card_);
      break;
    case 1001:
      toggleFlyout(!flyOpen_);
      break;
    case 1002: {
      AcquireSRWLockExclusive(&st_->lock);
      ResetLineToDefault(st_->lines[0], 0);
      ResetLineToDefault(st_->lines[1], 1);
      st_->note.active = false;
      st_->scriptVersion++;
      ReleaseSRWLockExclusive(&st_->lock);
      st_->dirty.store(true, std::memory_order_relaxed);
      SaveConfig(*st_);
      break;
    }
    case 1003:
      SetAutostart(!IsAutostartEnabled());
      break;
    case 1004: {
      EnsureConfigDir();
      ShellExecuteW(nullptr, L"open", ConfigPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    }
    case 1005:
      DestroyWindow(card_);
      break;
  }
}

// ---------------------------------------------------------------------------
inline bool WidgetUI::create(SharedState& st) {
  st_ = &st;
  palette();

  {
    AcquireSRWLockShared(&st_->lock);
    std::wstring r = st_->cfg.renderer;
    ReleaseSRWLockShared(&st_->lock);
    if (r == L"dcomp") backend_ = L"dcomp";
    else backend_ = L"gdi";  // auto -> most compatible
  }

  DWORD exCard = useDcomp()
                     ? (WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                        WS_EX_TOPMOST)
                     : (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
  WNDCLASSW wc{};
  wc.lpfnWndProc = CardProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SysWidgetCard";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  card_ = CreateWindowExW(exCard, L"SysWidgetCard", L"SysWidget", WS_POPUP, -2000, -2000, 160,
                          40, nullptr, nullptr, GetModuleHandleW(nullptr), this);
  if (!card_) {
    g_uiFailReason = L"CreateWindow failed";
    return false;
  }
  SetWindowLongPtrW(card_, GWLP_USERDATA, (LONG_PTR)this);
  st.cardHwnd = card_;
  cardW_ = 160;
  cardH_ = 40;

  if (!initBackend()) return false;

  msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

  SetTimer(card_, TID_SAMPLE, 1000, nullptr);
  SetTimer(card_, TID_ANIM, 100, nullptr);
  SetTimer(card_, TID_GLUE, 1000, nullptr);

  g_raiseCard = card_;
  g_raiseTray = FindWindowW(L"Shell_TrayWnd", nullptr);
  hookFg_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                            RaiseHook, 0, 0, WINEVENT_OUTOFCONTEXT);
  hookReorder_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr,
                                 RaiseHook, 0, 0, WINEVENT_OUTOFCONTEXT);

  if (st.cfg.trayIcon) {
    trayIcon_ = makeTrayIcon();
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = card_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = trayIcon_;
    wcscpy(nid_.szTip, L"SysWidget · CPU/RAM");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    trayAdded_ = true;
  }

  glue(true);
  renderCard();  // first frame immediately (ULW makes the window visible)
  return true;
}

inline int WidgetUI::run() {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
inline LRESULT WidgetUI::cardHandler(UINT msg, WPARAM wp, LPARAM lp) {
  if (msgTaskbarCreated_ && msg == msgTaskbarCreated_) {
    glue(true);
    return 0;
  }
  switch (msg) {
    case WM_TIMER:
      switch ((UINT)wp) {
        case TID_SAMPLE: {
          SysStats s;
          static Sampler sampler;  // single UI-thread sampler
          sampler.sample(s);
          AcquireSRWLockExclusive(&st_->lock);
          st_->stats = s;
          st_->histCpu.push_back(s.cpuPercent);
          st_->histRam.push_back(s.ramPercent);
          while (st_->histCpu.size() > 120) st_->histCpu.pop_front();
          while (st_->histRam.size() > 120) st_->histRam.pop_front();
          ReleaseSRWLockExclusive(&st_->lock);
          scheduleRender();
          if (flyOpen_) renderFlyout();
          break;
        }
        case TID_ANIM: {
          if (st_->quit.load(std::memory_order_relaxed)) {
            DestroyWindow(card_);
            return 0;
          }
          bool animating = false;
          bool expireNote = false;
          {
            AcquireSRWLockShared(&st_->lock);
            animating = st_->lines[0].kind == LineKind::Anim ||
                        st_->lines[1].kind == LineKind::Anim;
            if (st_->note.active && NowMs() > st_->note.until) expireNote = true;
            ReleaseSRWLockShared(&st_->lock);
          }
          if (expireNote) {
            AcquireSRWLockExclusive(&st_->lock);
            st_->note.active = false;
            ReleaseSRWLockExclusive(&st_->lock);
            scheduleRender();
          }
          if (animating) scheduleRender();
          if (dirtyRender_.exchange(false)) renderCard();
          break;
        }
        case TID_GLUE:
          glue(false);
          break;
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(card_, &ps);
      EndPaint(card_, &ps);
      scheduleRender();
      return 0;
    }

    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
      if (!trackingMouse_) {
        TRACKMOUSEEVENT t{};
        t.cbSize = sizeof(t);
        t.dwFlags = TME_LEAVE;
        t.hwndTrack = card_;
        TrackMouseEvent(&t);
        trackingMouse_ = true;
      }
      if (!hoverCard_) {
        hoverCard_ = true;
        scheduleRender();
      }
      return 0;

    case WM_MOUSELEAVE:
      trackingMouse_ = false;
      if (hoverCard_) {
        hoverCard_ = false;
        scheduleRender();
      }
      return 0;

    case WM_LBUTTONUP:
      settings_.open(st_, card_);
      return 0;

    case WM_APP_RAISE: {
      if (!cardVisible_) return 0;
      HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
      if (tray && trayAbove(tray))
        SetWindowPos(card_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      return 0;
    }

    case WM_APP_CFGCHANGED:
      glue(true);
      renderCard();
      return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
      glue(true);
      return 0;

    case WM_APP_HIDE:
      ShowWindow(card_, SW_HIDE);
      cardVisible_ = false;
      return 0;

    case WM_APP_SHOW:
      glue(true);
      return 0;

    case WM_APP_EXIT:
      DestroyWindow(card_);
      return 0;

    case WM_APP_TRAY:
      if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) trayMenu();
      else if (lp == WM_LBUTTONUP) toggleFlyout(!flyOpen_);
      return 0;

    case WM_DPICHANGED: {
      UINT dpi = HIWORD(wp);
      if (dpi) {
        scale_ = dpi / 96.f;
        makeFormats();
        glue(true);
      }
      return 0;
    }

    case WM_QUERYENDSESSION:
      return TRUE;

    case WM_CLOSE:
      DestroyWindow(card_);
      return 0;

    case WM_DESTROY:
      if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &nid_);
      if (trayIcon_) DestroyIcon(trayIcon_);
      toggleFlyout(false);
      settings_.requestClose();
      if (hookFg_) UnhookWinEvent(hookFg_);
      if (hookReorder_) UnhookWinEvent(hookReorder_);
      hookFg_ = hookReorder_ = nullptr;
      g_raiseCard = nullptr;
      st_->quit.store(true, std::memory_order_relaxed);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(card_, msg, wp, lp);
}

inline LRESULT CALLBACK WidgetUI::CardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  WidgetUI* self = (WidgetUI*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!self) {
    if (msg == WM_NCCREATE) {
      auto cs = (CREATESTRUCTW*)lp;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  return self->cardHandler(msg, wp, lp);
}

inline LRESULT WidgetUI::flyHandler(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
      if (!trackingFly_) {
        TRACKMOUSEEVENT t{};
        t.cbSize = sizeof(t);
        t.dwFlags = TME_LEAVE;
        t.hwndTrack = fly_;
        TrackMouseEvent(&t);
        trackingFly_ = true;
      }
      return 0;
    case WM_MOUSELEAVE:
      toggleFlyout(false);
      return 0;
    case WM_LBUTTONUP:
      toggleFlyout(false);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(fly_, &ps);
      EndPaint(fly_, &ps);
      return 0;
    }
  }
  return DefWindowProcW(fly_, msg, wp, lp);
}

inline LRESULT CALLBACK WidgetUI::FlyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  WidgetUI* self = (WidgetUI*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
  return self->flyHandler(msg, wp, lp);
}
