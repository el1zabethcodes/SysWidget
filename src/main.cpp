// SysWidget — native Win11 taskbar CPU/RAM widget with a local control pipe
// (NDJSON) and an MCP stdio bridge. Single portable executable.
#include "common.h"
#include "jsonlite.h"
#include "sampler.h"
#include "content.h"
#include "pipe.h"
#include "mcp.h"
#include "ui.h"

#include <atomic>

static void PrintUsage() {
  CliPrint(L"SysWidget " + Utf8ToWide(kVersion) +
           L" — нативный виджет CPU/RAM для панели задач Windows 11\n"
           L"\n"
           L"Использование:\n"
           L"  SysWidget.exe                     запустить виджет (2 строки CPU/RAM)\n"
           L"  SysWidget.exe --install           автозапуск + старт сейчас\n"
           L"  SysWidget.exe --uninstall         убрать автозапуск и выйти\n"
           L"  SysWidget.exe --send '<json>'     отправить команду в запущенный виджет\n"
           L"  SysWidget.exe --status            вывести CPU/RAM в формате JSON\n"
           L"  SysWidget.exe --mcp               MCP-сервер (stdio, JSON-RPC)\n"
           L"  SysWidget.exe --restart           перезапустить запущенный виджет\n"
           L"  SysWidget.exe --hide-widgets      скрыть кнопку «Виджеты» на панели задач\n"
           L"  SysWidget.exe --show-widgets      вернуть кнопку «Виджеты»\n"
           L"\n"
           L"Примеры команд --send:\n"
           L"  {\"cmd\":\"status\"}\n"
           L"  {\"cmd\":\"line\",\"index\":0,\"text\":\"deploy | OK\"}\n"
           L"  {\"cmd\":\"anim\",\"index\":0,\"frames\":[\"⠋\",\"⠙\",\"⠹\",\"⠸\"],\"intervalMs\":120}\n"
           L"  {\"cmd\":\"script\",\"index\":1,\"command\":\"powershell -NoProfile -c ...\",\"intervalMs\":5000}\n"
           L"  {\"cmd\":\"notify\",\"text\":\"Сборка завершена\",\"ms\":5000}\n"
           L"  {\"cmd\":\"reset\"}\n"
           L"\n"
           L"Канал управления: \\\\.\\pipe\\SysWidget (NDJSON, UTF-8)");
}

static void RestartExplorer() {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring tmp = L"cmd.exe /Q /c taskkill /f /im explorer.exe & start explorer.exe";
  std::vector<wchar_t> cl(tmp.begin(), tmp.end());
  cl.push_back(L'\0');
  if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static void SetTaskbarWidgets(bool show) {
  RegSetDwordHKCU(kTaskbarAdv, L"TaskbarDa", show ? 1u : 0u);
  RestartExplorer();
  CliPrint(show ? L"Кнопка «Виджеты» включена (TaskbarDa=1), explorer перезапущен."
                : L"Кнопка «Виджеты» скрыта (TaskbarDa=0), explorer перезапущен.");
}

static void SpawnDetachedSelf() {
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cl = std::wstring(L"\"") + exe + L"\"";
  std::vector<wchar_t> buf(cl.begin(), cl.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  for (int i = 0; i < argc; i++) args.push_back(argv[i]);
  LocalFree(argv);

  // ---- CLI argument handling ------------------------------------------------
  std::wstring sendPayload;
  for (size_t i = 0; i < args.size(); i++) {
    const std::wstring& a = args[i];
    if (a == L"--help" || a == L"-h" || a == L"/?") {
      PrintUsage();
      return 0;
    }
    if (a == L"--mcp") return mcp::runBridge();

    if (a == L"--send" || a.rfind(L"--send=", 0) == 0) {
      if (a.rfind(L"--send=", 0) == 0)
        sendPayload = a.substr(7);
      else if (i + 1 < args.size())
        sendPayload = args[++i];
      continue;
    }
    if (a == L"--status") {
      std::string req = Json::o().set("cmd", "status").dump();
      std::string resp;
      if (PipeSendRecv(req, resp, 1500)) {
        CliPrint(Utf8ToWide(resp));
        return 0;
      }
      // widget not running: quick local sample
      Sampler s;
      SysStats st{};
      s.sample(st);
      Sleep(350);
      s.sample(st);
      CliPrint(Utf8ToWide(Json::o()
                              .set("ok", true)
                              .set("cpu", st.cpuPercent)
                              .set("ramPct", st.ramPercent)
                              .set("ramUsedGB", st.ramUsedGB)
                              .set("ramTotalGB", st.ramTotalGB)
                              .set("widgetRunning", false)
                              .dump()));
      return 0;
    }
    if (a == L"--install") {
      if (!SetAutostart(true)) {
        CliErr(L"не удалось записать автозапуск в реестр");
        return 1;
      }
      CliPrint(L"Автозапуск включён (HKCU\\...\\Run).");
      if (!IsWidgetRunning()) {
        SpawnDetachedSelf();
        CliPrint(L"Виджет запущен.");
      } else {
        CliPrint(L"Виджет уже запущен.");
      }
      return 0;
    }
    if (a == L"--uninstall") {
      std::string resp;
      PipeSendRecv(Json::o().set("cmd", "exit").dump(), resp, 1000);
      SetAutostart(false);
      CliPrint(L"Автозапуск отключён, виджет остановлен.");
      return 0;
    }
    if (a == L"--restart") {
      std::string resp;
      PipeSendRecv(Json::o().set("cmd", "exit").dump(), resp, 1000);
      Sleep(800);
      SpawnDetachedSelf();
      CliPrint(L"Виджет перезапущен.");
      return 0;
    }
    if (a == L"--hide-widgets") {
      SetTaskbarWidgets(false);
      return 0;
    }
    if (a == L"--show-widgets") {
      SetTaskbarWidgets(true);
      return 0;
    }
  }

  if (!sendPayload.empty()) {
    Json req;
    std::string err;
    if (!JsonParse(WideToUtf8(sendPayload), req, err)) {
      CliErr(L"некорректный JSON: " + Utf8ToWide(err));
      return 1;
    }
    std::string resp;
    if (!PipeSendRecv(req.dump(), resp, 3000)) {
      CliErr(L"виджет не запущен (нет \\\\.\\pipe\\SysWidget)");
      return 1;
    }
    CliPrint(Utf8ToWide(resp));
    return 0;
  }

  // ---- GUI: single instance -------------------------------------------------
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    // bring to front is unnecessary (always-on); just exit quietly
    return 0;
  }

  // per-monitor v2 DPI awareness (dynamic, pre-Win10-safe)
  {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
      auto p = (BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT))GetProcAddress(
          u32, "SetProcessDpiAwarenessContext");
      if (p) p((DPI_AWARENESS_CONTEXT)-4);
    }
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
    CliErr(L"CoInitializeEx failed");
    return 1;
  }

  {
    SharedState st;
    LoadConfig(st);

    Trace("config loaded");
    PipeServerStart(&st);
    Trace("pipe started");
    ScriptRunner scripts(st);
    scripts.start();

    int rc = 0;
    WidgetUI ui;
    if (ui.create(st)) {
      Trace("ui created, entering loop");
      rc = ui.run();
      Trace("loop done");
    } else {
      CliErr(L"не удалось создать окно/графику: " + g_uiFailReason);
      st.quit.store(true, std::memory_order_relaxed);
      rc = 1;
    }

    st.quit.store(true, std::memory_order_relaxed);
    scripts.join();
    PipeServerStop();
    CoUninitialize();
    if (mutex) ReleaseMutex(mutex);
    return rc;
  }
}
