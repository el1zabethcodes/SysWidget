// SysWidget — lightweight native Win11 taskbar resource widget (CPU/RAM)
// with a local control pipe for MCP / external agents.
// Header-only common utilities (single translation unit build).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr const wchar_t* kPipeName   = L"\\\\.\\pipe\\SysWidget";
constexpr const wchar_t* kMutexName  = L"Local\\SysWidget.SingleInstance";
constexpr const wchar_t* kAppTitle   = L"SysWidget";
constexpr const char*    kVersion    = "1.1.0";
constexpr const wchar_t* kRunKey     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue   = L"SysWidget";
constexpr const wchar_t* kTaskbarAdv = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

// DWM attributes (guarded for older SDKs)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x02000000L
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Internal window messages
#define WM_APP_PIPE_DIRTY (WM_APP + 1)
#define WM_APP_TRAY       (WM_APP + 2)
#define WM_APP_HIDE       (WM_APP + 3)
#define WM_APP_SHOW       (WM_APP + 4)
#define WM_APP_EXIT       (WM_APP + 5)
#define WM_APP_CFGCHANGED (WM_APP + 6)
#define WM_APP_RAISE      (WM_APP + 7)

// ---------------------------------------------------------------------------
// Strings / conversions
// ---------------------------------------------------------------------------
inline std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)std::max(n, 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)std::max(n, 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

inline std::wstring WFmt(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, fmt);
  int n = _vsnwprintf(buf, 1023, fmt, args);
  va_end(args);
  if (n < 0) n = 0;
  buf[n] = 0;
  return std::wstring(buf, (size_t)n);
}

inline std::string StrFmt(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, 1023, fmt, args);
  va_end(args);
  if (n < 0) n = 0;
  buf[n] = 0;
  return std::string(buf, (size_t)n);
}

inline std::wstring WTrim(std::wstring s) {
  while (!s.empty() && iswspace((wint_t)s.front())) s.erase(s.begin());
  while (!s.empty() && iswspace((wint_t)s.back())) s.pop_back();
  return s;
}

inline std::wstring& WReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::wstring::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

inline bool ParseHexColor(const std::wstring& hex, uint32_t& argb) {
  std::wstring h = hex;
  if (!h.empty() && h[0] == L'#') h.erase(h.begin());
  if (h.empty()) return false;
  for (wchar_t c : h)
    if (!iswxdigit((wint_t)c)) return false;
  auto nib = [](wchar_t c) -> uint32_t {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return c - L'A' + 10;
  };
  if (h.size() == 3) {
    uint32_t r = nib(h[0]) * 17, g = nib(h[1]) * 17, b = nib(h[2]) * 17;
    argb = 0xFF000000u | (r << 16) | (g << 8) | b;
    return true;
  }
  if (h.size() == 6) {
    uint32_t r = (nib(h[0]) << 4) | nib(h[1]);
    uint32_t g = (nib(h[2]) << 4) | nib(h[3]);
    uint32_t b = (nib(h[4]) << 4) | nib(h[5]);
    argb = 0xFF000000u | (r << 16) | (g << 8) | b;
    return true;
  }
  if (h.size() == 8) {
    argb = 0;
    for (int i = 0; i < 8; i++) argb = (argb << 4) | nib(h[i]);
    if ((argb >> 24) == 0) argb |= 0xFF000000u;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Registry / paths / autostart
// ---------------------------------------------------------------------------
inline bool RegGetDwordHKCU(const wchar_t* key, const wchar_t* value, DWORD& out) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  DWORD type = 0, size = sizeof(DWORD), v = 0;
  bool ok = RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS && type == REG_DWORD;
  RegCloseKey(k);
  if (ok) out = v;
  return ok;
}

inline bool RegSetDwordHKCU(const wchar_t* key, const wchar_t* value, DWORD v) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
    return false;
  bool ok = RegSetValueExW(k, value, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
  RegCloseKey(k);
  return ok;
}

inline bool RegDeleteValueHKCU(const wchar_t* key, const wchar_t* value) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
  bool ok = RegDeleteValueW(k, value) == ERROR_SUCCESS;
  RegCloseKey(k);
  return ok;
}

inline std::wstring GetRegStringHKCU(const wchar_t* key, const wchar_t* value) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"";
  wchar_t buf[1024]; DWORD size = sizeof(buf), type = 0;
  std::wstring out;
  if (RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
    size_t len = size / sizeof(wchar_t);
    if (buf[len - 1] == 0 && len > 0) len--;
    out.assign(buf, len);
  }
  RegCloseKey(k);
  return out;
}

inline std::wstring ConfigDir() {
  wchar_t path[MAX_PATH];
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, path)))
    return L"";
  return std::wstring(path) + L"\\SysWidget";
}

inline std::wstring ConfigPath() { return ConfigDir() + L"\\config.json"; }

inline bool EnsureConfigDir() {
  std::wstring d = ConfigDir();
  if (d.empty()) return false;
  CreateDirectoryW(d.c_str(), nullptr);
  DWORD attr = GetFileAttributesW(d.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool IsAutostartEnabled() {
  std::wstring v = GetRegStringHKCU(kRunKey, kRunValue);
  return !v.empty();
}

inline bool SetAutostart(bool on) {
  if (on) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
      return false;
    bool ok = RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                             (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
  }
  RegDeleteValueHKCU(kRunKey, kRunValue);
  return true;
}

inline bool IsLightTheme() {
  DWORD v = 1;
  if (!RegGetDwordHKCU(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"SystemUsesLightTheme", v))
    return false;
  return v == 1;
}

inline uint32_t GetAccentArgb() {
  DWORD v = 0;
  if (RegGetDwordHKCU(L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", v)) {
    uint32_t a = (v >> 24) & 0xFF, r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    if (a < 0x40) a = 0xFF; // fully/semi transparent system value -> opaque
    return (a << 24) | (r << 16) | (g << 8) | b;
  }
  return 0xFF4CC2FF; // Win11 default accent (light blue)
}

// ---------------------------------------------------------------------------
// Console helpers (GUI-subsystem binary that still talks to a terminal)
// ---------------------------------------------------------------------------
inline void CliWrite(HANDLE h, const std::wstring& line) {
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  if (WriteConsoleW(h, (const void*)(line.c_str()), (DWORD)line.size(), &n, nullptr))
    return;
  // redirected/pipe (e.g. Git Bash): emit UTF-8 instead
  std::string u8 = WideToUtf8(line);
  WriteFile(h, u8.data(), (DWORD)u8.size(), &n, nullptr);
}

inline void CliPrint(const std::wstring& line) {
  static bool attached = false;
  if (!attached) { AttachConsole(ATTACH_PARENT_PROCESS); attached = true; }
  CliWrite(GetStdHandle(STD_OUTPUT_HANDLE), line + L"\n");
}

inline void CliErr(const std::wstring& line) {
  static bool attached = false;
  if (!attached) { AttachConsole(ATTACH_PARENT_PROCESS); attached = true; }
  CliWrite(GetStdHandle(STD_ERROR_HANDLE), L"[syswidget] " + line + L"\n");
}

inline ULONGLONG NowMs() { return GetTickCount64(); }

// ---------------------------------------------------------------------------
// Startup/render trace, enabled via SYSWIDGET_TRACE=1 (writes %TEMP% log)
// ---------------------------------------------------------------------------
inline void Trace(const char* what, long a = 0, long b = 0) {
  if (GetEnvironmentVariableA("SYSWIDGET_TRACE", nullptr, 0) == 0) return;
  static SRWLOCK tl = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&tl);
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  HANDLE h = CreateFileW((std::wstring(tmp) + L"syswidget_trace.log").c_str(),
                         FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    char line[256];
    int n = snprintf(line, sizeof line, "[%9llu] %s %ld %ld\n",
                     (unsigned long long)GetTickCount64(), what, a, b);
    DWORD w;
    WriteFile(h, line, n, &w, nullptr);
    CloseHandle(h);
  }
  ReleaseSRWLockExclusive(&tl);
}
