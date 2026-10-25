// Hierarchical layout engine: elements implement Measure / Arrange / Render
// (ILayoutElement), containers distribute space via weights; all geometry is
// computed from the current window size and dpiScale * userScale — no
// hardcoded pixel positions.
#pragma once
#include "common.h"
#include "graphics/wic_gif.h"
#include "ipc/agents.h"

#include <d2d1.h>
#include <dwrite.h>
#include <concepts>
#include <memory>

namespace layout {

struct LayoutRect {
  float x = 0, y = 0, w = 0, h = 0;
};

// RenderTheme is defined in config/theme.h; forward-use via template-free base.
struct ITheme {
  virtual ~ITheme() = default;
};

class Element {
 public:
  virtual ~Element() = default;
  // Ideal size within available space (scale = dpiScale * userScale).
  virtual D2D1_SIZE_F measure(float availW, float availH, float scale) = 0;
  virtual void arrange(const LayoutRect& r) { rect_ = r; }
  virtual void render(ID2D1RenderTarget* rt, ITheme& theme, float scale) = 0;
  const LayoutRect& rect() const { return rect_; }

  float weight = 0;   // main-axis flex share (0 = wrap/fixed)
  bool stretchCross = true;
  float userW = 0, userH = 0;  // fixed px @96dpi when > 0

 protected:
  LayoutRect rect_{};
};

template <class T>
concept LayoutElement = requires(T e, float a, float b, float c, ID2D1RenderTarget* rt,
                                 ITheme& th, const LayoutRect& r) {
  { e.measure(a, b, c) } -> std::same_as<D2D1_SIZE_F>;
  { e.arrange(r) } -> std::same_as<void>;
  { e.render(rt, th, c) } -> std::same_as<void>;
};
static_assert(LayoutElement<Element>);

using Ptr = std::unique_ptr<Element>;

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------
class Stack : public Element {
 public:
  explicit Stack(bool vertical, float spacing = 0) : vertical_(vertical), spacing_(spacing) {}
  Element& add(Ptr e) {
    Element& ref = *e;
    children_.push_back(std::move(e));
    return ref;  // returns the CHILD so .weight/.stretchCross configure it
  }
  Stack& padding(float l, float t, float r, float b) {
    pad_ = {l, t, r, b};
    return *this;
  }

  D2D1_SIZE_F measure(float availW, float availH, float scale) override {
    float main = 0, cross = 0;
    float availMain = vertical_ ? availH - (pad_.top + pad_.bottom) * scale
                                : availW - (pad_.left + pad_.right) * scale;
    float availCross = vertical_ ? availW - (pad_.left + pad_.right) * scale
                                 : availH - (pad_.top + pad_.bottom) * scale;
    size_t flex = 0;
    for (auto& c : children_) {
      D2D1_SIZE_F m = vertical_ ? c->measure(availCross, availMain, scale)
                                : c->measure(availMain, availCross, scale);
      if (c->weight > 0) {
        flex++;
        continue;
      }
      main += vertical_ ? m.height : m.width;
      cross = std::max(cross, vertical_ ? m.width : m.height);
    }
    if (!children_.empty()) main += spacing_ * (float)(children_.size() - 1) * scale;
    // flex children split the remaining main axis; reserve nothing here
    float padW = (pad_.left + pad_.right) * scale;
    float padH = (pad_.top + pad_.bottom) * scale;
    return {std::max(0.f, (vertical_ ? cross : main) + padW),
            std::max(0.f, (vertical_ ? main : cross) + padH)};
  }

  void arrange(const LayoutRect& r) override {
    rect_ = r;
    float scale = scale_;
    float innerX = r.x + pad_.left * scale, innerY = r.y + pad_.top * scale;
    float innerW = std::max(0.f, r.w - (pad_.left + pad_.right) * scale);
    float innerH = std::max(0.f, r.h - (pad_.top + pad_.bottom) * scale);
    float main = vertical_ ? innerH : innerW;
    float cross = vertical_ ? innerW : innerH;

    // pass 1: fixed children ideal sizes
    float used = 0;
    size_t flex = 0;
    std::vector<D2D1_SIZE_F> sizes(children_.size());
    for (size_t i = 0; i < children_.size(); i++) {
      auto& c = children_[i];
      if (c->weight > 0) {
        flex++;
        continue;
      }
      D2D1_SIZE_F m = vertical_ ? c->measure(cross, main, scale)
                                : c->measure(main, cross, scale);
      float w = vertical_ ? std::min(m.width, cross) : m.width;
      float h = vertical_ ? m.height : std::min(m.height, cross);
      sizes[i] = {w, h};
      used += vertical_ ? h : w;
    }
    if (!children_.empty()) used += spacing_ * (float)(children_.size() - 1) * scale;
    float flexEach = flex > 0 ? std::max(0.f, main - used) / (float)flex : 0;

    float pos = vertical_ ? innerY : innerX;
    for (size_t i = 0; i < children_.size(); i++) {
      auto& c = children_[i];
      float along;
      if (c->weight > 0) {
        float sum = 0;
        for (auto& q : children_)
          if (q->weight > 0) sum += q->weight;
        along = sum > 0 ? std::max(0.f, main - used) * (c->weight / sum) : 0;
      } else {
        along = vertical_ ? sizes[i].height : sizes[i].width;
      }
      float across = cross;
      LayoutRect cr;
      if (vertical_) {
        float w = c->stretchCross ? across : std::min(across, sizes[i].width);
        cr = {innerX + (c->stretchCross ? 0 : (across - w) / 2), pos, w, along};
      } else {
        float h = c->stretchCross ? across : std::min(across, sizes[i].height);
        cr = {pos, innerY + (c->stretchCross ? 0 : (across - h) / 2), along, h};
      }
      c->arrange(cr);
      pos += along + spacing_ * scale;
    }
  }

  void render(ID2D1RenderTarget* rt, ITheme& theme, float scale) override {
    for (auto& c : children_) c->render(rt, theme, scale);
  }

  void setScale(float s) { scale_ = s; }
  size_t count() const { return children_.size(); }
  Element* child(size_t i) const { return children_[i].get(); }

 private:
  bool vertical_;
  float spacing_;
  struct Pad {
    float left, top, right, bottom;
  } pad_{};
  std::vector<Ptr> children_;
  float scale_ = 1.f;
};

using VStack = Stack;
using HStack = Stack;

inline std::unique_ptr<Stack> makeV(float spacing = 0) {
  return std::make_unique<Stack>(true, spacing);
}
inline std::unique_ptr<Stack> makeH(float spacing = 0) {
  return std::make_unique<Stack>(false, spacing);
}

// ---------------------------------------------------------------------------
// Leaf elements
// ---------------------------------------------------------------------------
class Text : public Element {
 public:
  Text(std::wstring t, IDWriteTextFormat* fmt, uint32_t argb, bool alignRight = false)
      : text_(std::move(t)), fmt_(fmt), color_(argb), right_(alignRight) {}
  D2D1_SIZE_F measure(float, float, float scale) override {
    if (!fmt_) return {0, 0};
    Com4Layout lay;
    if (SUCCEEDED(dwrite()->CreateTextLayout(text_.c_str(), (UINT32)text_.size(), fmt_, 4096.f,
                                             512.f, &lay))) {
      DWRITE_TEXT_METRICS m{};
      lay->GetMetrics(&m);
      return {m.widthIncludingTrailingWhitespace, m.height};
    }
    return {(float)text_.size() * 7.f * scale, 18.f * scale};
  }
  void render(ID2D1RenderTarget* rt, ITheme&, float scale) override {
    if (!fmt_ || text_.empty() || !brush_) return;
    brush_->SetColor(toColor(color_));
    D2D1_RECT_F r = {rect_.x, rect_.y, rect_.x + rect_.w, rect_.y + rect_.h};
    if (right_) {
      D2D1_SIZE_F m = measure(rect_.w, rect_.h, scale);
      if (m.width < rect_.w) r.left = r.right - m.width;
    }
    rt->DrawTextW(text_.c_str(), (UINT32)text_.size(), fmt_, r, brush_);
  }
  void setBrush(ID2D1SolidColorBrush* b) { brush_ = b; }
  static void setFactory(IDWriteFactory* f, D2D1_COLOR_F (*cvt)(uint32_t)) {
    dw_ = f;
    cvt_ = cvt;
  }
  static void setSharedBrush(ID2D1SolidColorBrush* b) { sharedBrush_ = b; }
  void setText(const std::wstring& t) { text_ = t; }

 private:
  struct Com4Layout {
    IDWriteTextLayout* p = nullptr;
    ~Com4Layout() {
      if (p) p->Release();
    }
    IDWriteTextLayout** operator&() {
      if (p) { p->Release(); p = nullptr; }
      return &p;
    }
    IDWriteTextLayout* operator->() const { return p; }
  };
  static IDWriteFactory* dwrite() { return dw_; }
  D2D1_COLOR_F toColor(uint32_t c) const { return cvt_ ? cvt_(c) : D2D1_COLOR_F{1, 1, 1, 1}; }

  std::wstring text_;
  IDWriteTextFormat* fmt_ = nullptr;
  uint32_t color_;
  bool right_;
  ID2D1SolidColorBrush* brush_ = sharedBrush_;
  static inline IDWriteFactory* dw_ = nullptr;
  static inline D2D1_COLOR_F (*cvt_)(uint32_t) = nullptr;
  static inline ID2D1SolidColorBrush* sharedBrush_ = nullptr;
};

class Bar : public Element {
 public:
  Bar(double value, uint32_t fill, uint32_t track, float thickness = 4)
      : value_(value), fill_(fill), track_(track), th_(thickness) {}
  D2D1_SIZE_F measure(float, float, float scale) override { return {28.f * scale, th_ * scale}; }
  void render(ID2D1RenderTarget* rt, ITheme&, float) override {
    if (!brush_) return;
    float h = std::max(2.f, th_ * scaleRef());
    float r = h / 2;
    float x = rect_.x, y = rect_.y + (rect_.h - h) / 2, w = rect_.w;
    brush_->SetColor(cvt_(track_));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_);
    double v = std::min(1.0, std::max(0.0, value_));
    if (v > 0.005) {
      float fw = (float)(w * v);
      if (fw < h) fw = h;
      brush_->SetColor(cvt_(fill_));
      rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + fw, y + h), r, r), brush_);
    }
  }
  void setValue(double v) { value_ = v; }
  void setBrush(ID2D1SolidColorBrush* b) { brush_ = b; }
  void setScaleRef(float s) { scaleRef_ = s; }
  static void setCvt(D2D1_COLOR_F (*cvt)(uint32_t)) { cvt_ = cvt; }

 private:
  float scaleRef() const { return scaleRef_ == 0 ? 1 : scaleRef_; }
  double value_;
  uint32_t fill_, track_;
  float th_, scaleRef_ = 0;
  ID2D1SolidColorBrush* brush_ = nullptr;
  static inline D2D1_COLOR_F (*cvt_)(uint32_t) = nullptr;
};

enum class Anchor { TopLeft, TopRight, Center, BottomLeft, BottomRight };

class Gif : public Element {
 public:
  Gif(WicAnimatedGif* gif, float zoom = 1.f, Anchor anchor = Anchor::Center)
      : gif_(gif), zoom_(zoom), anchor_(anchor) {}
  D2D1_SIZE_F measure(float, float, float scale) override {
    if (!gif_ || !gif_->ok()) return {0, 0};
    return {(float)gif_->width() * zoom_ * scale, (float)gif_->height() * zoom_ * scale};
  }
  void render(ID2D1RenderTarget* rt, ITheme&, float scale) override {
    if (!gif_ || !gif_->ok() || rect_.w <= 0 || rect_.h <= 0) return;
    const WicAnimatedGif::Frame& f = gif_->current();
    D2D1_SIZE_F bs = f.bmp->GetSize();
    float k = std::min(rect_.w / bs.width, rect_.h / bs.height);  // fit into rect
    float dw = bs.width * k, dh = bs.height * k;
    float x = rect_.x, y = rect_.y;
    switch (anchor_) {
      case Anchor::TopLeft: break;
      case Anchor::TopRight: x += rect_.w - dw; break;
      case Anchor::Center: x += (rect_.w - dw) / 2; y += (rect_.h - dh) / 2; break;
      case Anchor::BottomLeft: y += rect_.h - dh; break;
      case Anchor::BottomRight: x += rect_.w - dw; y += rect_.h - dh; break;
    }
    rt->DrawBitmap(f.bmp, D2D1::RectF(x, y, x + dw, y + dh), 1.f,
                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
  }
  void setZoom(float z) { zoom_ = z; }

 private:
  WicAnimatedGif* gif_;
  float zoom_;
  Anchor anchor_;
};

class Spacer : public Element {
 public:
  D2D1_SIZE_F measure(float, float, float) override { return {0, 0}; }
  void render(ID2D1RenderTarget*, ITheme&, float) override {}
};

// Agent card: rounded plate + status dot (pulsing/blinking) + name + status text.
class AgentCard : public Element {
 public:
  AgentCard(const AgentState& a, uint32_t cardBg, uint32_t textPrimary, uint32_t textSecondary)
      : a_(a), cardBg_(cardBg), tp_(textPrimary), ts_(textSecondary) {}
  D2D1_SIZE_F measure(float, float, float scale) override { return {240.f * scale, 30.f * scale}; }
  void render(ID2D1RenderTarget* rt, ITheme&, float scale) override {
    if (!brush_) return;
    float r = 5.f * scale;
    D2D1_RECT_F rc = {rect_.x, rect_.y, rect_.x + rect_.w, rect_.y + rect_.h};
    brush_->SetColor(cvt_(cardBg_));
    rt->FillRoundedRectangle(D2D1::RoundedRect(rc, r, r), brush_);
    uint32_t statusColor = colorFor(a_.status);
    brush_->SetColor(cvt_((statusColor & 0xFFFFFF) | (dotAlpha() << 24)));
    float d = 7.f * scale;
    float cx = rc.left + 12.f * scale, cy = (rc.top + rc.bottom) / 2;
    D2D1_ELLIPSE e = {{cx, cy}, d / 2, d / 2};
    rt->FillEllipse(e, brush_);
    // status ring
    brush_->SetColor(cvt_(statusColor));
    rt->DrawEllipse(e, brush_, 1.4f * scale);

    if (fmt_ && fmtSmall_) {
      float tx = cx + d;
      float tw = rc.right - tx - 8.f * scale;
      std::wstring name = toWide(a_.name);
      brush_->SetColor(cvt_(tp_));
      rt->DrawTextW(name.c_str(), (UINT32)name.size(), fmt_,
                    {tx, rc.top, tx + tw, rc.top + rect_.h / 2}, brush_);
      std::wstring st = toWide(a_.statusText);
      brush_->SetColor(cvt_(ts_));
      rt->DrawTextW(st.c_str(), (UINT32)st.size(), fmtSmall_,
                    {tx, rc.top + rect_.h / 2, tx + tw, rc.bottom}, brush_);
    }
  }
  void setBrush(ID2D1SolidColorBrush* b) { brush_ = b; }
  void setFormats(IDWriteTextFormat* name, IDWriteTextFormat* small) {
    fmt_ = name;
    fmtSmall_ = small;
  }
  void setCvt(D2D1_COLOR_F (*cvt)(uint32_t)) { cvt_ = cvt; }
  void setWideCvt(std::wstring (*w)(const std::string&)) { toWide_ = w; }
  void setColors(uint32_t working, uint32_t done, uint32_t attention) {
    working_ = working;
    done_ = done;
    attention_ = attention;
  }
  static float pulseAlpha(ULONGLONG now) {
    double t = (double)(now % 1200) / 1200.0;
    return (float)(0.4 + 0.6 * (0.5 + 0.5 * sin(t * 6.2831853)));
  }

 private:
  std::wstring toWide(const std::string& s) const {
    return toWide_ ? toWide_(s) : std::wstring(s.begin(), s.end());
  }
  uint32_t colorFor(AgentStatus s) const {
    return s == AgentStatus::Done ? done_
           : s == AgentStatus::Attention ? attention_
                                          : working_;
  }
  uint32_t dotAlpha() const {
    ULONGLONG now = GetTickCount64();
    if (a_.status == AgentStatus::Working) return (uint32_t)(pulseAlpha(now) * 255);
    if (a_.status == AgentStatus::Attention) {
      ULONGLONG m = now % 1600;
      return m < 700 ? 255 : 60;  // blink
    }
    return 255;  // done: static
  }
  AgentState a_;
  uint32_t cardBg_, tp_, ts_;
  uint32_t working_ = 0xFF38BDF8, done_ = 0xFF22C55E, attention_ = 0xFFEF4444;
  ID2D1SolidColorBrush* brush_ = nullptr;
  IDWriteTextFormat* fmt_ = nullptr, *fmtSmall_ = nullptr;
  D2D1_COLOR_F (*cvt_)(uint32_t) = nullptr;
  std::wstring (*toWide_)(const std::string&) = nullptr;
};

}  // namespace layout
