// WicAnimatedGif: GIF decoder via WIC with full frame-disposal composition,
// per-frame delays from /grctlext metadata, hardware bitmaps (ID2D1Bitmap),
// frame timing and aspect-preserving free scaling (see layout::GifElement).
#pragma once
#include "common.h"
#include "graphics/wic.h"

#include <d2d1.h>
#include <wincodec.h>
#include <vector>

class WicAnimatedGif {
 public:
  struct Frame {
    ID2D1Bitmap* bmp = nullptr;   // fully composed frame (premultiplied)
    float delayMs = 100;
    int disposal = 0;             // 0/1 leave, 2 restore-bg, 3 restore-prev
  };

  ~WicAnimatedGif() { clear(); }
  WicAnimatedGif(WicAnimatedGif&& o) noexcept
      : frames_(std::move(o.frames_)), w_(o.w_), h_(o.h_), current_(o.current_),
        frameStart_(o.frameStart_) {
    o.frames_.clear();
  }
  WicAnimatedGif& operator=(WicAnimatedGif&& o) noexcept {
    if (this != &o) {
      clear();
      frames_ = std::move(o.frames_);
      w_ = o.w_; h_ = o.h_; current_ = o.current_; frameStart_ = o.frameStart_;
      o.frames_.clear();
    }
    return *this;
  }
  WicAnimatedGif() = default;
  WicAnimatedGif(const WicAnimatedGif&) = delete;
  WicAnimatedGif& operator=(const WicAnimatedGif&) = delete;

  bool load(ID2D1RenderTarget* rt, const std::wstring& path) {
    clear();
    IWICImagingFactory* f = wicFactory();
    if (!f || !rt) return false;

    WicCom<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &dec)))
      return false;
    UINT count = 0;
    if (FAILED(dec->GetFrameCount(&count)) || count == 0 || count > 256) return false;

    // logical canvas size = first frame size
    {
      WicCom<IWICBitmapFrameDecode> f0;
      if (FAILED(dec->GetFrame(0, &f0)) || FAILED(f0->GetSize(&w_, &h_)) || w_ == 0 || h_ == 0)
        return false;
    }

    WicCom<IWICBitmap> canvas;
    if (FAILED(f->CreateBitmap(w_, h_, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, &canvas)))
      return false;
    if (!clearRect(canvas, 0, 0, (int)w_, (int)h_)) return false;

    int prevDisposal = 0;
    RECT prevRect{0, 0, 0, 0};
    std::vector<uint8_t> snapshot;  // canvas state before current frame blend

    for (UINT i = 0; i < count; i++) {
      WicCom<IWICBitmapFrameDecode> src;
      if (FAILED(dec->GetFrame(i, &src))) return false;

      UINT fw = w_, fh = h_;
      int fx = 0, fy = 0;
      readFrameRect(src, fx, fy, fw, fh);
      if (fx < 0 || fy < 0 || fw == 0 || fh == 0 || fx + fw > w_ || fy + fh > h_) {
        fx = 0; fy = 0; fw = w_; fh = h_;
      }

      float delayMs = readDelay(src);
      int disposal = readDisposal(src);

      // decode frame pixels → premultiplied BGRA
      WicCom<IWICFormatConverter> conv;
      if (FAILED(f->CreateFormatConverter(&conv))) return false;
      if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.f,
                                  WICBitmapPaletteTypeCustom)))
        return false;
      std::vector<uint8_t> fbuf((size_t)fw * fh * 4);
      if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)fbuf.size(), fbuf.data())))
        return false;

      // disposal handling against the previous frame
      if (i > 0) {
        if (prevDisposal == 2) {
          if (!clearRect(canvas, prevRect.left, prevRect.top,
                         prevRect.right - prevRect.left, prevRect.bottom - prevRect.top))
            return false;
        } else if (prevDisposal == 3 && !snapshot.empty()) {
          if (!writeCanvas(canvas, snapshot)) return false;
        }
      }
      // remember canvas state so disposal==3 can restore it later
      if (!readCanvas(canvas, snapshot)) return false;

      if (!blend(canvas, fx, fy, fw, fh, fbuf)) return false;

      // composed canvas → hardware bitmap
      D2D1_BITMAP_PROPERTIES bp{};
      bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
      bp.dpiX = bp.dpiY = 96;
      ID2D1Bitmap* bmp = nullptr;
      if (FAILED(rt->CreateBitmapFromWicBitmap(canvas, &bp, &bmp))) return false;
      frames_.push_back(Frame{bmp, delayMs, disposal});
      prevDisposal = disposal;
      prevRect = {fx, fy, (LONG)(fx + fw), (LONG)(fy + fh)};
    }
    current_ = 0;
    frameStart_ = GetTickCount64();
    return !frames_.empty();
  }

  void clear() {
    for (auto& fr : frames_)
      if (fr.bmp) fr.bmp->Release();
    frames_.clear();
    current_ = 0;
  }

  bool ok() const { return !frames_.empty(); }
  UINT width() const { return w_; }
  UINT height() const { return h_; }
  size_t frameCount() const { return frames_.size(); }

  // Advance by wall clock; returns the delay (ms) that should elapse until
  // the next frame change (drive a timer with it).
  DWORD advance() {
    if (frames_.empty()) return 1000;
    ULONGLONG now = GetTickCount64();
    for (int guard = 0; guard < 256; guard++) {
      float left = (float)(frameStart_ + (ULONGLONG)frames_[current_].delayMs) - (float)now;
      if (left > 0) return (DWORD)std::max(10.f, left);
      current_ = (current_ + 1) % frames_.size();
      frameStart_ += frames_[current_].delayMs > 0 ? (ULONGLONG)frames_[current_].delayMs : 100;
      if (frameStart_ > now) frameStart_ = now;
    }
    return 10;
  }

  const Frame& current() const { return frames_[current_]; }

 private:
  static float readDelay(IWICBitmapFrameDecode* src) {
    IWICMetadataQueryReader* qr = nullptr;
    float ms = 100.f;
    if (SUCCEEDED(src->GetMetadataQueryReader(&qr))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(qr->GetMetadataByName(L"/grctlext/Delay", &v))) {
        UINT d = 0;
        if (v.vt == VT_UI2) d = v.uiVal;
        else if (v.vt == VT_UI4) d = v.ulVal;
        else if (v.vt == VT_UI1) d = v.bVal;
        ms = (float)(d * 10);
        PropVariantClear(&v);
      }
      qr->Release();
    }
    if (ms <= 10.f) ms = 100.f;  // browser-style clamp for 0-delay frames
    return std::min(60000.f, ms);
  }
  static int readDisposal(IWICBitmapFrameDecode* src) {
    IWICMetadataQueryReader* qr = nullptr;
    int disp = 0;
    if (SUCCEEDED(src->GetMetadataQueryReader(&qr))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(qr->GetMetadataByName(L"/grctlext/Disposal", &v))) {
        if (v.vt == VT_UI1) disp = v.bVal;
        else if (v.vt == VT_UI2) disp = v.uiVal;
        else if (v.vt == VT_UI4) disp = (int)v.ulVal;
        PropVariantClear(&v);
      }
      qr->Release();
    }
    return disp;
  }
  static void readFrameRect(IWICBitmapFrameDecode* src, int& fx, int& fy, UINT& fw, UINT& fh) {
    IWICMetadataQueryReader* qr = nullptr;
    if (FAILED(src->GetMetadataQueryReader(&qr))) return;
    auto u = [&](const wchar_t* name) -> int {
      PROPVARIANT v;
      PropVariantInit(&v);
      int out = -1;
      if (SUCCEEDED(qr->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI2) out = v.uiVal;
        else if (v.vt == VT_UI4) out = (int)v.ulVal;
        else if (v.vt == VT_UI1) out = v.bVal;
        PropVariantClear(&v);
      }
      return out;
    };
    int L = u(L"/imgdesc/Left"), T = u(L"/imgdesc/Top");
    int W = u(L"/imgdesc/Width"), H = u(L"/imgdesc/Height");
    qr->Release();
    if (L >= 0 && T >= 0 && W > 0 && H > 0) {
      fx = L; fy = T; fw = (UINT)W; fh = (UINT)H;
    }
  }

  static bool readCanvas(IWICBitmap* bmp, std::vector<uint8_t>& out) {
    IWICBitmapLock* lk = nullptr;
    UINT w = 0, h = 0;
    if (FAILED(bmp->GetSize(&w, &h)) || FAILED(bmp->Lock(nullptr, WICBitmapLockRead, &lk)))
      return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lk->GetStride(&stride)) || FAILED(lk->GetDataPointer(&size, &data))) {
      lk->Release();
      return false;
    }
    out.resize((size_t)stride * h);
    for (UINT y = 0; y < h; y++) memcpy(out.data() + (size_t)y * stride, data + (size_t)y * stride, (size_t)w * 4);
    lk->Release();
    return true;
  }
  static bool writeCanvas(IWICBitmap* bmp, const std::vector<uint8_t>& in) {
    UINT w = 0, h = 0;
    if (FAILED(bmp->GetSize(&w, &h)) || in.size() < (size_t)w * h * 4) return false;
    IWICBitmapLock* lk = nullptr;
    if (FAILED(bmp->Lock(nullptr, WICBitmapLockWrite, &lk)))
      return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lk->GetStride(&stride)) || FAILED(lk->GetDataPointer(&size, &data)) ||
        in.size() < (size_t)stride * h) {
      lk->Release();
      return false;
    }
    for (UINT y = 0; y < h; y++) memcpy(data + (size_t)y * stride, in.data() + (size_t)y * stride, (size_t)w * 4);
    lk->Release();
    return true;
  }
  static bool clearRect(IWICBitmap* bmp, int rx, int ry, int rw, int rh) {
    IWICBitmapLock* lk = nullptr;
    WICRect rc{rx, ry, rw, rh};
    if (FAILED(bmp->Lock(&rc, WICBitmapLockWrite, &lk))) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lk->GetStride(&stride)) || FAILED(lk->GetDataPointer(&size, &data))) {
      lk->Release();
      return false;
    }
    UINT rows = (UINT)rh;
    for (UINT y = 0; y < rows; y++) memset(data + (size_t)y * stride, 0, (size_t)rw * 4);
    lk->Release();
    return true;
  }
  // premultiplied-BGRA "source over"
  static bool blend(IWICBitmap* canvas, int fx, int fy, UINT fw, UINT fh,
                    const std::vector<uint8_t>& fbuf) {
    UINT w = 0, h = 0;
    if (FAILED(canvas->GetSize(&w, &h))) return false;
    WICRect rc{fx, fy, (int)fw, (int)fh};
    IWICBitmapLock* lk = nullptr;
    if (FAILED(canvas->Lock(&rc, WICBitmapLockWrite, &lk))) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lk->GetStride(&stride)) || FAILED(lk->GetDataPointer(&size, &data))) {
      lk->Release();
      return false;
    }
    for (UINT y = 0; y < fh; y++) {
      const uint8_t* s = fbuf.data() + (size_t)y * fw * 4;
      uint8_t* d = data + (size_t)y * stride;
      for (UINT x = 0; x < fw; x++, s += 4, d += 4) {
        uint32_t sa = s[3], da = d[3];
        uint32_t oa = sa + da * (255 - sa) / 255;
        for (int c = 0; c < 3; c++) {
          uint32_t out = s[c] + d[c] * (255 - sa) / 255;
          d[c] = (uint8_t)std::min(255u, out);
        }
        d[3] = (uint8_t)std::min(255u, oa);
      }
    }
    lk->Release();
    return true;
  }

  std::vector<Frame> frames_;
  UINT w_ = 0, h_ = 0;
  size_t current_ = 0;
  ULONGLONG frameStart_ = 0;
};
