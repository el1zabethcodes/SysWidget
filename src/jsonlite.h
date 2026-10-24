// Minimal JSON parser/serializer (UTF-8 in, UTF-8 out). No dependencies.
#pragma once
#include "common.h"

class Json {
public:
  enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
  Type type = NUL;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  Json() = default;
  Json(const char* v) : type(STR), str(v) {}
  Json(const std::string& v) : type(STR), str(v) {}
  Json(double v) : type(NUM), num(v) {}
  Json(int v) : type(NUM), num((double)v) {}
  Json(bool v) : type(BOOL), b(v) {}

  static Json s(const std::string& v) { Json j; j.type = STR; j.str = v; return j; }
  static Json s(const std::wstring& v) { return s(WideToUtf8(v)); }
  static Json n(double v) { Json j; j.type = NUM; j.num = v; return j; }
  static Json bl(bool v) { Json j; j.type = BOOL; j.b = v; return j; }
  static Json a() { Json j; j.type = ARR; return j; }
  static Json o() { Json j; j.type = OBJ; return j; }

  Json& push(Json v) { arr.push_back(std::move(v)); return *this; }
  Json& set(const std::string& k, Json v) {
    for (auto& kv : obj)
      if (kv.first == k) { kv.second = std::move(v); return *this; }
    obj.emplace_back(k, std::move(v));
    return *this;
  }
  const Json* find(const std::string& k) const {
    if (type != OBJ) return nullptr;
    for (auto& kv : obj)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
  bool has(const std::string& k) const { return find(k) != nullptr; }
  std::string getStr(const std::string& k, const std::string& def = "") const {
    const Json* j = find(k);
    return (j && j->type == STR) ? j->str : def;
  }
  double getNum(const std::string& k, double def = 0) const {
    const Json* j = find(k);
    return (j && j->type == NUM) ? j->num : ((j && j->type == BOOL) ? (j->b ? 1 : 0) : def);
  }
  bool getBool(const std::string& k, bool def = false) const {
    const Json* j = find(k);
    if (!j) return def;
    if (j->type == BOOL) return j->b;
    if (j->type == NUM) return j->num != 0;
    return def;
  }

  void dumpTo(std::string& out) const {
    char buf[64];
    switch (type) {
      case NUL: out += "null"; break;
      case BOOL: out += b ? "true" : "false"; break;
      case NUM: {
        if (std::isfinite(num) && num == (long long)num && std::fabs(num) < 1e15)
          snprintf(buf, sizeof buf, "%lld", (long long)num);
        else
          snprintf(buf, sizeof buf, "%.10g", num);
        out += buf;
        break;
      }
      case STR: {
        out += '"';
        for (unsigned char c : str) {
          switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
              if (c < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
              else out += (char)c;
          }
        }
        out += '"';
        break;
      }
      case ARR: {
        out += '[';
        bool first = true;
        for (auto& v : arr) { if (!first) out += ','; first = false; v.dumpTo(out); }
        out += ']';
        break;
      }
      case OBJ: {
        out += '{';
        bool first = true;
        for (auto& kv : obj) {
          if (!first) out += ',';
          first = false;
          Json js; js.type = STR; js.str = kv.first; js.dumpTo(out);
          out += ':';
          kv.second.dumpTo(out);
        }
        out += '}';
        break;
      }
    }
  }
  std::string dump() const { std::string s; dumpTo(s); return s; }
};

inline bool JsonParse(const std::string& text, Json& out, std::string& err) {
  const char* p = text.c_str();
  const char* end = p + text.size();
  int depth = 0;

  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB)
    p += 3; // skip UTF-8 BOM

  struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

    void appendUtf8(std::string& s, uint32_t cp) {
      if (cp <= 0x7F) s += (char)cp;
      else if (cp <= 0x7FF) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
      else if (cp <= 0xFFFF) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
      else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }

    bool parseString(std::string& s) {
      if (p >= end || *p != '"') { err = "expected string"; return false; }
      p++;
      s.clear();
      while (p < end && *p != '"') {
        char c = *p;
        if ((unsigned char)c < 0x20) { err = "control char in string"; return false; }
        if (c == '\\') {
          p++;
          if (p >= end) { err = "bad escape"; return false; }
          char e = *p++;
          switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
              if (end - p < 4) { err = "bad \\u"; return false; }
              auto hex4 = [&]() -> uint32_t {
                uint32_t v = 0;
                for (int i = 0; i < 4; i++) {
                  char h = *p++;
                  v <<= 4;
                  if (h >= '0' && h <= '9') v |= (h - '0');
                  else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
                  else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
                  else { err = "bad hex"; return 0xFFFFFFFF; }
                }
                return v;
              };
              uint32_t cp = hex4();
              if (cp == 0xFFFFFFFF) return false;
              if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                p += 2;
                uint32_t lo = hex4();
                if (lo == 0xFFFFFFFF) return false;
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else { err = "bad surrogate"; return false; }
              } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD; // lone surrogate -> replacement
              }
              appendUtf8(s, cp);
              break;
            }
            default: err = "bad escape"; return false;
          }
        } else {
          s += c;
          p++;
        }
      }
      if (p >= end) { err = "unterminated string"; return false; }
      p++; // closing quote
      return true;
    }

    bool parseValue(Json& j) {
      if (depth > 48) { err = "too deep"; return false; }
      ws();
      if (p >= end) { err = "unexpected end"; return false; }
      char c = *p;
      if (c == '{') {
        p++; depth++;
        j.type = Json::OBJ;
        ws();
        if (p < end && *p == '}') { p++; depth--; return true; }
        for (;;) {
          ws();
          std::string key;
          if (!parseString(key)) return false;
          ws();
          if (p >= end || *p != ':') { err = "expected ':'"; return false; }
          p++;
          Json val;
          if (!parseValue(val)) return false;
          j.obj.emplace_back(std::move(key), std::move(val));
          ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == '}') { p++; depth--; return true; }
          err = "expected ',' or '}'";
          return false;
        }
      }
      if (c == '[') {
        p++; depth++;
        j.type = Json::ARR;
        ws();
        if (p < end && *p == ']') { p++; depth--; return true; }
        for (;;) {
          Json val;
          if (!parseValue(val)) return false;
          j.arr.push_back(std::move(val));
          ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == ']') { p++; depth--; return true; }
          err = "expected ',' or ']'";
          return false;
        }
      }
      if (c == '"') {
        j.type = Json::STR;
        return parseString(j.str);
      }
      if (c == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; j.type = Json::BOOL; j.b = true; return true; }
      if (c == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; j.type = Json::BOOL; j.b = false; return true; }
      if (c == 'n' && end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; j.type = Json::NUL; return true; }
      // number
      {
        char* e = nullptr;
        double v = strtod(p, &e);
        if (e == p) { err = "unexpected token"; return false; }
        j.type = Json::NUM;
        j.num = v;
        p = e;
        return true;
      }
    }
  } ps{p, end, {}};

  if (!ps.parseValue(out)) {
    err = ps.err.empty() ? "parse error" : ps.err;
    return false;
  }
  ps.ws();
  return true;
}
