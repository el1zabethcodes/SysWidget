// CPU / RAM sampling via native WinAPI (GetSystemTimes + GlobalMemoryStatusEx).
#pragma once
#include "common.h"

struct SysStats {
  double cpuPercent = 0;
  double ramPercent = 0;
  double ramUsedGB = 0;
  double ramTotalGB = 0;
  ULONGLONG uptimeMs = 0;
};

class Sampler {
 public:
  void sample(SysStats& out) {
    FILETIME idle, kern, user;
    if (GetSystemTimes(&idle, &kern, &user)) {
      auto u64 = [](const FILETIME& f) -> ULONGLONG {
        return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
      };
      ULONGLONG i = u64(idle), k = u64(kern), u = u64(user);
      if (!first_) {
        double dIdle = (double)(i - idle_), dKern = (double)(k - kern_), dUser = (double)(u - user_);
        double dTotal = dKern + dUser;  // kernel time includes idle
        double cpu = (dTotal > 0) ? 100.0 * (1.0 - dIdle / dTotal) : 0.0;
        if (cpu < 0) cpu = 0;
        if (cpu > 100) cpu = 100;
        cpu_ema_ = cpu_ema_ < 0 ? cpu : (0.6 * cpu + 0.4 * cpu_ema_);
        out.cpuPercent = cpu_ema_;
      }
      first_ = false;
      idle_ = i; kern_ = k; user_ = u;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
      out.ramPercent = (double)ms.dwMemoryLoad;
      out.ramTotalGB = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
      out.ramUsedGB = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
    out.uptimeMs = GetTickCount64();
  }

 private:
  bool first_ = true;
  double cpu_ema_ = -1;
  ULONGLONG idle_ = 0, kern_ = 0, user_ = 0;
};
