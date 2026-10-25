// Shared WIC helpers: imaging factory singleton + any-image-file → D2D bitmap.
#pragma once
#include "common.h"
#include "ui/card_window.h"

#include <wincodec.h>
#include <d2d1.h>
#include <string>

// MinGW sometimes lacks the WIC class GUID symbol; embed the well-known one.
inline const CLSID kWicFactoryClsid = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

template <typename T>
class WicCom {
 public:
  WicCom() = default;
  ~WicCom() { reset(); }
  WicCom(const WicCom&) = delete;
  WicCom& operator=(const WicCom&) = delete;
  T** operator&() { reset(); return &p_; }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  operator T*() const { return p_; }
  void reset() {
    if (p_) { p_->Release(); p_ = nullptr; }
  }

 private:
  T* p_ = nullptr;
};

inline IWICImagingFactory* wicFactory() {
  static IWICImagingFactory* f = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    CoCreateInstance(kWicFactoryClsid, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IWICImagingFactory), (void**)&f);
  }
  return f;
}

// Decode any image file (png/jpg/gif frame/bmp) into a D2D bitmap.
inline bool DecodeImageToD2D(ID2D1RenderTarget* rt, const std::wstring& path,
                             ID2D1Bitmap** out) {
  *out = nullptr;
  IWICImagingFactory* f = wicFactory();
  if (!f || !rt) return false;
  WicCom<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand, &dec)))
    return false;
  WicCom<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return false;
  WicCom<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.f,
                              WICBitmapPaletteTypeCustom)))
    return false;
  D2D1_BITMAP_PROPERTIES bp{};
  bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
  bp.dpiX = bp.dpiY = 96;
  return SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv, &bp, out));
}
