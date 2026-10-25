// AI-agent status monitor: async (OVERLAPPED) named-pipe server
// \\.\pipe\syswidget_agents_ipc + thread-safe agent registry.
// Protocol: one JSON per line, UTF-8:
//   {"id":"claude","name":"Claude","icon_path":"assets/claude.png",
//    "status":"working","status_text":"Refactoring layout engine..."}
//   status: "working" | "done" | "attention" | "offline"  (offline = hidden)
#pragma once
#include "common.h"
#include "jsonlite.h"
#include "content.h"

#include <string_view>
#include <thread>
#include <unordered_map>

constexpr const wchar_t* kAgentsPipeName = L"\\\\.\\pipe\\syswidget_agents_ipc";

enum class AgentStatus { Working, Done, Attention, Offline };

inline bool AgentStatusFromName(std::string_view s, AgentStatus& out) {
  if (s == "working") { out = AgentStatus::Working; return true; }
  if (s == "done") { out = AgentStatus::Done; return true; }
  if (s == "attention") { out = AgentStatus::Attention; return true; }
  if (s == "offline") { out = AgentStatus::Offline; return true; }
  return false;
}

struct AgentState {
  std::string id, name, iconPath, statusText;
  AgentStatus status = AgentStatus::Working;
  ULONGLONG updatedMs = 0;
};

class AgentService {
 public:
  void start(HWND notify, SharedState* st) {
    notify_ = notify;
    st_ = st;
    for (int i = 0; i < 2; i++)
      ths_.emplace_back([this](std::stop_token tok) { instanceLoop(std::move(tok)); });
  }
  void stop() {
    for (auto& t : ths_) t.request_stop();
    // nudge blocked ConnectNamedPipe calls with a dummy client
    for (int i = 0; i < 2; i++) {
      HANDLE h = CreateFileW(kAgentsPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, "\n", 1, &w, nullptr);
        CloseHandle(h);
      }
      Sleep(60);
    }
    for (auto& t : ths_) {
      if (t.joinable()) t.join();
    }
    ths_.clear();
  }
  ~AgentService() { stop(); }

  // Register/refresh an agent from a JSON message (pipe cmd or legacy "agent").
  void apply(const Json& req) {
    AgentState a;
    a.id = req.getStr("id", "");
    if (a.id.empty()) return;
    a.name = req.getStr("name", a.id);
    a.iconPath = req.getStr("icon_path", "");
    a.statusText = req.getStr("status_text", "");
    AgentStatus st;
    a.status = AgentStatusFromName(req.getStr("status", "working"), st) ? st : AgentStatus::Working;
    a.updatedMs = GetTickCount64();
    bool changed = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      auto it = agents_.find(a.id);
      if (it == agents_.end()) {
        agents_.emplace(a.id, std::move(a));
        changed = true;
      } else {
        changed = it->second.status != a.status || it->second.statusText != a.statusText ||
                  it->second.iconPath != a.iconPath || it->second.name != a.name;
        it->second = std::move(a);
      }
    }
    if (changed) {
      if (st_) st_->dirty.store(true, std::memory_order_relaxed);
      if (notify_) PostMessageW(notify_, WM_APP_DATA_UPDATED, 0, 0);
    }
  }

  // Snapshot of agents that should be rendered (offline excluded), id-sorted.
  std::vector<AgentState> visible() const {
    std::vector<AgentState> out;
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [id, a] : agents_)
      if (a.status != AgentStatus::Offline) out.push_back(a);
    std::sort(out.begin(), out.end(), [](const AgentState& l, const AgentState& r) { return l.id < r.id; });
    return out;
  }

  static std::string HandleLine(std::string_view line, void* ctx) {
    auto* self = static_cast<AgentService*>(ctx);
    Json req;
    std::string err;
    std::string resp;
    if (JsonParse(std::string(line), req, err) && req.type == Json::OBJ) {
      self->apply(req);
      resp = Json::o().set("ok", true).dump();
    } else {
      resp = Json::o().set("ok", false).set("error", "bad json: " + err).dump();
    }
    return resp + "\n";
  }

 private:
  void instanceLoop(std::stop_token tok) {
    while (!tok.stop_requested()) {
      HANDLE pipe = CreateNamedPipeW(
          kAgentsPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 8192,
          8192, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

      OVERLAPPED ov{};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      BOOL connected = ConnectNamedPipe(pipe, &ov);
      if (!connected && GetLastError() != ERROR_IO_PENDING &&
          GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        continue;
      }
      HANDLE waits[2] = {ov.hEvent, stopEvent_};
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
      if (w != WAIT_OBJECT_0 && GetLastError() != ERROR_PIPE_CONNECTED) {
        // stop requested or timeout without client
        CancelIo(pipe);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        continue;
      }
      serve(pipe);
      DisconnectNamedPipe(pipe);
      CloseHandle(ov.hEvent);
      CloseHandle(pipe);
    }
  }

  void serve(HANDLE pipe) {
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
      if (!ReadFile(pipe, buf, sizeof buf, &got, nullptr) || got == 0) break;
      pending.append(buf, got);
      size_t pos;
      while ((pos = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::string out = HandleLine(line, this);
        DWORD w = 0;
        if (!WriteFile(pipe, out.data(), (DWORD)out.size(), &w, nullptr)) return;
        FlushFileBuffers(pipe);
      }
      if (pending.size() > (1u << 20)) break;  // abuse guard
    }
  }

  HANDLE stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<std::jthread> ths_;
  mutable std::mutex m_;
  std::unordered_map<std::string, AgentState> agents_;
  HWND notify_ = nullptr;
  SharedState* st_ = nullptr;
};
