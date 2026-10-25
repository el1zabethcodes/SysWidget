// Local named-pipe control server (newline-delimited JSON protocol) + client.
#pragma once
#include "common.h"
#include "jsonlite.h"
#include "content.h"

#include <atomic>

// ---------------------------------------------------------------------------
// Client: send one NDJSON request, read one response line.
// ---------------------------------------------------------------------------
inline bool PipeSendRecv(const std::string& jsonReq, std::string& resp, DWORD timeoutMs = 3000) {
  HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (!WaitNamedPipeW(kPipeName, timeoutMs)) return false;
    h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
  }
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

  std::string req = jsonReq + "\n";
  DWORD written = 0;
  BOOL ok = WriteFile(h, req.data(), (DWORD)req.size(), &written, nullptr);
  if (!ok || written != req.size()) { CloseHandle(h); return false; }
  FlushFileBuffers(h);

  resp.clear();
  char buf[4096];
  DWORD got = 0;
  ULONGLONG t0 = GetTickCount64();
  while (resp.find('\n') == std::string::npos) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
      if (GetTickCount64() - t0 > timeoutMs) break;
      Sleep(20);
      continue;
    }
    if (!ReadFile(h, buf, (DWORD)sizeof buf, &got, nullptr) || got == 0) break;
    resp.append(buf, got);
  }
  CloseHandle(h);
  size_t nl = resp.find('\n');
  if (nl != std::string::npos) resp.resize(nl);
  return !resp.empty();
}

inline bool IsWidgetRunning() {
  std::string resp;
  return PipeSendRecv(Json::o().set("cmd", "status").dump(), resp, 500);
}

// ---------------------------------------------------------------------------
// Server: N worker instances, each owning one pipe instance.
// ---------------------------------------------------------------------------
namespace pipedetail {

struct ServerCtx {
  SharedState* st = nullptr;
  HANDLE quitEvent = nullptr;
  std::vector<HANDLE> threads;
};

inline void ServeClient(HANDLE pipe, SharedState* st) {
  std::string pending;
  char buf[4096];
  DWORD got = 0;
  for (;;) {
    Trace("ServeClient ReadFile...");
    if (!ReadFile(pipe, buf, sizeof buf, &got, nullptr) || got == 0) { Trace("ServeClient read end", (long)GetLastError()); break; }
    Trace("ServeClient read", (long)got);
    pending.append(buf, got);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      // trim CR/space
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty()) continue;
      std::string out;
      Json req;
      std::string err;
      if (JsonParse(line, req, err))
        out = ApplyCommand(*st, req);
      else
        out = Json::o().set("ok", false).set("error", "bad json: " + err).dump();
      out += "\n";
      DWORD written = 0;
      if (!WriteFile(pipe, out.data(), (DWORD)out.size(), &written, nullptr)) return;
      FlushFileBuffers(pipe);
    }
    if (pending.size() > 1 << 20) break;  // abuse guard
  }
}

inline DWORD WINAPI InstanceThread(LPVOID param) {
  ServerCtx* ctx = (ServerCtx*)param;
  while (WaitForSingleObject(ctx->quitEvent, 0) != WAIT_OBJECT_0) {
    HANDLE pipe = CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) break;

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if (!connected && GetLastError() != ERROR_IO_PENDING) {
      // client connected between create & connect
      if (GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(ov.hEvent); CloseHandle(pipe); break; }
    }
    HANDLE waits[2] = {ov.hEvent, ctx->quitEvent};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 + 1) {  // quit
      CancelIo(pipe);
      CloseHandle(ov.hEvent);
      CloseHandle(pipe);
      break;
    }
    ServeClient(pipe, ctx->st);
    DisconnectNamedPipe(pipe);
    CloseHandle(ov.hEvent);
    CloseHandle(pipe);
  }
  return 0;
}

inline ServerCtx g_ctx;

}  // namespace pipedetail

inline void PipeServerStart(SharedState* st) {
  using namespace pipedetail;
  g_ctx.st = st;
  g_ctx.quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  for (int i = 0; i < 3; i++) {
    HANDLE t = CreateThread(nullptr, 0, InstanceThread, &g_ctx, 0, nullptr);
    if (t) g_ctx.threads.push_back(t);
  }
}

inline void PipeServerStop() {
  using namespace pipedetail;
  if (g_ctx.quitEvent) SetEvent(g_ctx.quitEvent);
  // Nudge any blocked connects by opening+closing a pipe handle.
  HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD got;
    WriteFile(h, "\n", 1, &got, nullptr);
    CloseHandle(h);
  }
  for (HANDLE t : g_ctx.threads) {
    WaitForSingleObject(t, 3000);
    CloseHandle(t);
  }
  g_ctx.threads.clear();
}
