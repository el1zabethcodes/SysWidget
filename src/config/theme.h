// Theming: all colors / radii / fonts live in %APPDATA%\SysWidget\theme.json
// (optional — sane Win11 defaults when absent).
#pragma once
#include "common.h"
#include "jsonlite.h"

#include <string>

struct RenderTheme : layout::ITheme {
  uint32_t textPrimary = 0xECFFFFFF, textSecondary = 0x9EFFFFFF;
  uint32_t accent = 0xFF4CC2FF, warn = 0xFFFFD84D, critical = 0xFFFF5F5F;
  uint32_t windowBg = 0xCC1F1F23;
  uint32_t cardBg = 0x14FFFFFF, cardStroke = 0x17FFFFFF, track = 0x24FFFFFF;
  uint32_t agentWorking = 0xFF38BDF8, agentDone = 0xFF22C55E, agentAttention = 0xFFEF4444;
  uint32_t agentCardBg = 0x1AFFFFFF;
  float radiusWindow = 8.f, radiusCards = 6.f;
  std::wstring fontFamily = L"Segoe UI Variable Text";
  double fontSize = 12.0;
  bool light = false;

  D2D1_COLOR_F c(uint32_t argb) const { return ArgbToColorLocal(argb); }

  static D2D1_COLOR_F ArgbToColorLocal(uint32_t c) {
    return D2D1_COLOR_F{(float)((c >> 16) & 0xFF) / 255.f, (float)((c >> 8) & 0xFF) / 255.f,
                        (float)(c & 0xFF) / 255.f, (float)((c >> 24) & 0xFF) / 255.f};
  }

  static uint32_t hex(const Json& j, const char* key, uint32_t def) {
    uint32_t v;
    if (ParseHexColor(Utf8ToWide(j.getStr(key, "")), v)) return v;
    return def;
  }

  static RenderTheme load() {
    RenderTheme t;
    t.light = IsLightTheme();
    if (t.light) {
      t.textPrimary = 0xE6000000;
      t.textSecondary = 0x87000000;
      t.cardBg = 0x0F000000;
      t.cardStroke = 0x12000000;
      t.track = 0x1E000000;
      t.windowBg = 0xCCF6F6F6;
      t.agentCardBg = 0x14000000;
    }
    if (t.light) t.agentWorking = 0xFF0EA5E9;
    t.accent = GetAccentArgb() | 0xFF000000u;

    HANDLE h = CreateFileW(themePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return t;
    std::string data;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) data.append(buf, n);
    CloseHandle(h);
    Json root;
    std::string err;
    if (!JsonParse(data, root, err) || root.type != Json::OBJ) return t;

    t.textPrimary = hex(root, "text_primary", t.textPrimary);
    t.textSecondary = hex(root, "text_secondary", t.textSecondary);
    t.accent = hex(root, "accent", t.accent);
    t.warn = hex(root, "warn", t.warn);
    t.critical = hex(root, "critical", t.critical);
    t.windowBg = hex(root, "background_color", t.windowBg);
    t.cardBg = hex(root, "card_background", t.cardBg);
    t.cardStroke = hex(root, "card_stroke", t.cardStroke);
    t.track = hex(root, "track", t.track);
    t.radiusWindow = (float)root.getNum("corner_radius_window", t.radiusWindow);
    t.radiusCards = (float)root.getNum("corner_radius_cards", t.radiusCards);
    t.fontFamily = Utf8ToWide(root.getStr("font_family", WideToUtf8(t.fontFamily)));
    t.fontSize = root.getNum("font_size", t.fontSize);
    if (root.has("agents") && root.find("agents")->type == Json::OBJ) {
      const Json& a = *root.find("agents");
      t.agentWorking = hex(a, "working", t.agentWorking);
      t.agentDone = hex(a, "done", t.agentDone);
      t.agentAttention = hex(a, "attention", t.agentAttention);
      t.agentCardBg = hex(a, "card_background", t.agentCardBg);
    }
    return t;
  }

  static std::wstring themePath() { return ConfigDir() + L"\\theme.json"; }
};
