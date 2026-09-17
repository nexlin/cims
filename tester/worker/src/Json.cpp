#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const Json s_null;

const Json& Json::operator[](const std::string& k) const {
    if (m_type != OBJECT) return s_null;
    auto it = m_o.find(k);
    return it == m_o.end() ? s_null : it->second;
}

Json& Json::operator[](const std::string& k) {
    if (m_type != OBJECT) { m_type = OBJECT; m_a.clear(); m_s.clear(); }
    return m_o[k];
}

const Json& Json::at(size_t i) const {
    if (m_type != ARRAY || i >= m_a.size()) return s_null;
    return m_a[i];
}

Json& Json::push(const Json& v) {
    if (m_type != ARRAY) { m_type = ARRAY; m_o.clear(); m_s.clear(); }
    m_a.push_back(v);
    return *this;
}

static void escapeTo(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const {
    switch (m_type) {
    case NUL: out += "null"; break;
    case BOOL: out += m_b ? "true" : "false"; break;
    case NUMBER: {
        if (std::isnan(m_d) || std::isinf(m_d)) { out += "null"; break; }
        char b[64];
        if (m_d == std::floor(m_d) && std::fabs(m_d) < 1e15) snprintf(b, sizeof(b), "%lld", (long long)m_d);
        else snprintf(b, sizeof(b), "%.15g", m_d);   // unix 초(소수 3자리)·지표값을 잃지 않게
        out += b;
        break;
    }
    case STRING: escapeTo(m_s, out); break;
    case ARRAY: {
        out += '[';
        for (size_t i = 0; i < m_a.size(); ++i) { if (i) out += ','; m_a[i].dumpTo(out); }
        out += ']';
        break;
    }
    case OBJECT: {
        out += '{';
        bool first = true;
        for (auto& kv : m_o) {
            if (!first) out += ',';
            first = false;
            escapeTo(kv.first, out);
            out += ':';
            kv.second.dumpTo(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::dump() const { std::string s; s.reserve(256); dumpTo(s); return s; }

static void skipWs(const std::string& t, size_t& p) {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
}

static bool parseString(const std::string& t, size_t& p, std::string& out, std::string& err) {
    if (p >= t.size() || t[p] != '"') { err = "string expected"; return false; }
    ++p;
    while (p < t.size()) {
        char c = t[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= t.size()) break;
            char e = t[p++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (p + 4 > t.size()) { err = "bad \\u"; return false; }
                unsigned int cp = (unsigned int)strtoul(t.substr(p, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                    unsigned int lo = (unsigned int)strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: err = "bad escape"; return false;
            }
        } else {
            out += c;
        }
    }
    err = "unterminated string";
    return false;
}

bool Json::parseValue(const std::string& t, size_t& p, Json& out, std::string& err, int depth) {
    if (depth > 64) { err = "too deep"; return false; }
    skipWs(t, p);
    if (p >= t.size()) { err = "unexpected end"; return false; }
    char c = t[p];
    if (c == '{') {
        ++p;
        out = Json::Object();
        skipWs(t, p);
        if (p < t.size() && t[p] == '}') { ++p; return true; }
        while (true) {
            skipWs(t, p);
            std::string key;
            if (!parseString(t, p, key, err)) return false;
            skipWs(t, p);
            if (p >= t.size() || t[p] != ':') { err = "':' expected"; return false; }
            ++p;
            Json v;
            if (!parseValue(t, p, v, err, depth + 1)) return false;
            out.m_o[key] = v;
            skipWs(t, p);
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            err = "',' or '}' expected";
            return false;
        }
    }
    if (c == '[') {
        ++p;
        out = Json::Array();
        skipWs(t, p);
        if (p < t.size() && t[p] == ']') { ++p; return true; }
        while (true) {
            Json v;
            if (!parseValue(t, p, v, err, depth + 1)) return false;
            out.m_a.push_back(v);
            skipWs(t, p);
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            err = "',' or ']' expected";
            return false;
        }
    }
    if (c == '"') {
        std::string s;
        if (!parseString(t, p, s, err)) return false;
        out = Json(s);
        return true;
    }
    if (t.compare(p, 4, "true") == 0) { p += 4; out = Json(true); return true; }
    if (t.compare(p, 5, "false") == 0) { p += 5; out = Json(false); return true; }
    if (t.compare(p, 4, "null") == 0) { p += 4; out = Json(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char* start = t.c_str() + p;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) { err = "bad number"; return false; }
        p += (size_t)(end - start);
        out = Json(d);
        return true;
    }
    err = std::string("unexpected char '") + c + "'";
    return false;
}

bool Json::parse(const std::string& text, Json& out, std::string& err) {
    size_t p = 0;
    if (!parseValue(text, p, out, err, 0)) return false;
    skipWs(text, p);
    if (p != text.size()) { err = "trailing characters"; return false; }
    return true;
}
