// Settings window (native Win32 controls, dark/light themed): presets,
// per-line content selection, value colors, width / font / background opacity.
#pragma once
#include "common.h"
#include "content.h"

#include <commctrl.h>
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

enum {
  IDC_LKIND0 = 100, IDC_LTEXT0, IDC_LCMD0, IDC_LINT0, IDC_LBAR0, IDC_LBARV0, IDC_LCOLOR0,
  IDC_LKIND1 = 110, IDC_LTEXT1, IDC_LCMD1, IDC_LINT1, IDC_LBAR1, IDC_LBARV1, IDC_LCOLOR1,
  IDC_WIDTH = 120, IDC_WLAB, IDC_FS, IDC_FLAB, IDC_OPACITY, IDC_OLAB, IDC_MATERIAL,
  IDC_P1 = 130, IDC_P2, IDC_P3, IDC_P4,
  IDC_AUTORUN = 140, IDC_RESET, IDC_CLOSE
};

struct ColorEntry {
  const wchar_t* name;
  uint32_t argb;
};

inline UINT SettingsDpiOf(HWND h) {
  static UINT(WINAPI * pGetDpi)(HWND) = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) pGetDpi = (UINT(WINAPI*)(HWND))GetProcAddress(u, "GetDpiForWindow");
  }
  if (pGetDpi) return pGetDpi(h);
  HDC dc = GetDC(h);
  UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(h, dc);
  return d ? d : 96;
}

class SettingsUI {
 public:
  void open(SharedState* st, HWND anchor) {
    st_ = st;
    anchor_ = anchor;
    if (hwnd_) {
      ShowWindow(hwnd_, SW_SHOWNORMAL);
      SetForegroundWindow(hwnd_);
      return;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    light_ = IsLightTheme();
    dark_ = !light_;
    scale_ = SettingsDpiOf(anchor) / 96.f;

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SysWidgetSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int W = (int)(470 * scale_), H = (int)(584 * scale_);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;

    hwnd_ = CreateWindowExW(0, L"SysWidgetSettings", L"SysWidget — настройки",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, W, H, nullptr,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    BOOL darkTitle = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
    INT round = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));

    bgBrush_ = CreateSolidBrush(light_ ? RGB(249, 249, 249) : RGB(32, 32, 32));
    ctlBrush_ = CreateSolidBrush(light_ ? RGB(255, 255, 255) : RGB(43, 43, 43));
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    hfont_ = CreateFontIndirectW(&ncm.lfMessageFont);

    build();
    refreshControls();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
  }
  void requestClose() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
  bool isOpen() const { return hwnd_ != nullptr; }

 private:
  static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    SettingsUI* self = (SettingsUI*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) {
      if (m == WM_NCCREATE) {
        self = (SettingsUI*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
      } else {
        return DefWindowProcW(h, m, wp, lp);
      }
    }
    return self->handler(m, wp, lp);
  }

  LRESULT handler(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillRect((HDC)wp, &rc, bgBrush_);
        return 1;
      }
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, light_ ? RGB(20, 20, 20) : RGB(241, 241, 241));
        SetBkColor(dc, light_ ? RGB(255, 255, 255) : RGB(43, 43, 43));
        return (LRESULT)ctlBrush_;
      }
      case WM_DRAWITEM:
        drawButton((DRAWITEMSTRUCT*)lp);
        return TRUE;

      case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
          case IDC_LKIND0: case IDC_LCOLOR0:
            if (code == CBN_SELCHANGE) applyLine(0);
            break;
          case IDC_LBAR0:
            if (code == BN_CLICKED) applyLine(0);
            break;
          case IDC_LTEXT0: case IDC_LCMD0: case IDC_LINT0:
            if (code == EN_KILLFOCUS) applyLine(0);
            break;
          case IDC_LKIND1: case IDC_LCOLOR1:
            if (code == CBN_SELCHANGE) applyLine(1);
            break;
          case IDC_LBAR1:
            if (code == BN_CLICKED) applyLine(1);
            break;
          case IDC_LTEXT1: case IDC_LCMD1: case IDC_LINT1:
            if (code == EN_KILLFOCUS) applyLine(1);
            break;
          case IDC_MATERIAL:
            if (code == CBN_SELCHANGE) applyAppearance();
            break;
          case IDC_AUTORUN:
            if (code == BN_CLICKED)
              SetAutostart(IsDlgButtonChecked(hwnd_, IDC_AUTORUN) == BST_CHECKED);
            break;
          case IDC_RESET:
            if (code == BN_CLICKED) doReset();
            break;
          case IDC_CLOSE:
            if (code == BN_CLICKED) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
          case IDC_P1: case IDC_P2: case IDC_P3: case IDC_P4:
            applyPreset(id);
            break;
        }
        return 0;
      }

      case WM_HSCROLL: {
        HWND src = (HWND)lp;
        if (src) {
          int id = GetDlgCtrlID(src);
          if (id == IDC_WIDTH || id == IDC_FS || id == IDC_OPACITY) applyAppearance();
        }
        return 0;
      }

      case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

      case WM_DESTROY: {
        SaveConfig(*st_);
        if (bgBrush_) DeleteObject(bgBrush_);
        if (ctlBrush_) DeleteObject(ctlBrush_);
        if (hfont_) DeleteObject(hfont_);
        bgBrush_ = ctlBrush_ = nullptr;
        hfont_ = nullptr;
        hwnd_ = nullptr;
        return 0;
      }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
  }

  void drawButton(DRAWITEMSTRUCT* d) {
    bool accent = d->CtlID == IDC_CLOSE;
    COLORREF fill = accent ? RGB(0, 103, 192)
                           : (light_ ? RGB(255, 255, 255) : RGB(53, 53, 56));
    if (d->itemState & ODS_SELECTED)
      fill = accent ? RGB(0, 84, 158) : (light_ ? RGB(229, 229, 229) : RGB(70, 70, 74));
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(d->hDC, &d->rcItem, b);
    DeleteObject(b);
    if (!accent) {
      HBRUSH border = CreateSolidBrush(light_ ? RGB(202, 202, 202) : RGB(84, 84, 88));
      FrameRect(d->hDC, &d->rcItem, border);
      DeleteObject(border);
    }
    wchar_t txt[64];
    GetWindowTextW(d->hwndItem, txt, 64);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, accent ? RGB(255, 255, 255)
                                : (light_ ? RGB(20, 20, 20) : RGB(241, 241, 241)));
    DrawTextW(d->hDC, txt, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  HWND mk(DWORD style, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int hpx,
          int id, DWORD ex = 0) {
    HWND ctrl = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                (int)(x * scale_), (int)(y * scale_), (int)(w * scale_),
                                (int)(hpx * scale_), hwnd_, (HMENU)(intptr_t)id,
                                GetModuleHandleW(nullptr), nullptr);
    if (ctrl && hfont_) SendMessageW(ctrl, WM_SETFONT, (WPARAM)hfont_, TRUE);
    if (ctrl && dark_) SetWindowTheme(ctrl, L"DarkMode_Explorer", nullptr);
    return ctrl;
  }

  void build() {
    // ---- presets
    mk(0, L"Static", L"Пресеты отображения", 16, 12, 300, 16, 0);
    mk(BS_OWNERDRAW, L"Button", L"Классика", 16, 32, 104, 30, IDC_P1);
    mk(BS_OWNERDRAW, L"Button", L"Минимализм", 126, 32, 104, 30, IDC_P2);
    mk(BS_OWNERDRAW, L"Button", L"Часы + ресурсы", 236, 32, 118, 30, IDC_P3);
    mk(BS_OWNERDRAW, L"Button", L"Скрипт-пример", 360, 32, 94, 30, IDC_P4);

    // ---- two line panels
    layoutLinePanel(0, 78, L"Строка 1 — верхняя");
    layoutLinePanel(1, 220, L"Строка 2 — нижняя");

    // ---- appearance
    mk(0, L"Static", L"Внешний вид", 16, 364, 200, 16, 0);
    mk(0, L"Static", L"Ширина:", 16, 392, 88, 18, 0);
    mk(TBS_HORZ, TRACKBAR_CLASSW, L"", 108, 386, 240, 26, IDC_WIDTH);
    mk(0, L"Static", L"", 356, 392, 98, 18, IDC_WLAB);
    mk(0, L"Static", L"Шрифт:", 16, 420, 88, 18, 0);
    mk(TBS_HORZ, TRACKBAR_CLASSW, L"", 108, 414, 240, 26, IDC_FS);
    mk(0, L"Static", L"", 356, 420, 98, 18, IDC_FLAB);
    mk(0, L"Static", L"Фон, %:", 16, 448, 88, 18, 0);
    mk(TBS_HORZ, TRACKBAR_CLASSW, L"", 108, 442, 240, 26, IDC_OPACITY);
    mk(0, L"Static", L"", 356, 448, 98, 18, IDC_OLAB);
    mk(0, L"Static", L"Стиль фона:", 16, 480, 88, 18, 0);
    mk(CBS_DROPDOWNLIST, L"Combobox", L"", 108, 476, 150, 120, IDC_MATERIAL);
    SendDlgItemMessageW(hwnd_, IDC_MATERIAL, CB_ADDSTRING, 0, (LPARAM)L"Карточка");
    SendDlgItemMessageW(hwnd_, IDC_MATERIAL, CB_ADDSTRING, 0, (LPARAM)L"Без фона");

    mk(BS_AUTOCHECKBOX, L"Button", L"Запускать вместе с Windows", 16, 508, 280, 20,
       IDC_AUTORUN);
    mk(BS_OWNERDRAW, L"Button", L"Сбросить всё", 16, 536, 150, 32, IDC_RESET);
    mk(BS_OWNERDRAW, L"Button", L"Готово", 304, 536, 150, 32, IDC_CLOSE);

    SendDlgItemMessageW(hwnd_, IDC_WIDTH, TBM_SETRANGE, TRUE, MAKELPARAM(96, 280));
    SendDlgItemMessageW(hwnd_, IDC_FS, TBM_SETRANGE, TRUE, MAKELPARAM(0, 22));
    SendDlgItemMessageW(hwnd_, IDC_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
  }

  void layoutLinePanel(int idx, int y, const wchar_t* title) {
    int base = idx == 0 ? IDC_LKIND0 : IDC_LKIND1;
    mk(0, L"Static", title, 16, y, 240, 16, 0);
    mk(0, L"Static", L"Содержимое:", 16, y + 26, 88, 18, 0);
    mk(CBS_DROPDOWNLIST, L"Combobox", L"", 108, y + 22, 150, 160, base);
    SendDlgItemMessageW(hwnd_, base, CB_ADDSTRING, 0, (LPARAM)L"Процессор (CPU)");
    SendDlgItemMessageW(hwnd_, base, CB_ADDSTRING, 0, (LPARAM)L"Память (RAM)");
    SendDlgItemMessageW(hwnd_, base, CB_ADDSTRING, 0, (LPARAM)L"Текст");
    SendDlgItemMessageW(hwnd_, base, CB_ADDSTRING, 0, (LPARAM)L"Скрипт");
    SendDlgItemMessageW(hwnd_, base, CB_ADDSTRING, 0, (LPARAM)L"Скрытая");
    mk(0, L"Static", L"Цвет:", 272, y + 26, 40, 18, 0);
    mk(CBS_DROPDOWNLIST, L"Combobox", L"", 316, y + 22, 138, 160, base + 6);
    for (auto& c : kColors) SendDlgItemMessageW(hwnd_, base + 6, CB_ADDSTRING, 0, (LPARAM)c.name);
    mk(0, L"Static", L"Текст:", 16, y + 54, 88, 18, 0);
    mk(ES_AUTOHSCROLL, L"Edit", L"", 108, y + 50, 346, 22, base + 1, WS_EX_CLIENTEDGE);
    mk(0, L"Static", L"Скрипт:", 16, y + 82, 88, 18, 0);
    mk(ES_AUTOHSCROLL, L"Edit", L"", 108, y + 78, 346, 22, base + 2, WS_EX_CLIENTEDGE);
    mk(0, L"Static", L"Интервал:", 16, y + 110, 88, 18, 0);
    mk(ES_AUTOHSCROLL | ES_NUMBER, L"Edit", L"", 108, y + 106, 64, 22, base + 3,
       WS_EX_CLIENTEDGE);
    mk(BS_AUTOCHECKBOX, L"Button", L"Мини-бар", 188, y + 107, 84, 20, base + 4);
    mk(TBS_HORZ, TRACKBAR_CLASSW, L"", 276, y + 102, 178, 26, base + 5);
    SendDlgItemMessageW(hwnd_, base + 5, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
  }

  static const ColorEntry kColors[8];

  int kindToIndex(LineKind k) {
    switch (k) {
      case LineKind::Cpu: return 0;
      case LineKind::Ram: return 1;
      case LineKind::Text: return 2;
      case LineKind::Anim: return 2;
      case LineKind::Script: return 3;
      case LineKind::Hidden: return 4;
    }
    return 2;
  }

  int colorToIndex(uint32_t argb) {
    for (int i = 1; i < 8; i++)
      if (kColors[i].argb == argb) return i;
    return 0;
  }

  void refreshControls() {
    AcquireSRWLockShared(&st_->lock);
    for (int idx = 0; idx < 2; idx++) {
      int base = idx == 0 ? IDC_LKIND0 : IDC_LKIND1;
      const LineSpec& L = st_->lines[idx];
      SendDlgItemMessageW(hwnd_, base, CB_SETCURSEL, kindToIndex(L.kind), 0);
      SendDlgItemMessageW(hwnd_, base + 6, CB_SETCURSEL, colorToIndex(L.color), 0);
      SetDlgItemTextW(hwnd_, base + 1, L.text.c_str());
      SetDlgItemTextW(hwnd_, base + 2, L.script.c_str());
      wchar_t b[32];
      swprintf(b, 32, L"%llu", (unsigned long long)L.scriptIntervalMs);
      SetDlgItemTextW(hwnd_, base + 3, b);
      CheckDlgButton(hwnd_, base + 4, L.barOverride >= 0 ? BST_CHECKED : BST_UNCHECKED);
      int bv = L.barOverride >= 0 ? (int)(L.barOverride * 100) : 50;
      SendDlgItemMessageW(hwnd_, base + 5, TBM_SETPOS, TRUE, bv);
    }
    SendDlgItemMessageW(hwnd_, IDC_WIDTH, TBM_SETPOS, TRUE, st_->cfg.width);
    SendDlgItemMessageW(hwnd_, IDC_FS, TBM_SETPOS, TRUE,
                        (int)((st_->cfg.fontSize - 9.0) * 2));
    SendDlgItemMessageW(hwnd_, IDC_OPACITY, TBM_SETPOS, TRUE, st_->cfg.bgOpacity);
    SendDlgItemMessageW(hwnd_, IDC_MATERIAL, CB_SETCURSEL,
                        st_->cfg.material == L"flat" ? 1 : 0, 0);
    ReleaseSRWLockShared(&st_->lock);

    updateLabels();
    CheckDlgButton(hwnd_, IDC_AUTORUN, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
  }

  void updateLabels() {
    wchar_t b[48];
    swprintf(b, 48, L"%d px", (int)SendDlgItemMessageW(hwnd_, IDC_WIDTH, TBM_GETPOS, 0, 0));
    SetDlgItemTextW(hwnd_, IDC_WLAB, b);
    swprintf(b, 48, L"%.1f px", 9.0 + 0.5 * SendDlgItemMessageW(hwnd_, IDC_FS, TBM_GETPOS, 0, 0));
    SetDlgItemTextW(hwnd_, IDC_FLAB, b);
    swprintf(b, 48, L"%d%%", (int)SendDlgItemMessageW(hwnd_, IDC_OPACITY, TBM_GETPOS, 0, 0));
    SetDlgItemTextW(hwnd_, IDC_OLAB, b);
  }

  void commit() {
    SaveConfig(*st_);
    st_->dirty.store(true, std::memory_order_relaxed);
    if (anchor_) PostMessageW(anchor_, WM_APP_CFGCHANGED, 0, 0);
  }

  void applyLine(int idx) {
    int base = idx == 0 ? IDC_LKIND0 : IDC_LKIND1;
    int ksel = (int)SendDlgItemMessageW(hwnd_, base, CB_GETCURSEL, 0, 0);
    if (ksel < 0) return;
    int csel = (int)SendDlgItemMessageW(hwnd_, base + 6, CB_GETCURSEL, 0, 0);
    uint32_t color = (csel > 0) ? kColors[csel].argb : 0;
    bool barOn = IsDlgButtonChecked(hwnd_, base + 4) == BST_CHECKED;
    int barVal = (int)SendDlgItemMessageW(hwnd_, base + 5, TBM_GETPOS, 0, 0);
    double bar = barOn ? std::min(1.0, std::max(0.0, barVal / 100.0)) : -1.0;

    wchar_t txt[512] = L"", cmd[512] = L"";
    GetDlgItemTextW(hwnd_, base + 1, txt, 512);
    GetDlgItemTextW(hwnd_, base + 2, cmd, 512);
    int interval = GetDlgItemInt(hwnd_, base + 3, nullptr, FALSE);
    if (interval <= 0) interval = 2000;
    interval = std::min(60000, std::max(100, interval));

    AcquireSRWLockExclusive(&st_->lock);
    LineSpec& L = st_->lines[idx];
    switch (ksel) {
      case 0:
      case 1: {
        LineKind k = ksel == 0 ? LineKind::Cpu : LineKind::Ram;
        uint32_t keepColor = color;
        ResetLineToDefault(L, idx);
        L.color = keepColor;
        (void)k;
        break;
      }
      case 2:
        L.kind = LineKind::Text;
        L.text = txt;
        L.frames.clear();
        L.script.clear();
        L.scriptOutput.clear();
        L.barOverride = bar;
        L.color = color;
        break;
      case 3:
        L.kind = LineKind::Script;
        L.script = cmd;
        L.scriptIntervalMs = (ULONGLONG)interval;
        L.scriptLastRun = 0;
        L.scriptOutput.clear();
        L.frames.clear();
        L.barOverride = bar;
        L.color = color;
        break;
      default:
        L.kind = LineKind::Hidden;
        L.text.clear();
        L.script.clear();
        L.frames.clear();
        L.scriptOutput.clear();
        L.barOverride = -1.0;
        L.color = 0;
        break;
    }
    st_->scriptVersion++;
    ReleaseSRWLockExclusive(&st_->lock);
    commit();
  }

  void applyAppearance() {
    int width = (int)SendDlgItemMessageW(hwnd_, IDC_WIDTH, TBM_GETPOS, 0, 0);
    double fs = 9.0 + 0.5 * SendDlgItemMessageW(hwnd_, IDC_FS, TBM_GETPOS, 0, 0);
    int op = (int)SendDlgItemMessageW(hwnd_, IDC_OPACITY, TBM_GETPOS, 0, 0);
    int matSel = (int)SendDlgItemMessageW(hwnd_, IDC_MATERIAL, CB_GETCURSEL, 0, 0);
    AcquireSRWLockExclusive(&st_->lock);
    st_->cfg.width = std::min(280, std::max(96, width));
    st_->cfg.fontSize = std::min(20.0, std::max(9.0, fs));
    st_->cfg.bgOpacity = std::min(100, std::max(0, op));
    if (matSel >= 0) st_->cfg.material = matSel == 1 ? L"flat" : L"card";
    ReleaseSRWLockExclusive(&st_->lock);
    updateLabels();
    commit();
  }

  void applyPreset(int id) {
    AcquireSRWLockExclusive(&st_->lock);
    if (id == IDC_P1) {  // Классика: CPU + RAM
      ResetLineToDefault(st_->lines[0], 0);
      ResetLineToDefault(st_->lines[1], 1);
      st_->cfg.width = 156;
      st_->cfg.fontSize = 12;
      st_->cfg.bgOpacity = 8;
      st_->cfg.material = L"card";
    } else if (id == IDC_P2) {  // Минимализм
      LineSpec a;
      a.kind = LineKind::Text;
      a.text = L"CPU {cpu}%";
      LineSpec b;
      b.kind = LineKind::Text;
      b.text = L"RAM {ram}%";
      st_->lines[0] = a;
      st_->lines[1] = b;
      st_->cfg.width = 108;
      st_->cfg.fontSize = 11;
    } else if (id == IDC_P3) {  // Часы + ресурсы
      LineSpec a;
      a.kind = LineKind::Text;
      a.text = L"{time}";
      LineSpec b;
      b.kind = LineKind::Text;
      b.text = L"CPU {cpu}%  RAM {ram}%";
      st_->lines[0] = a;
      st_->lines[1] = b;
      st_->cfg.width = 168;
    } else {  // Скрипт-пример
      ResetLineToDefault(st_->lines[0], 0);
      LineSpec b;
      b.kind = LineKind::Script;
      b.script = L"powershell -NoProfile -Command \"(Get-Date).ToString('HH:mm:ss')\"";
      b.scriptIntervalMs = 2000;
      st_->lines[1] = b;
    }
    st_->note.active = false;
    st_->scriptVersion++;
    ReleaseSRWLockExclusive(&st_->lock);
    refreshControls();
    commit();
  }

  void doReset() {
    AcquireSRWLockExclusive(&st_->lock);
    ResetLineToDefault(st_->lines[0], 0);
    ResetLineToDefault(st_->lines[1], 1);
    bool tray = st_->cfg.trayIcon;
    st_->cfg = Config{};
    st_->cfg.trayIcon = tray;
    st_->note.active = false;
    st_->scriptVersion++;
    ReleaseSRWLockExclusive(&st_->lock);
    refreshControls();
    commit();
  }

  SharedState* st_ = nullptr;
  HWND anchor_ = nullptr;
  HWND hwnd_ = nullptr;
  HBRUSH bgBrush_ = nullptr, ctlBrush_ = nullptr;
  HFONT hfont_ = nullptr;
  bool light_ = false, dark_ = false;
  float scale_ = 1.f;
};

inline const ColorEntry SettingsUI::kColors[8] = {
    {L"Авто (по значению)", 0},
    {L"Белый", 0xFFF2F2F2},
    {L"Зелёный", 0xFF6CCB5F},
    {L"Жёлтый", 0xFFFFD84D},
    {L"Красный", 0xFFFF5F5F},
    {L"Голубой", 0xFF60CDFF},
    {L"Розовый", 0xFFFF7EB6},
    {L"Оранжевый", 0xFFFFB86C},
};
