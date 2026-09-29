// libcimsue 내부 — CSC 문서(GMS 그룹·CMS user-profile/service-config) 문자열 스캔 도구.
// CscClient 와 같은 이유로 pjlib(pj_xml)에 의존하지 않는다 — 요소는 접두사 무관하게 로컬 이름으로 찾는다.
#pragma once

#include <cstdlib>
#include <string>

namespace cimsue {
namespace xmlscan {

inline std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

inline std::string unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&'; else if (ent == "lt") o += '<'; else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"'; else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? (unsigned)std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                              : (unsigned)std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

inline std::string trim(const std::string& v) {
    size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, t - b + 1);
}

inline bool nameEnd(char c) { return c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n'; }

/** "<[prefix:]local" 시작 위치 — from 이후 첫 매치. 없으면 npos. */
inline size_t findOpen(const std::string& s, const std::string& local, size_t from = 0) {
    size_t p = from;
    while ((p = s.find('<', p)) != std::string::npos) {
        size_t q = p + 1;
        if (q < s.size() && (s[q] == '/' || s[q] == '?' || s[q] == '!')) { ++p; continue; }
        size_t n = q;
        while (n < s.size() && !nameEnd(s[n])) ++n;
        std::string tag = s.substr(q, n - q);
        size_t colon = tag.find(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag == local) return p;
        p = n;
    }
    return std::string::npos;
}

/** "</[prefix:]local" 위치 — from 이후 첫 매치. 없으면 npos. */
inline size_t findClose(const std::string& s, const std::string& local, size_t from) {
    size_t e = s.find("</", from);
    while (e != std::string::npos) {
        size_t n = e + 2; while (n < s.size() && !nameEnd(s[n])) ++n;
        std::string tag = s.substr(e + 2, n - e - 2);
        size_t colon = tag.find(':'); if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag == local) return e;
        e = s.find("</", n);
    }
    return std::string::npos;
}

/** 요소 텍스트(첫 매치). 빈 요소(<x/>)나 없음이면 빈 문자열, found 로 존재 여부. */
inline std::string elemText(const std::string& s, const std::string& local, bool* found = nullptr, size_t from = 0) {
    if (found) *found = false;
    size_t p = findOpen(s, local, from);
    if (p == std::string::npos) return std::string();
    size_t gt = s.find('>', p);
    if (gt == std::string::npos) return std::string();
    if (found) *found = true;
    if (s[gt - 1] == '/') return std::string();
    size_t e = findClose(s, local, gt);
    if (e == std::string::npos) return std::string();
    return unesc(trim(s.substr(gt + 1, e - gt - 1)));
}

/** 요소 하나 — 여는 태그 원문(속성 읽기용)과 내용 원문(하위 요소 스캔용). 같은 이름의 중첩은 다루지 않는다(CSC 문서에 없다). */
struct Elem {
    bool found = false;
    std::string openTag;              // "<x a=\"…\">" 까지
    std::string inner;                // 여는 태그와 닫는 태그 사이(빈 요소면 빈 문자열)
    size_t end = std::string::npos;   // 요소 뒤 위치 — 다음 형제 스캔의 from
};

inline Elem elem(const std::string& s, const std::string& local, size_t from = 0) {
    Elem r;
    size_t p = findOpen(s, local, from);
    if (p == std::string::npos) return r;
    size_t gt = s.find('>', p);
    if (gt == std::string::npos) return r;
    r.found = true;
    r.openTag = s.substr(p, gt - p + 1);
    if (s[gt - 1] == '/') { r.end = gt + 1; return r; }
    size_t e = findClose(s, local, gt);
    if (e == std::string::npos) { r.inner = s.substr(gt + 1); r.end = s.size(); return r; }
    r.inner = s.substr(gt + 1, e - gt - 1);
    size_t close = s.find('>', e);
    r.end = close == std::string::npos ? s.size() : close + 1;
    return r;
}

inline std::string attrOf(const std::string& tag, const std::string& name) {
    size_t p = tag.find(name + "=\"");
    if (p == std::string::npos) return std::string();
    size_t b = p + name.size() + 2, e = tag.find('"', b);
    return e == std::string::npos ? std::string() : unesc(tag.substr(b, e - b));
}

inline bool isTrue(const std::string& v) { return v == "true" || v == "1" || v == "TRUE" || v == "True"; }

}  // namespace xmlscan
}  // namespace cimsue
