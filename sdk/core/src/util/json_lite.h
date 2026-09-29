// libcimsue 내부 — 최소 JSON 파서(헤더 전용). Engine(pjlib)과 독립·임의 스레드에서 쓰이므로 pj_json 을 쓰지 않는다 —
// CSC 클라이언트(프로비저닝·토큰)와 계측 링크(drive/device_link)가 같이 쓴다.
#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cimsue {
namespace jsonlite {

/** 최소 JSON 파서 — CscClient 는 Engine(pjlib)과 독립·임의 스레드에서 쓰이므로 pj_json 을 쓰지 않는다.
 *  객체/배열/문자열(escape·\uXXXX)/숫자/불/null. 값은 트리로 들고 이름으로 찾는다. */
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<std::pair<std::string, JVal>> kids;     // Obj: name/value, Arr: name 빈 문자열
    const JVal* get(const char* k) const { if (t != Obj) return nullptr; for (auto& kv : kids) if (kv.first == k) return &kv.second; return nullptr; }
};
struct JParser {
    const std::string& s; size_t p = 0; bool ok = true;
    explicit JParser(const std::string& in) : s(in) {}
    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    bool lit(const char* l) { size_t n = std::strlen(l); if (s.compare(p, n, l) == 0) { p += n; return true; } return false; }
    JVal parse() { ws(); JVal v;
        if (p >= s.size()) { ok = false; return v; }
        char c = s[p];
        if (c == '{') { v.t = JVal::Obj; ++p; ws(); if (p < s.size() && s[p] == '}') { ++p; return v; }
            while (ok) { ws(); JVal k = parse(); if (k.t != JVal::Str) { ok = false; break; } ws(); if (!lit(":")) { ok = false; break; }
                JVal val = parse(); v.kids.emplace_back(k.s, val); ws(); if (lit(",")) continue; if (lit("}")) break; ok = false; }
        } else if (c == '[') { v.t = JVal::Arr; ++p; ws(); if (p < s.size() && s[p] == ']') { ++p; return v; }
            while (ok) { JVal val = parse(); v.kids.emplace_back(std::string(), val); ws(); if (lit(",")) continue; if (lit("]")) break; ok = false; }
        } else if (c == '"') { v.t = JVal::Str; ++p;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) { ++p; char e = s[p];
                    if (e == 'n') v.s += '\n'; else if (e == 't') v.s += '\t'; else if (e == 'r') v.s += '\r'; else if (e == 'b') v.s += '\b'; else if (e == 'f') v.s += '\f';
                    else if (e == 'u' && p + 4 < s.size()) { unsigned cp = (unsigned)std::strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16); p += 4;
                        if (cp < 0x80) v.s += (char)cp; else if (cp < 0x800) { v.s += (char)(0xC0 | (cp >> 6)); v.s += (char)(0x80 | (cp & 0x3F)); }
                        else { v.s += (char)(0xE0 | (cp >> 12)); v.s += (char)(0x80 | ((cp >> 6) & 0x3F)); v.s += (char)(0x80 | (cp & 0x3F)); } }
                    else v.s += e; ++p; }
                else v.s += s[p++]; }
            if (p < s.size()) ++p; else ok = false;
        } else if (lit("true")) { v.t = JVal::Bool; v.b = true; }
        else if (lit("false")) { v.t = JVal::Bool; v.b = false; }
        else if (lit("null")) { v.t = JVal::Null; }
        else { size_t e = p; while (e < s.size() && (std::isdigit((unsigned char)s[e]) || s[e] == '-' || s[e] == '+' || s[e] == '.' || s[e] == 'e' || s[e] == 'E')) ++e;
            if (e == p) { ok = false; return v; } v.t = JVal::Num; v.n = std::atof(s.substr(p, e - p).c_str()); p = e; }
        return v; }
};
struct Json {
    JVal rootVal; const JVal* root = nullptr;
    explicit Json(const std::string& text) { JParser jp(text); rootVal = jp.parse(); if (jp.ok) root = &rootVal; }
    static const JVal* child(const JVal* obj, const char* name) { return obj ? obj->get(name) : nullptr; }
    static std::string str(const JVal* obj, const char* name, const std::string& dflt = std::string()) {
        const JVal* e = child(obj, name); if (!e) return dflt;
        if (e->t == JVal::Str) return e->s; if (e->t == JVal::Num) return std::to_string((long)e->n); if (e->t == JVal::Bool) return e->b ? "true" : "false";
        return dflt; }
    static int num(const JVal* obj, const char* name, int dflt) { const JVal* e = child(obj, name); if (!e) return dflt;
        if (e->t == JVal::Num) return (int)e->n; if (e->t == JVal::Str) return std::atoi(e->s.c_str()); return dflt; }
    static bool boolean(const JVal* obj, const char* name, bool dflt) { const JVal* e = child(obj, name); return (e && e->t == JVal::Bool) ? e->b : dflt; }
    template <typename F> static void each(const JVal* arr, F f) { if (!arr || arr->t != JVal::Arr) return; for (auto& kv : arr->kids) f(&kv.second); }
};

}  // namespace jsonlite
}  // namespace cimsue
