#ifndef __SIMPLE_JSON_H__
#define __SIMPLE_JSON_H__

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdio>

// Very Minimal JSON Parser/Builder
// Limitations: No arrays (except via string parsing manually if needed), supports Object, String, Int/Double (as string/int).
// Sufficient for CSP-CMP protocol which is Command-Response with Key-Values.

namespace SimpleJson {

enum NodeType {
    JSON_NULL,
    JSON_OBJECT,
    JSON_STRING,
    JSON_INT,
    JSON_ARRAY
};

class JsonNode {
public:
    NodeType type;
    std::map<std::string, JsonNode> objects;
    std::vector<JsonNode> array;
    std::string strValue;
    long long intValue;

    JsonNode() : type(JSON_NULL), intValue(0) {}
    
    // Constructors
    JsonNode(const std::string& val) : type(JSON_STRING), strValue(val), intValue(0) {}
    JsonNode(const char* val) : type(JSON_STRING), strValue(val), intValue(0) {}
    JsonNode(int val) : type(JSON_INT), intValue(val) {}
    JsonNode(long long val) : type(JSON_INT), intValue(val) {}

    // Object setters
    void Set(const std::string& key, const std::string& val) {
        if (type == JSON_NULL) type = JSON_OBJECT;
        objects[key] = JsonNode(val);
    }

    void Set(const std::string& key, const char* val) {
        if (type == JSON_NULL) type = JSON_OBJECT;
        objects[key] = JsonNode(val);
    }
    
    void Set(const std::string& key, int val) {
        if (type == JSON_NULL) type = JSON_OBJECT;
        objects[key] = JsonNode(val);
    }
    
    void Set(const std::string& key, long long val) {
        if (type == JSON_NULL) type = JSON_OBJECT;
        objects[key] = JsonNode(val);
    }

     void Set(const std::string& key, const JsonNode& node) {
        if (type == JSON_NULL) type = JSON_OBJECT;
        objects[key] = node;
    }

    // Array Add
    void Add(const JsonNode& node) {
        if (type == JSON_NULL) type = JSON_ARRAY;
        array.push_back(node);
    }

    // Getters
    std::string GetString(const std::string& key, const std::string& defaultVal = "") const {
        auto it = objects.find(key);
        if (it != objects.end()) {
             if (it->second.type == JSON_STRING) return it->second.strValue;
             if (it->second.type == JSON_INT) return std::to_string(it->second.intValue);
        }
        return defaultVal;
    }

    std::string AsString() const { 
        if (type == JSON_INT) return std::to_string(intValue);
        return strValue; 
    }
    int AsInt() const { 
        if (type == JSON_STRING) {
            try { return std::stoi(strValue); } catch(...) { return 0; }
        }
        return (int)intValue; 
    }

    long long GetInt(const std::string& key, int defaultVal = 0) const {
        auto it = objects.find(key);
        if (it != objects.end()) {
             if (it->second.type == JSON_INT) return it->second.intValue;
             if (it->second.type == JSON_STRING) {
                 try { return std::stoll(it->second.strValue); } catch(...) {}
             }
        }
        return defaultVal;
    }
    
    bool Has(const std::string& key) const {
        return objects.find(key) != objects.end();
    }
    
    JsonNode Get(const std::string& key) const {
         auto it = objects.find(key);
         if (it != objects.end()) return it->second;
         return JsonNode();
    }
    
    size_t Size() const {
        if (type == JSON_ARRAY) return array.size();
        return 0;
    }
    
    JsonNode At(size_t index) const {
        if (type == JSON_ARRAY && index < array.size()) return array[index];
        return JsonNode();
    }

    // Serializer
    std::string ToString() const {
        if (type == JSON_STRING) {
            return "\"" + Escape(strValue) + "\"";
        } else if (type == JSON_INT) {
            return std::to_string(intValue);
        } else if (type == JSON_OBJECT) {
            std::stringstream ss;
            ss << "{";
            bool first = true;
            for(auto const& [key, val] : objects) {
                if (!first) ss << ",";
                ss << "\"" << key << "\":" << val.ToString();
                first = false;
            }
            ss << "}";
            return ss.str();
        } else if (type == JSON_ARRAY) {
            std::stringstream ss;
            ss << "[";
            bool first = true;
            for(const auto& item : array) {
                if (!first) ss << ",";
                ss << item.ToString();
                first = false;
            }
            ss << "]";
            return ss.str();
        }
        return "null";
    }

    // Simple Parser
    static JsonNode Parse(const std::string& json) {
        size_t pos = 0;
        SkipSpace(json, pos);
        return ParseValue(json, pos);
    }

private:
    static JsonNode ParseValue(const std::string& json, size_t& pos) {
        SkipSpace(json, pos);
        if (pos >= json.length()) return JsonNode();
        
        char c = json[pos];
        if (c == '{') return ParseObject(json, pos);
        if (c == '[') return ParseArray(json, pos);
        if (c == '"') return JsonNode(ParseString(json, pos));
        if (isdigit(c) || c == '-' || c == '.') return ParseNumber(json, pos);
        
        // true/false/null or invalid
        // Simple fallback
        size_t start = pos;
        while(pos < json.length() && isalnum(json[pos])) pos++;
        std::string val = json.substr(start, pos-start);
        if (val == "true" || val == "false" || val == "null") return JsonNode(val); // Treat as string for simplicity
        
        return JsonNode(val);
    }

    static JsonNode ParseObject(const std::string& json, size_t& pos) {
        JsonNode node;
        node.type = JSON_OBJECT;
        pos++; // skip {
        
        while(pos < json.length()) {
            SkipSpace(json, pos);
            if (json[pos] == '}') { pos++; break; }
            
            std::string key = ParseString(json, pos);
            if (key.empty()) break;
            
            SkipSpace(json, pos);
            if (pos < json.length() && json[pos] == ':') pos++;
            
            node.Set(key, ParseValue(json, pos));
            
            SkipSpace(json, pos);
            if (pos < json.length() && json[pos] == ',') pos++;
            else if (json[pos] == '}') { pos++; break; }
        }
        return node;
    }

    static JsonNode ParseArray(const std::string& json, size_t& pos) {
        JsonNode node;
        node.type = JSON_ARRAY;
        pos++; // skip [
        
        while(pos < json.length()) {
             SkipSpace(json, pos);
             if (json[pos] == ']') { pos++; break; }
             
             node.Add(ParseValue(json, pos));
             
             SkipSpace(json, pos);
             if (pos < json.length() && json[pos] == ',') pos++;
             else if (json[pos] == ']') { pos++; break; }
        }
        return node;
    }

    static JsonNode ParseNumber(const std::string& json, size_t& pos) {
        size_t start = pos;
        while(pos < json.length() && (isdigit(json[pos]) || json[pos] == '-' || json[pos] == '.')) pos++;
        std::string numStr = json.substr(start, pos - start);
        // Basic check for float vs int, simplified to try int then string
        try {
            return JsonNode((long long)std::stoll(numStr));
        } catch(...) {
            return JsonNode(numStr);
        }
    }


private:
    static void SkipSpace(const std::string& s, size_t& pos) {
        while(pos < s.length() && isspace((unsigned char)s[pos])) pos++;
    }

    static std::string ParseString(const std::string& s, size_t& pos) {
        std::string res;
        if (pos >= s.length() || s[pos] != '"') return "";
        pos++; // skip start quote
        while(pos < s.length()) {
            if (s[pos] == '"') {
                pos++; // skip end quote
                return res;
            }
            if (s[pos] == '\\' && pos + 1 < s.length()) {
                pos++; // skip backslash
                // Handle minimal escapes
                if (s[pos] == 'n') res += '\n';
                else if (s[pos] == 'r') res += '\r';
                else if (s[pos] == 't') res += '\t';
                else res += s[pos];
            } else {
                res += s[pos];
            }
            pos++;
        }
        return res;
    }
    
public:
    // JSON 문자열 값 이스케이프 — **모듈 공용 정본**. CSP 의 서비스 로그 기록기
    // (CCallDir::Esc · CSipMessageLogger::JsonEsc)도 이리로 위임한다.
    //
    // 두 가지를 반드시 건다.
    //  (1) 제어문자(<0x20) → \uXXXX. 날것으로 두면 JSON 파싱 자체가 깨진다.
    //  (2) 0x80 이상은 **유효한 UTF-8 시퀀스일 때만** 통과. JSON 은 UTF-8 만 허용한다
    //      (RFC 8259 §8.1). 정상 경로(한글 표시이름 등)는 그대로 지나가고 임의 바이트만
    //      걸린다 — SIP 포트로 들어온 비-SIP 패킷(스캐너의 DTLS ClientHello 등)이 원문
    //      로그에 실리면 그 JSONL 이 UTF-8 이 아니게 되고, 파일을 읽는 통계 집계·이력이
    //      통째로 멈춘다(2026-09-10 실측: 0xfe 한 바이트에 전 서비스 통계가 6시간 정지).
    static std::string Escape(const std::string& s) {
        std::string res;
        res.reserve(s.size() + 16);
        const size_t n = s.size();
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case '"':  res += "\\\""; continue;
                case '\\': res += "\\\\"; continue;
                case '\b': res += "\\b";  continue;
                case '\f': res += "\\f";  continue;
                case '\n': res += "\\n";  continue;
                case '\r': res += "\\r";  continue;
                case '\t': res += "\\t";  continue;
                default: break;
            }
            if (c < 0x20) {
                char h[8];
                snprintf(h, sizeof(h), "\\u%04x", c);
                res += h;
            } else if (c < 0x80) {
                res += (char)c;
            } else {
                // 선두 바이트별 후속 바이트 수. 0x80~0xC1 은 선두가 될 수 없고(0xC0·0xC1 은
                // overlong), 0xF5 이상은 Unicode 범위 밖이다.
                int need = (c >= 0xC2 && c <= 0xDF) ? 1
                         : (c >= 0xE0 && c <= 0xEF) ? 2
                         : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
                bool ok = (need > 0) && (i + (size_t)need < n);
                for (int k = 1; ok && k <= need; k++) {
                    if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
                }
                if (ok) {
                    res.append(s, i, (size_t)need + 1);
                    i += (size_t)need;
                } else {
                    res += "\\ufffd";   // 텍스트가 아니었음만 남긴다 (원 바이트는 복원하지 않는다)
                }
            }
        }
        return res;
    }
};

} // namespace

#endif // __SIMPLE_JSON_H__
