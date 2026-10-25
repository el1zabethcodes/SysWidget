// Widget content model: 2 taskbar lines (CPU/RAM by default), pipe-driven
// overrides (text / animation / script), notify ticker, config persistence.
#pragma once
#include "common.h"
#include "jsonlite.h"
#include "sampler.h"

#include <atomic>
#include <thread>

enum class LineKind { Cpu, Ram, Text, Anim, Script, Hidden };

struct LineSpec {
  LineKind kind = LineKind::Text;
  std::wstring text;          // Text/Anim: template with {tokens}
  std::wstring icon;          // optional glyph prefix (Segoe Fluent Icons)
  uint32_t color = 0;         // optional 0xAARRGGBB override for the value
  double barOverride = -1.0;  // >=0 -> draw mini bar with this 0..1 value
  // Anim
  std::vector<std::wstring> frames;
  ULONGLONG animStart = 0;
  ULONGLONG animIntervalMs = 150;
  // Script
  std::wstring script;
  ULONGLONG scriptIntervalMs = 2000;
  ULONGLONG scriptLastRun = 0;
  // Runtime
  std::wstring scriptOutput;
};

struct NotifyState {
  bool active = false;
  std::wstring text;
  ULONGLONG until = 0;
};

struct Config {
  int width = 156;                    // px @96dpi
  double fontSize = 12.0;             // px @96dpi
  std::wstring align = L"left";       // left | right corner of the taskbar
  std::wstring material = L"card";    // card | flat | acrylic
  std::wstring renderer = L"auto";    // auto | gdi (ULW+D2D) | dcomp
  int bgOpacity = 8;                  // 0..100, card background alpha in %
  bool trayIcon = true;
};

struct SharedState {
  SRWLOCK lock = SRWLOCK_INIT;
  LineSpec lines[2];
  NotifyState note;
  Config cfg;
  SysStats stats{};
  std::deque<double> histCpu, histRam;  // ~90 samples
  uint64_t scriptVersion = 0;
  HWND cardHwnd = nullptr;  // set by UI thread before workers post messages
  std::atomic<bool> dirty{true};
  std::atomic<bool> quit{false};

  SharedState() {
    lines[0].kind = LineKind::Cpu;
    lines[1].kind = LineKind::Ram;
  }
};

inline const wchar_t* LineKindName(LineKind k) {
  switch (k) {
    case LineKind::Cpu: return L"cpu";
    case LineKind::Ram: return L"ram";
    case LineKind::Text: return L"text";
    case LineKind::Anim: return L"anim";
    case LineKind::Script: return L"script";
    case LineKind::Hidden: return L"hidden";
  }
  return L"text";
}

inline bool LineKindFromName(const std::string& s, LineKind& k) {
  if (s == "cpu") { k = LineKind::Cpu; return true; }
  if (s == "ram") { k = LineKind::Ram; return true; }
  if (s == "text") { k = LineKind::Text; return true; }
  if (s == "anim") { k = LineKind::Anim; return true; }
  if (s == "script") { k = LineKind::Script; return true; }
  if (s == "hidden") { k = LineKind::Hidden; return true; }
  return false;
}

// Token expansion for text templates: {cpu} {ram} {ramUsed} {ramTotal} {time}
inline std::wstring ExpandTokens(std::wstring s, const SysStats& st) {
  wchar_t b[64];
  swprintf(b, 64, L"%.0f", st.cpuPercent);  WReplaceAll(s, L"{cpu}", b);
  swprintf(b, 64, L"%.0f", st.ramPercent);  WReplaceAll(s, L"{ram}", b);
  swprintf(b, 64, L"%.1f", st.ramUsedGB);   WReplaceAll(s, L"{ramUsed}", b);
  swprintf(b, 64, L"%.0f", st.ramTotalGB);  WReplaceAll(s, L"{ramTotal}", b);
  SYSTEMTIME t; GetLocalTime(&t);
  swprintf(b, 64, L"%02d:%02d", t.wHour, t.wMinute); WReplaceAll(s, L"{time}", b);
  return s;
}

// ---------------------------------------------------------------------------
// Config persistence (%APPDATA%\SysWidget\config.json)
// ---------------------------------------------------------------------------
inline void SerializeLines(const LineSpec lines[2], Json& out) {
  out = Json::a();
  for (int i = 0; i < 2; i++) {
    const LineSpec& L = lines[i];
    Json j = Json::o().set("kind", WideToUtf8(LineKindName(L.kind)));
    if (!L.text.empty()) j.set("text", WideToUtf8(L.text));
    if (!L.icon.empty()) j.set("icon", WideToUtf8(L.icon));
    if (L.color) {
      char c[16];
      snprintf(c, sizeof c, "#%08X", (unsigned)L.color);
      j.set("color", c);
    }
    if (L.barOverride >= 0) j.set("bar", L.barOverride);
    if (L.kind == LineKind::Anim) {
      Json fr = Json::a();
      for (auto& f : L.frames) fr.push(Json::s(f));
      j.set("frames", fr).set("intervalMs", (double)L.animIntervalMs);
    }
    if (L.kind == LineKind::Script)
      j.set("command", WideToUtf8(L.script)).set("intervalMs", (double)L.scriptIntervalMs);
    out.push(j);
  }
}

inline void SaveConfig(SharedState& st) {
  if (!EnsureConfigDir()) return;
  AcquireSRWLockShared(&st.lock);
  Json cfg = Json::o();
  cfg.set("width", (double)st.cfg.width)
     .set("fontSize", st.cfg.fontSize)
     .set("align", WideToUtf8(st.cfg.align))
     .set("material", WideToUtf8(st.cfg.material))
     .set("renderer", WideToUtf8(st.cfg.renderer))
     .set("bgOpacity", (double)st.cfg.bgOpacity)
     .set("trayIcon", st.cfg.trayIcon);
  Json lines;
  SerializeLines(st.lines, lines);
  Json root = Json::o().set("version", kVersion).set("config", cfg).set("lines", lines);
  std::string data = root.dump();
  ReleaseSRWLockShared(&st.lock);
  HANDLE h = CreateFileW(ConfigPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD w = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
  }
}

inline void DeserializeLine(const Json& j, LineSpec& L) {
  LineKind k;
  if (LineKindFromName(j.getStr("kind", "text"), k)) L.kind = k;
  L.text = Utf8ToWide(j.getStr("text", ""));
  L.icon = Utf8ToWide(j.getStr("icon", ""));
  uint32_t c;
  if (ParseHexColor(Utf8ToWide(j.getStr("color", "")), c)) L.color = c;
  const Json* bar = j.find("bar");
  if (bar && bar->type == Json::NUM) L.barOverride = std::min(1.0, std::max(0.0, bar->num));
  const Json* fr = j.find("frames");
  if (fr && fr->type == Json::ARR && !fr->arr.empty()) {
    L.frames.clear();
    for (auto& f : fr->arr)
      if (f.type == Json::STR) L.frames.push_back(Utf8ToWide(f.str));
    L.animStart = GetTickCount64();
  }
  if (j.has("intervalMs")) L.animIntervalMs = (ULONGLONG)std::max(30.0, j.getNum("intervalMs", 150));
  L.script = Utf8ToWide(j.getStr("command", ""));
  if (j.has("intervalMs") && L.kind == LineKind::Script)
    L.scriptIntervalMs = (ULONGLONG)std::max(100.0, j.getNum("intervalMs", 2000));
}

inline void LoadConfig(SharedState& st) {
  HANDLE h = CreateFileW(ConfigPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    SaveConfig(st);  // write defaults for discoverability
    return;
  }
  std::string data;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) data.append(buf, n);
  CloseHandle(h);
  Json root;
  std::string err;
  if (!JsonParse(data, root, err) || root.type != Json::OBJ) return;
  const Json* cfg = root.find("config");
  if (cfg && cfg->type == Json::OBJ) {
    if (cfg->has("width"))
      st.cfg.width = (int)std::min(280.0, std::max(96.0, cfg->getNum("width", 156)));
    if (cfg->has("fontSize"))
      st.cfg.fontSize = std::min(20.0, std::max(9.0, cfg->getNum("fontSize", 12)));
    if (cfg->has("align")) st.cfg.align = Utf8ToWide(cfg->getStr("align", "left"));
    if (cfg->has("material")) st.cfg.material = Utf8ToWide(cfg->getStr("material", "card"));
    if (cfg->has("renderer")) st.cfg.renderer = Utf8ToWide(cfg->getStr("renderer", "auto"));
    if (cfg->has("bgOpacity"))
      st.cfg.bgOpacity = (int)std::min(100.0, std::max(0.0, cfg->getNum("bgOpacity", 8)));
    if (cfg->has("trayIcon")) st.cfg.trayIcon = cfg->getBool("trayIcon", true);
  }
  const Json* lines = root.find("lines");
  if (lines && lines->type == Json::ARR) {
    int i = 0;
    for (auto& lj : lines->arr) {
      if (i > 1) break;
      LineSpec L;
      L.animStart = GetTickCount64();
      DeserializeLine(lj, L);
      st.lines[i++] = std::move(L);
    }
  }
  st.scriptVersion++;  // let the script runner pick up persisted scripts
}

// ---------------------------------------------------------------------------
// Capture stdout of a shell command without flashing a console window.
// ---------------------------------------------------------------------------
inline bool RunCapture(const std::wstring& cmd, std::string& out, DWORD timeoutMs = 8000) {
  out.clear();
  SECURITY_ATTRIBUTES sa{(DWORD)sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::wstring cl = L"cmd.exe /Q /c " + cmd;
  std::vector<wchar_t> clbuf(cl.begin(), cl.end());
  clbuf.push_back(L'\0');
  BOOL okc = CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!okc) { CloseHandle(rd); return false; }
  CloseHandle(pi.hThread);
  ULONGLONG t0 = GetTickCount64();
  char rb[4096];
  DWORD got = 0;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
    if (avail == 0) {
      if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
               ReadFile(rd, rb, sizeof rb, &got, nullptr) && got > 0) {
          out.append(rb, got);
          if (out.size() > 8192) break;
        }
        break;
      }
      if (GetTickCount64() - t0 > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
      continue;
    }
    DWORD want = avail < sizeof rb ? avail : (DWORD)sizeof rb;
    if (!ReadFile(rd, rb, want, &got, nullptr) || got == 0) break;
    out.append(rb, got);
    if (out.size() > 8192) { TerminateProcess(pi.hProcess, 1); break; }
  }
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, 2000);
  CloseHandle(pi.hProcess);
  return true;
}

// First line of captured output, decoded as UTF-8 (fallback: OEM codepage).
inline std::wstring RunCaptureLine(const std::wstring& cmd) {
  std::string out8;
  if (!RunCapture(cmd, out8)) return L"";
  std::string first;
  for (char c : out8) {
    if (c == '\n') break;
    first += c;
  }
  while (!first.empty() && first.back() == '\r') first.pop_back();
  bool valid = true;
  for (size_t i = 0; i < first.size() && valid; i++) {
    unsigned char c = (unsigned char)first[i];
    if (c < 0x80) continue;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || i + (size_t)extra >= first.size()) { valid = false; break; }
    i += extra;
  }
  if (valid) return Utf8ToWide(first);
  int n = MultiByteToWideChar(GetOEMCP(), 0, first.data(), (int)first.size(), nullptr, 0);
  std::wstring w((size_t)std::max(n, 0), L'\0');
  if (n > 0) MultiByteToWideChar(GetOEMCP(), 0, first.data(), (int)first.size(), &w[0], n);
  return w;
}

// ---------------------------------------------------------------------------
// Script runner thread: executes Script-kind lines periodically.
// ---------------------------------------------------------------------------
class ScriptRunner {
 public:
  explicit ScriptRunner(SharedState& st) : st_(st) {}
  void start() { th_ = std::thread([this] { loop(); }); }
  void join() {
    if (th_.joinable()) th_.join();
  }

 private:
  void loop() {
    while (!st_.quit.load(std::memory_order_relaxed)) {
      ULONGLONG now = GetTickCount64();
      struct Job { int idx; std::wstring cmd; };
      std::vector<Job> jobs;
      {
        AcquireSRWLockShared(&st_.lock);
        for (int i = 0; i < 2; i++) {
          const LineSpec& L = st_.lines[i];
          if (L.kind == LineKind::Script && !L.script.empty() &&
              now - L.scriptLastRun >= L.scriptIntervalMs)
            jobs.push_back({i, L.script});
        }
        ReleaseSRWLockShared(&st_.lock);
      }
      for (auto& job : jobs) {
        {
          AcquireSRWLockExclusive(&st_.lock);
          st_.lines[job.idx].scriptLastRun = now;
          ReleaseSRWLockExclusive(&st_.lock);
        }
        std::wstring line = RunCaptureLine(job.cmd);
        AcquireSRWLockExclusive(&st_.lock);
        st_.lines[job.idx].scriptOutput = line;
        ReleaseSRWLockExclusive(&st_.lock);
        st_.dirty.store(true, std::memory_order_relaxed);
      }
      for (int i = 0; i < 10 && !st_.quit.load(std::memory_order_relaxed); i++) Sleep(100);
    }
  }

  SharedState& st_;
  std::thread th_;
};

// ---------------------------------------------------------------------------
// Display text for a line (used by "get", MCP status dumps and the renderer).
// ---------------------------------------------------------------------------
inline std::wstring LineViewText(const LineSpec& L, const SysStats& st, ULONGLONG now) {
  switch (L.kind) {
    case LineKind::Cpu:
      return WFmt(L"%.0f%%", st.cpuPercent);
    case LineKind::Ram:
      return WFmt(L"%.0f%%  %.1f/%.0fG", st.ramPercent, st.ramUsedGB, st.ramTotalGB);
    case LineKind::Text:
      return ExpandTokens(L.text, st);
    case LineKind::Anim: {
      std::wstring frame;
      if (!L.frames.empty()) {
        ULONGLONG elapsed = now > L.animStart ? now - L.animStart : 0;
        size_t idx = (size_t)((elapsed / (L.animIntervalMs ? L.animIntervalMs : 1)) % L.frames.size());
        frame = L.frames[idx];
      }
      std::wstring rest = ExpandTokens(L.text, st);
      if (frame.empty()) return rest;
      if (rest.empty()) return frame;
      return frame + L" " + rest;
    }
    case LineKind::Script:
      return L.scriptOutput;
    case LineKind::Hidden:
      return L"";
  }
  return L"";
}

inline Json LineToJson(const LineSpec& L, const SysStats& st) {
  Json j = Json::o().set("kind", WideToUtf8(LineKindName(L.kind)));
  j.set("display", WideToUtf8(LineViewText(L, st, GetTickCount64())));
  if (!L.icon.empty()) j.set("icon", WideToUtf8(L.icon));
  if (L.color) {
    char c[16];
    snprintf(c, sizeof c, "#%08X", (unsigned)L.color);
    j.set("color", c);
  }
  if (L.barOverride >= 0) j.set("bar", L.barOverride);
  if (!L.text.empty()) j.set("text", WideToUtf8(L.text));
  if (L.kind == LineKind::Script) {
    j.set("command", WideToUtf8(L.script)).set("intervalMs", (double)L.scriptIntervalMs);
    if (!L.scriptOutput.empty()) j.set("output", WideToUtf8(L.scriptOutput));
  }
  if (L.kind == LineKind::Anim) {
    Json fr = Json::a();
    for (auto& f : L.frames) fr.push(Json::s(f));
    j.set("frames", fr).set("intervalMs", (double)L.animIntervalMs);
  }
  return j;
}

inline void ResetLineToDefault(LineSpec& L, int idx) {
  L = LineSpec{};
  L.kind = idx == 0 ? LineKind::Cpu : LineKind::Ram;
}

inline std::string ErrResp(const char* e) {
  return Json::o().set("ok", false).set("error", e).dump();
}

// ---------------------------------------------------------------------------
// Command application — shared by the named pipe server and the MCP bridge.
// Input:  {"cmd": "...", ...}   Output: {"ok":true,...} | {"ok":false,"error"}
// ---------------------------------------------------------------------------
inline std::string ApplyCommand(SharedState& st, const Json& req) {
  std::string cmd = req.getStr("cmd", "");
  if (cmd.empty()) return ErrResp("missing 'cmd'");

  auto indexArg = [&](int def) -> int {
    int i = (int)req.getNum("index", (double)def);
    return (i == 0 || i == 1) ? i : def;
  };
  auto colorOpt = [&](const char* key, uint32_t& dst, bool& had) -> bool {
    const Json* j = req.find(key);
    if (!j || j->type != Json::STR) return true;
    std::wstring hx = Utf8ToWide(j->str);
    if (hx.empty()) { dst = 0; had = true; return true; }
    uint32_t c;
    if (!ParseHexColor(hx, c)) return false;
    dst = c;
    had = true;
    return true;
  };

  if (cmd == "status") {
    Trace("status: acquiring lock");
    AcquireSRWLockShared(&st.lock);
    Trace("status: lock acquired");
    std::string out = Json::o()
                          .set("ok", true)
                          .set("cpu", st.stats.cpuPercent)
                          .set("ramPct", st.stats.ramPercent)
                          .set("ramUsedGB", st.stats.ramUsedGB)
                          .set("ramTotalGB", st.stats.ramTotalGB)
                          .set("uptimeSec", (double)(st.stats.uptimeMs / 1000))
                          .set("version", kVersion)
                          .dump();
    ReleaseSRWLockShared(&st.lock);
    Trace("status: done");
    return out;
  }

  if (cmd == "get") {
    AcquireSRWLockShared(&st.lock);
    Json lines = Json::a();
    for (int i = 0; i < 2; i++) lines.push(LineToJson(st.lines[i], st.stats));
    Json cfg = Json::o();
    cfg.set("width", (double)st.cfg.width)
       .set("fontSize", st.cfg.fontSize)
       .set("align", WideToUtf8(st.cfg.align))
       .set("material", WideToUtf8(st.cfg.material))
       .set("renderer", WideToUtf8(st.cfg.renderer))
       .set("bgOpacity", (double)st.cfg.bgOpacity)
       .set("trayIcon", st.cfg.trayIcon);
    Json r = Json::o().set("ok", true).set("lines", lines).set("config", cfg);
    if (st.note.active) r.set("notify", WideToUtf8(st.note.text));
    std::string out = r.dump();
    ReleaseSRWLockShared(&st.lock);
    return out;
  }

  if (cmd == "line" || cmd == "lines") {
    AcquireSRWLockExclusive(&st.lock);
    if (cmd == "lines") {
      const Json* texts = req.find("texts");
      if (!texts || texts->type != Json::ARR) {
        ReleaseSRWLockExclusive(&st.lock);
        return ErrResp("'texts' array required");
      }
      size_t i = 0;
      for (auto& t : texts->arr) {
        if (i > 1 || t.type != Json::STR) continue;
        LineSpec L{};
        L.kind = LineKind::Text;
        L.text = Utf8ToWide(t.str);
        st.lines[i++] = std::move(L);
      }
      st.scriptVersion++;
      ReleaseSRWLockExclusive(&st.lock);
      st.dirty.store(true, std::memory_order_relaxed);
      SaveConfig(st);
      return Json::o().set("ok", true).dump();
    }
    int idx = indexArg(0);
    const Json* text = req.find("text");
    if (!text || text->type != Json::STR) {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("'text' string required");
    }
    LineSpec L = st.lines[idx];
    L.kind = LineKind::Text;
    L.frames.clear();
    L.script.clear();
    L.scriptOutput.clear();
    L.text = Utf8ToWide(text->str);
    L.animStart = 0;
    bool had = false;
    if (!colorOpt("color", L.color, had)) {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("bad 'color' (use #RRGGBB)");
    }
    const Json* icon = req.find("icon");
    if (icon && icon->type == Json::STR) L.icon = Utf8ToWide(icon->str);
    const Json* bar = req.find("bar");
    if (bar && bar->type == Json::NUM) L.barOverride = std::min(1.0, std::max(0.0, bar->num));
    if (bar && bar->type == Json::NUL) L.barOverride = -1.0;
    st.lines[idx] = std::move(L);
    st.scriptVersion++;
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "anim") {
    int idx = indexArg(0);
    const Json* frames = req.find("frames");
    if (!frames || frames->type != Json::ARR || frames->arr.empty())
      return ErrResp("'frames' non-empty array required");
    AcquireSRWLockExclusive(&st.lock);
    LineSpec L = st.lines[idx];
    L.kind = LineKind::Anim;
    L.frames.clear();
    for (auto& f : frames->arr)
      if (f.type == Json::STR) L.frames.push_back(Utf8ToWide(f.str));
    if (L.frames.empty()) {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("'frames' must contain strings");
    }
    L.script.clear();
    L.animStart = GetTickCount64();
    L.animIntervalMs = (ULONGLONG)std::max(30.0, req.getNum("intervalMs", 150));
    const Json* text = req.find("text");
    if (text && text->type == Json::STR) L.text = Utf8ToWide(text->str);
    bool had = false;
    if (!colorOpt("color", L.color, had)) {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("bad 'color'");
    }
    st.lines[idx] = std::move(L);
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "script") {
    int idx = indexArg(0);
    std::string command = req.getStr("command", "");
    if (command.empty()) return ErrResp("'command' required");
    AcquireSRWLockExclusive(&st.lock);
    LineSpec L = st.lines[idx];
    L.kind = LineKind::Script;
    L.script = Utf8ToWide(command);
    L.frames.clear();
    L.scriptIntervalMs = (ULONGLONG)std::max(100.0, req.getNum("intervalMs", 2000));
    L.scriptLastRun = 0;
    L.scriptOutput.clear();
    const Json* text = req.find("text");
    if (text && text->type == Json::STR) L.text = Utf8ToWide(text->str);
    const Json* icon = req.find("icon");
    if (icon && icon->type == Json::STR) L.icon = Utf8ToWide(icon->str);
    bool had = false;
    if (!colorOpt("color", L.color, had)) {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("bad 'color'");
    }
    st.lines[idx] = std::move(L);
    st.scriptVersion++;
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "script.remove") {
    int idx = indexArg(0);
    AcquireSRWLockExclusive(&st.lock);
    ResetLineToDefault(st.lines[idx], idx);
    st.scriptVersion++;
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "bar") {
    int idx = indexArg(0);
    const Json* v = req.find("value");
    if (!v) return ErrResp("'value' (0..1 or null) required");
    AcquireSRWLockExclusive(&st.lock);
    if (v->type == Json::NUL) st.lines[idx].barOverride = -1.0;
    else if (v->type == Json::NUM)
      st.lines[idx].barOverride = std::min(1.0, std::max(0.0, v->num));
    else {
      ReleaseSRWLockExclusive(&st.lock);
      return ErrResp("'value' must be number or null");
    }
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "notify") {
    std::string text = req.getStr("text", "");
    if (text.empty()) return ErrResp("'text' required");
    ULONGLONG ms = (ULONGLONG)std::min(60000.0, std::max(300.0, req.getNum("ms", 4000)));
    AcquireSRWLockExclusive(&st.lock);
    st.note.active = true;
    st.note.text = Utf8ToWide(text);
    st.note.until = GetTickCount64() + ms;
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "reset") {
    AcquireSRWLockExclusive(&st.lock);
    if (req.has("index")) {
      int idx = indexArg(0);
      ResetLineToDefault(st.lines[idx], idx);
    } else {
      ResetLineToDefault(st.lines[0], 0);
      ResetLineToDefault(st.lines[1], 1);
    }
    st.note.active = false;
    st.scriptVersion++;
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "config") {
    AcquireSRWLockExclusive(&st.lock);
    if (req.has("width"))
      st.cfg.width = (int)std::min(280.0, std::max(96.0, req.getNum("width", 156)));
    if (req.has("fontSize"))
      st.cfg.fontSize = std::min(20.0, std::max(9.0, req.getNum("fontSize", 12)));
    if (req.has("align")) st.cfg.align = Utf8ToWide(req.getStr("align", "left"));
    if (req.has("material")) st.cfg.material = Utf8ToWide(req.getStr("material", "card"));
    if (req.has("renderer")) st.cfg.renderer = Utf8ToWide(req.getStr("renderer", "auto"));
    if (req.has("bgOpacity"))
      st.cfg.bgOpacity = (int)std::min(100.0, std::max(0.0, req.getNum("bgOpacity", 8)));
    if (req.has("trayIcon")) st.cfg.trayIcon = req.getBool("trayIcon", true);
    ReleaseSRWLockExclusive(&st.lock);
    st.dirty.store(true, std::memory_order_relaxed);
    SaveConfig(st);
    return Json::o().set("ok", true).dump();
  }

  if (cmd == "save") {
    SaveConfig(st);
    return Json::o().set("ok", true).set("path", WideToUtf8(ConfigPath())).dump();
  }

  if (cmd == "hide" || cmd == "show" || cmd == "exit") {
    HWND h = st.cardHwnd;
    if (h) {
      if (cmd == "hide") PostMessage(h, WM_APP_HIDE, 0, 0);
      if (cmd == "show") PostMessage(h, WM_APP_SHOW, 0, 0);
      if (cmd == "exit") PostMessage(h, WM_APP_EXIT, 0, 0);
    }
    if (cmd == "exit") st.quit.store(true, std::memory_order_relaxed);
    return Json::o().set("ok", true).dump();
  }

  return ErrResp("unknown cmd (status|get|line|lines|anim|script|script.remove|bar|notify|reset|config|save|hide|show|exit)");
}
