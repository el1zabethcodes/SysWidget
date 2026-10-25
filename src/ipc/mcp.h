// MCP (Model Context Protocol) stdio bridge: JSON-RPC 2.0, one JSON per line.
// Tools map 1:1 onto the widget's named-pipe protocol; the bridge starts the
// widget automatically if it is not running.
#pragma once
#include "common.h"
#include "jsonlite.h"
#include "pipe_server.h"

namespace mcp {

inline Json toolDef(const char* name, const char* desc, Json schema) {
  return Json::o().set("name", name).set("description", desc).set("inputSchema", schema);
}

inline Json objSchema(Json props, Json required = Json()) {
  Json s = Json::o().set("type", "object").set("properties", props);
  if (required.type == Json::ARR && !required.arr.empty()) s.set("required", required);
  return s;
}

inline Json propStr(const char* desc) { return Json::o().set("type", "string").set("description", desc); }
inline Json propNum(const char* desc) { return Json::o().set("type", "number").set("description", desc); }
inline Json propInt(const char* desc) {
  return Json::o().set("type", "integer").set("description", desc);
}
inline Json propStrArr(const char* desc) {
  return Json::o().set("type", "array").set("items", Json::o().set("type", "string"))
      .set("description", desc);
}

inline std::string toolsList() {
  Json tools = Json::a();

  tools.push(toolDef(
      "syswidget_status", "Read live CPU / RAM utilization of the machine.",
      objSchema(Json::o())));

  Json setLineProps = Json::o();
  setLineProps.set("index", propInt("Taskbar line index: 0 (top) or 1 (bottom)"));
  setLineProps.set("text",
                   propStr("Text to display. Supports '|' to split label|value and tokens "
                           "{cpu} {ram} {ramUsed} {ramTotal} {time}"));
  setLineProps.set("icon", propStr("Optional Segoe Fluent Icons glyph (single character)"));
  setLineProps.set("color", propStr("Optional value color like #60CDFF"));
  setLineProps.set("bar", propNum("Optional 0..1 value for a mini progress bar"));
  tools.push(toolDef("syswidget_set_line", "Set one of the widget's two taskbar lines to custom text.",
                     objSchema(setLineProps, Json::a().push(Json::s("text")))));

  Json setLinesProps = Json::o();
  setLinesProps.set("texts", propStrArr("Exactly two strings for line 0 and line 1"));
  tools.push(toolDef("syswidget_set_lines", "Set both taskbar lines at once.",
                     objSchema(setLinesProps, Json::a().push(Json::s("texts")))));

  Json animProps = Json::o();
  animProps.set("index", propInt("Line index: 0 or 1"));
  animProps.set("frames", propStrArr("Animation frames, e.g. braille spinner [\"⠋\",\"⠙\",\"⠹\",...]"));
  animProps.set("interval_ms", propNum("Frame interval in ms (min 30)"));
  animProps.set("text", propStr("Optional static suffix after the frame glyph"));
  tools.push(toolDef("syswidget_animate",
                     "Play a frame-by-frame animation on a line (spinner, progress etc.).",
                     objSchema(animProps, Json::a().push(Json::s("frames")))));

  Json scriptProps = Json::o();
  scriptProps.set("index", propInt("Line index: 0 or 1"));
  scriptProps.set("command", propStr("Shell command (cmd.exe); its first stdout line becomes the text"));
  scriptProps.set("interval_ms", propNum("Refresh interval in ms (min 100, default 2000)"));
  tools.push(toolDef("syswidget_script",
                     "Run a shell command periodically and show its first output line on the widget.",
                     objSchema(scriptProps, Json::a().push(Json::s("command")))));

  Json rmProps = Json::o();
  rmProps.set("index", propInt("Line index: 0 or 1"));
  tools.push(toolDef("syswidget_script_remove",
                     "Remove a script/override from a line and restore the default CPU/RAM line.",
                     objSchema(rmProps)));

  Json noteProps = Json::o();
  noteProps.set("text", propStr("Notification text"));
  noteProps.set("duration_ms", propNum("How long to show it (default 4000)"));
  tools.push(toolDef("syswidget_notify", "Temporarily flash a notification on the top line.",
                     objSchema(noteProps, Json::a().push(Json::s("text")))));

  tools.push(toolDef("syswidget_reset", "Reset both lines to the default CPU/RAM monitors.",
                     objSchema(Json::o())));

  return Json::o().set("tools", tools).dump();
}

// Map a tools/call to a pipe request. Returns false with errMsg on bad args.
inline bool mapTool(const std::string& name, const Json& args, Json& req, std::string& errMsg) {
  auto argInt = [&](const char* k, int def) { return (int)args.getNum(k, (double)def); };
  auto argStr = [&](const char* k) { return args.getStr(k, ""); };

  if (name == "syswidget_status") {
    req = Json::o().set("cmd", "status");
    return true;
  }
  if (name == "syswidget_set_line") {
    if (args.getStr("text", "").empty() && !args.find("text")) {
      errMsg = "'text' is required";
      return false;
    }
    req = Json::o().set("cmd", "line").set("index", (double)argInt("index", 0));
    req.set("text", argStr("text"));
    if (!args.getStr("icon", "").empty()) req.set("icon", argStr("icon"));
    if (!args.getStr("color", "").empty()) req.set("color", argStr("color"));
    if (args.find("bar")) req.set("bar", args.getNum("bar", 0));
    return true;
  }
  if (name == "syswidget_set_lines") {
    const Json* t = args.find("texts");
    if (!t || t->type != Json::ARR) {
      errMsg = "'texts' array is required";
      return false;
    }
    req = Json::o().set("cmd", "lines");
    Json arr = Json::a();
    for (auto& v : t->arr) arr.push(Json::s(v.type == Json::STR ? v.str : ""));
    req.set("texts", arr);
    return true;
  }
  if (name == "syswidget_animate") {
    const Json* f = args.find("frames");
    if (!f || f->type != Json::ARR || f->arr.empty()) {
      errMsg = "'frames' non-empty array is required";
      return false;
    }
    req = Json::o().set("cmd", "anim").set("index", (double)argInt("index", 0));
    Json arr = Json::a();
    for (auto& v : f->arr) arr.push(Json::s(v.type == Json::STR ? v.str : ""));
    req.set("frames", arr);
    if (args.find("interval_ms")) req.set("intervalMs", args.getNum("interval_ms", 150));
    if (!args.getStr("text", "").empty()) req.set("text", argStr("text"));
    return true;
  }
  if (name == "syswidget_script") {
    if (argStr("command").empty()) {
      errMsg = "'command' is required";
      return false;
    }
    req = Json::o().set("cmd", "script").set("index", (double)argInt("index", 0));
    req.set("command", argStr("command"));
    if (args.find("interval_ms")) req.set("intervalMs", args.getNum("interval_ms", 2000));
    return true;
  }
  if (name == "syswidget_script_remove") {
    req = Json::o().set("cmd", "script.remove").set("index", (double)argInt("index", 0));
    return true;
  }
  if (name == "syswidget_notify") {
    if (argStr("text").empty()) {
      errMsg = "'text' is required";
      return false;
    }
    req = Json::o().set("cmd", "notify").set("text", argStr("text"));
    if (args.find("duration_ms")) req.set("ms", args.getNum("duration_ms", 4000));
    return true;
  }
  if (name == "syswidget_reset") {
    req = Json::o().set("cmd", "reset");
    return true;
  }
  errMsg = "unknown tool: " + name;
  return false;
}

inline void writeLine(const std::string& s) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD w = 0;
  std::string line = s + "\n";
  WriteFile(out, line.data(), (DWORD)line.size(), &w, nullptr);
  FlushFileBuffers(out);
}

inline void respond(const Json& id, Json result) {
  Json r = Json::o().set("jsonrpc", "2.0").set("id", id);
  r.set("result", std::move(result));
  writeLine(r.dump());
}

inline void respondErr(const Json& id, int code, const std::string& message) {
  Json err = Json::o().set("code", (double)code).set("message", message);
  Json r = Json::o().set("jsonrpc", "2.0").set("id", id);
  r.set("error", std::move(err));
  writeLine(r.dump());
}

inline bool ensureWidgetRunning() {
  std::string resp;
  if (PipeSendRecv(Json::o().set("cmd", "status").dump(), resp, 400)) return true;
  // spawn the widget GUI detached
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cl = std::wstring(L"\"") + exe + L"\"";
  std::vector<wchar_t> buf(cl.begin(), cl.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                      nullptr, &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  for (int i = 0; i < 40; i++) {  // up to ~8s
    Sleep(200);
    if (PipeSendRecv(Json::o().set("cmd", "status").dump(), resp, 400)) return true;
  }
  return false;
}

inline int runBridge() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  std::string pending;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(in, buf, sizeof buf, &got, nullptr) && got > 0) {
    pending.append(buf, got);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty()) continue;

      Json msg;
      std::string err;
      if (!JsonParse(line, msg, err)) continue;  // skip malformed lines
      if (msg.type != Json::OBJ) continue;

      bool hasId = msg.find("id") != nullptr;
      Json id = hasId ? *msg.find("id") : Json();
      std::string method = msg.getStr("method", "");

      if (method == "initialize") {
        Json cap = Json::o().set("tools", Json::o());
        Json res = Json::o()
                       .set("protocolVersion", "2024-11-05")
                       .set("capabilities", cap)
                       .set("serverInfo", Json::o().set("name", "syswidget").set("version", kVersion));
        respond(id, std::move(res));
      } else if (method == "notifications/initialized" || method == "initialized") {
        // notification, no response
      } else if (method == "ping") {
        respond(id, Json::o());
      } else if (method == "tools/list") {
        Json res;
        std::string tl = toolsList();
        JsonParse(tl, res, err);
        respond(id, std::move(res));
      } else if (method == "tools/call") {
        const Json* params = msg.find("params");
        std::string tname = params ? params->getStr("name", "") : "";
        const Json* args = params ? params->find("arguments") : nullptr;
        Json emptyArgs = Json::o();
        if (!args || args->type != Json::OBJ) args = &emptyArgs;

        Json req;
        std::string mapErr;
        if (!mapTool(tname, *args, req, mapErr)) {
          respondErr(id, -32602, mapErr);
          continue;
        }
        std::string pipeResp;
        if (!ensureWidgetRunning())
          pipeResp = "{\"ok\":false,\"error\":\"widget is not running\"}";
        else if (!PipeSendRecv(req.dump(), pipeResp, 5000))
          pipeResp = "{\"ok\":false,\"error\":\"pipe timeout\"}";

        // bubble the raw widget response through MCP text content
        Json content = Json::a();
        content.push(Json::o().set("type", "text").set("text", pipeResp));
        bool isErr = pipeResp.find("\"ok\":false") != std::string::npos;
        Json res = Json::o().set("content", content);
        if (isErr) res.set("isError", true);
        respond(id, std::move(res));
      } else if (!method.empty() && hasId) {
        respondErr(id, -32601, "method not found: " + method);
      }
    }
  }
  return 0;
}

}  // namespace mcp
