// Json — cims-tester-worker 의 JSON 값 (파싱·직렬화). 컨트롤러 계약(tester_models.py → schema/*.json)의
// 문서를 읽고 1초 집계(agg)·이벤트·health 를 쓴다. include/SimpleJson.h 는 실수·불리언이 없어 지표
// 직렬화에 못 쓰므로 워커가 자기 것을 가진다(외부 의존 없음).
#ifndef _CIMS_TESTER_JSON_H_
#define _CIMS_TESTER_JSON_H_

#include <map>
#include <string>
#include <vector>

class Json {
public:
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Json() : m_type(NUL), m_b(false), m_d(0) {}
    Json(bool b) : m_type(BOOL), m_b(b), m_d(0) {}
    Json(int i) : m_type(NUMBER), m_b(false), m_d(i) {}
    Json(long long i) : m_type(NUMBER), m_b(false), m_d((double)i) {}
    Json(unsigned long long i) : m_type(NUMBER), m_b(false), m_d((double)i) {}
    Json(double d) : m_type(NUMBER), m_b(false), m_d(d) {}
    Json(const char* s) : m_type(STRING), m_b(false), m_d(0), m_s(s ? s : "") {}
    Json(const std::string& s) : m_type(STRING), m_b(false), m_d(0), m_s(s) {}
    static Json Array() { Json j; j.m_type = ARRAY; return j; }
    static Json Object() { Json j; j.m_type = OBJECT; return j; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == NUL; }
    bool isObject() const { return m_type == OBJECT; }
    bool isArray() const { return m_type == ARRAY; }
    bool isString() const { return m_type == STRING; }
    bool isNumber() const { return m_type == NUMBER; }
    bool isBool() const { return m_type == BOOL; }

    bool asBool(bool def = false) const { return m_type == BOOL ? m_b : (m_type == NUMBER ? m_d != 0 : def); }
    double asDouble(double def = 0) const { return m_type == NUMBER ? m_d : def; }
    long long asInt(long long def = 0) const { return m_type == NUMBER ? (long long)m_d : def; }
    const std::string& asString() const { return m_s; }
    std::string asString(const std::string& def) const { return m_type == STRING ? m_s : def; }

    // 객체
    bool has(const std::string& k) const { return m_type == OBJECT && m_o.count(k) > 0; }
    const Json& operator[](const std::string& k) const;       // 없으면 정적 null
    Json& operator[](const std::string& k);                   // 객체로 승격 후 삽입
    Json& set(const std::string& k, const Json& v) { (*this)[k] = v; return *this; }
    const std::map<std::string, Json>& items() const { return m_o; }
    // 배열
    size_t size() const { return m_type == ARRAY ? m_a.size() : (m_type == OBJECT ? m_o.size() : 0); }
    const Json& at(size_t i) const;
    Json& push(const Json& v);
    const std::vector<Json>& elems() const { return m_a; }

    std::string dump() const;                                  // 한 줄(JSONL 용)
    static bool parse(const std::string& text, Json& out, std::string& err);

private:
    Type m_type;
    bool m_b;
    double m_d;
    std::string m_s;
    std::vector<Json> m_a;
    std::map<std::string, Json> m_o;

    void dumpTo(std::string& out) const;
    static bool parseValue(const std::string& t, size_t& p, Json& out, std::string& err, int depth);
};

#endif
