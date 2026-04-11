#ifndef _CALL_DIR_H_
#define _CALL_DIR_H_

#include <string>
#include <map>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <errno.h>
#include <fstream>

class CCallDir {
public:
    void Init(const std::string& strBaseDir, const std::string& strComponent) {
        if (strBaseDir.empty()) return;
        m_strCallsDir = strBaseDir;
        m_strComponent = strComponent;
        MkdirP(m_strCallsDir);
    }

    bool IsEnabled() const { return !m_strCallsDir.empty(); }

    // ── Session-ID 관리 ────────────────────────────────

    /** Session-ID 생성 (통화 세션 고유 ID) */
    static std::string GenerateSessionId() {
        char buf[64];
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm t;
        localtime_r(&ts.tv_sec, &t);
        snprintf(buf, sizeof(buf), "S%04d%02d%02d%02d%02d%02d%06ld",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec/1000);
        return buf;
    }

    /** Call-ID → Session-ID 매핑 등록 (B2BUA에서 두 leg 모두 등록) */
    void MapCallToSession(const std::string& strCallId, const std::string& strSessionId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_mapCallSession[strCallId] = strSessionId;
    }

    /** Call-ID에서 Session-ID 조회 (없으면 빈 문자열) */
    std::string GetSessionId(const std::string& strCallId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapCallSession.find(strCallId);
        return (it != m_mapCallSession.end()) ? it->second : "";
    }

    /** Session-ID에서 log_dir 조회 */
    std::string GetSessionDir(const std::string& strSessionId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        return _dir(strSessionId);
    }

    /** Write session mapping (session.json) to the .d directory */
    void WriteSessionMapping(const std::string& strSessionId,
                              const std::string& strCallIdA,
                              const std::string& strCallIdB) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strSessionId);
        if (dir.empty()) {
            // Try via call-id
            dir = _dir(strCallIdA);
        }
        if (dir.empty()) return;
        std::string path = dir + "/session.json";
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        fprintf(f, "{\"session_id\":\"%s\",\"call_ids\":[\"%s\",\"%s\"]}\n",
                Esc(strSessionId).c_str(),
                Esc(strCallIdA).c_str(),
                Esc(strCallIdB).c_str());
        fclose(f);
    }

    // ── VoIP ─────────────────────────────────────────────

    /** VoIP 세션 디렉터리 생성. Session-ID 기반. */
    std::string GetVoipDir(const std::string& strCallId,
                            const std::string& strCaller,
                            const std::string& strCallee = "") {
        std::lock_guard<std::mutex> lock(m_mtx);
        // Call-ID → Session-ID 매핑이 있으면 해당 세션 디렉터리 반환
        auto itSess = m_mapCallSession.find(strCallId);
        if (itSess != m_mapCallSession.end()) {
            auto itDir = m_mapDir.find(itSess->second);
            if (itDir != m_mapDir.end()) {
                m_mapDir[strCallId] = itDir->second;
                return itDir->second;
            }
            // Session-ID는 있지만 디렉터리 없음 — 발신 leg으로 다시 조회
            for (auto& [cid, sid] : m_mapCallSession) {
                if (sid == itSess->second && cid != strCallId) {
                    auto itDir2 = m_mapDir.find(cid);
                    if (itDir2 != m_mapDir.end()) {
                        m_mapDir[itSess->second] = itDir2->second;
                        m_mapDir[strCallId] = itDir2->second;
                        return itDir2->second;
                    }
                }
            }
        }
        // 기존 Call-ID로도 조회
        auto it = m_mapDir.find(strCallId);
        if (it != m_mapDir.end()) return it->second;

        // 새 디렉터리 생성 (Session-ID가 있으면 사용, 없으면 Call-ID)
        std::string key = strCallId;
        if (itSess != m_mapCallSession.end()) key = itSess->second;

        std::string yyyy, mm, dd, hh;
        DateHour(yyyy, mm, dd, hh);
        std::string sc = San(strCaller, 20);
        std::string dir = m_strCallsDir + "/voip/" + yyyy + "/" + mm + "/" + dd + "/" + hh
                        + "/" + Prefix(sc) + "/" + sc + "/" + San(key, 80) + ".d";
        MkdirP(dir);
        m_mapDir[key] = dir;
        // Call-ID로도 같은 디렉터리를 찾을 수 있게
        if (key != strCallId) m_mapDir[strCallId] = dir;
        return dir;
    }

    void VoipCallStart(const std::string& strCallId,
                        const std::string& strCaller, const std::string& strCallee) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strCallId);
        if (dir.empty()) return;
        std::string path = dir + "/call.json";
        struct stat st; if (stat(path.c_str(), &st) == 0) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        fprintf(f,
            "{\"call_id\":\"%s\",\"call_type\":\"voip\","
            "\"initiator\":\"%s\",\"callee\":\"%s\","
            "\"state\":\"ringing\",\"invite_time\":\"%s\","
            "\"answer_time\":null,\"end_time\":null,"
            "\"duration\":0,\"end_reason\":null}\n",
            Esc(strCallId).c_str(), Esc(strCaller).c_str(), Esc(strCallee).c_str(), ts);
        fclose(f);
    }

    void VoipCallAnswer(const std::string& strCallId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strCallId);
        if (dir.empty()) return;
        std::string path = dir + "/call.json";
        std::string c = _readFile(path);
        if (c.empty()) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        _replace(c, "\"state\":\"ringing\"", "\"state\":\"active\"");
        _replace(c, "\"answer_time\":null", std::string("\"answer_time\":\"") + ts + "\"");
        FILE* f = fopen(path.c_str(), "w");
        if (f) { fputs(c.c_str(), f); fclose(f); }
    }

    void VoipCallEnd(const std::string& strCallId, const std::string& strReason = "normal", int iDur = 0) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strCallId);
        if (dir.empty()) return;
        _updateCallJson(dir, strReason, iDur);
        _appendIndex(dir, strCallId, "voip");
        m_mapDir.erase(strCallId);
    }

    void VoipAddParticipant(const std::string& strCallId,
                             const std::string& strMsisdn, const std::string& strRole) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strCallId);
        if (dir.empty()) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        FILE* f = fopen((dir + "/participants.jsonl").c_str(), "a");
        if (f) {
            fprintf(f, "{\"msisdn\":\"%s\",\"role\":\"%s\",\"join_time\":\"%s\",\"leave_time\":null}\n",
                    Esc(strMsisdn).c_str(), strRole.c_str(), ts);
            fclose(f);
        }
    }

    // ── PTT ──────────────────────────────────────────────

    /** PTT 세션 디렉터리 (groupId → active session dir) */
    std::string GetPttSessionDir(const std::string& strGroupId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapPttSession.find(strGroupId);
        if (it != m_mapPttSession.end()) return it->second;

        // 새 세션 디렉터리 생성
        char tsBuf[32];
        {
            time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
            snprintf(tsBuf, sizeof(tsBuf), "%04d%02d%02d_%02d%02d%02d",
                     t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        }
        std::string sg = San(strGroupId, 20);
        std::string dir = m_strCallsDir + "/ptt/" + sg + "/sessions/" + tsBuf + ".d";
        MkdirP(dir);
        MkdirP(dir + "/recordings");
        MkdirP(dir + "/daily");
        m_mapPttSession[strGroupId] = dir;
        m_mapPttSessionId[strGroupId] = tsBuf;
        return dir;
    }

    void PttSessionStart(const std::string& strGroupId,
                          const std::string& strCallId,
                          const std::string& strInitiator,
                          const std::string& strGroupJson = "{}") {
        std::lock_guard<std::mutex> lock(m_mtx);
        // Ensure session dir exists (unlocked helper)
        std::string dir;
        {
            auto it = m_mapPttSession.find(strGroupId);
            if (it != m_mapPttSession.end()) {
                dir = it->second;
            }
        }
        if (dir.empty()) {
            // create via unlock-relock pattern not needed since _pttSessionDirLocked does not lock
            char tsBuf[32];
            {
                time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
                snprintf(tsBuf, sizeof(tsBuf), "%04d%02d%02d_%02d%02d%02d",
                         t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
            }
            std::string sg = San(strGroupId, 20);
            dir = m_strCallsDir + "/ptt/" + sg + "/sessions/" + tsBuf + ".d";
            MkdirP(dir);
            MkdirP(dir + "/recordings");
            MkdirP(dir + "/daily");
            m_mapPttSession[strGroupId] = dir;
            m_mapPttSessionId[strGroupId] = tsBuf;
        }

        char ts[32]; IsoNow(ts, sizeof(ts));
        // Write session.json
        std::string path = dir + "/session.json";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f,
                "{\"session_id\":\"%s\",\"group_id\":\"%s\","
                "\"call_id\":\"%s\",\"initiator\":\"%s\","
                "\"state\":\"active\",\"start_time\":\"%s\","
                "\"group_snapshot\":%s}\n",
                Esc(m_mapPttSessionId[strGroupId]).c_str(),
                Esc(strGroupId).c_str(),
                Esc(strCallId).c_str(),
                Esc(strInitiator).c_str(), ts,
                strGroupJson.c_str());
            fclose(f);
        }
        // Append to group index.jsonl
        std::string sg = San(strGroupId, 20);
        std::string indexPath = m_strCallsDir + "/ptt/" + sg + "/index.jsonl";
        f = fopen(indexPath.c_str(), "a");
        if (f) {
            fprintf(f, "{\"ts\":\"%s\",\"type\":\"session_start\",\"session_id\":\"%s\","
                    "\"call_id\":\"%s\",\"initiator\":\"%s\"}\n",
                    ts, Esc(m_mapPttSessionId[strGroupId]).c_str(),
                    Esc(strCallId).c_str(), Esc(strInitiator).c_str());
            fclose(f);
        }
    }

    void PttSessionEnd(const std::string& strGroupId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapPttSession.find(strGroupId);
        if (it == m_mapPttSession.end()) return;
        std::string dir = it->second;

        // Read session.json, update state→ended, write end_time
        std::string path = dir + "/session.json";
        std::string c = _readFile(path);
        if (!c.empty()) {
            char ts[32]; IsoNow(ts, sizeof(ts));
            _replace(c, "\"state\":\"active\"", "\"state\":\"ended\"");
            // Insert end_time before closing brace
            size_t lastBrace = c.rfind('}');
            if (lastBrace != std::string::npos) {
                c.insert(lastBrace, std::string(",\"end_time\":\"") + ts + "\"");
            }
            FILE* f = fopen(path.c_str(), "w");
            if (f) { fputs(c.c_str(), f); fclose(f); }

            // Append end event to index.jsonl
            std::string sg = San(strGroupId, 20);
            std::string indexPath = m_strCallsDir + "/ptt/" + sg + "/index.jsonl";
            f = fopen(indexPath.c_str(), "a");
            if (f) {
                auto itId = m_mapPttSessionId.find(strGroupId);
                std::string sid = (itId != m_mapPttSessionId.end()) ? itId->second : "";
                fprintf(f, "{\"ts\":\"%s\",\"type\":\"session_end\",\"session_id\":\"%s\"}\n",
                        ts, Esc(sid).c_str());
                fclose(f);
            }
        }

        m_mapPttSession.erase(strGroupId);
        m_mapPttSessionId.erase(strGroupId);
    }

    void PttLogEvent(const std::string& strGroupId,
                     const std::string& strType,
                     const std::string& strJsonData) {
        if (m_strCallsDir.empty()) return;
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapPttSession.find(strGroupId);
        if (it == m_mapPttSession.end()) return;
        std::string dir = it->second;
        char ts[32]; IsoNow(ts, sizeof(ts));

        // Build merged line: {"ts":"...","type":"...", ...data fields}
        std::string line = "{\"ts\":\"" + std::string(ts) + "\",\"type\":\"" + Esc(strType) + "\"";
        // Merge jsonData fields (strip outer braces)
        if (!strJsonData.empty() && strJsonData.front() == '{' && strJsonData.back() == '}') {
            std::string inner = strJsonData.substr(1, strJsonData.size() - 2);
            if (!inner.empty()) {
                line += "," + inner;
            }
        }
        line += "}";

        // Append to events.jsonl
        FILE* f = fopen((dir + "/events.jsonl").c_str(), "a");
        if (f) { fprintf(f, "%s\n", line.c_str()); fclose(f); }

        // Append to daily/YYYY-MM-DD.jsonl
        std::string dailyFile = dir + "/daily/" + std::string(ts, 10) + ".jsonl";
        f = fopen(dailyFile.c_str(), "a");
        if (f) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
    }

    // ── Flow 메시지 기록 ────────────────────────────
    void LogVoip(const std::string& strCallId,
                  const char* from, const char* to,
                  const char* proto, const char* label, const char* body) {
        if (m_strCallsDir.empty()) return;
        std::string dir = GetVoipDir(strCallId, "unknown");
        if (dir.empty()) return;
        _writeJsonlSafe(dir, from, to, proto, label, body);
    }

    void _writeJsonlSafe(const std::string& dir, const char* from, const char* to,
                          const char* proto, const char* label, const char* body) {
        std::lock_guard<std::mutex> lock(m_mtx);
        _writeJsonl(dir, from, to, proto, label, body);
    }

    void LogPtt(const std::string& strGroupId,
                 const char* from, const char* to,
                 const char* proto, const char* label, const char* body) {
        if (m_strCallsDir.empty()) return;
        std::string dir = GetPttSessionDir(strGroupId);
        if (dir.empty()) return;
        std::lock_guard<std::mutex> lock(m_mtx);
        _writeJsonl(dir, from, to, proto, label, body);
    }

    /** JSON string escaper (public for external callers) */
    static std::string JsonEsc(const std::string& s) { return Esc(s); }

private:
    std::string m_strCallsDir;
    std::string m_strComponent;
    std::mutex  m_mtx;
    std::map<std::string, std::string> m_mapDir;          // key(sessionId or callId) → dir path
    std::map<std::string, std::string> m_mapCallSession;  // callId → sessionId
    std::map<std::string, std::string> m_mapPttSession;   // groupId → active session dir
    std::map<std::string, std::string> m_mapPttSessionId; // groupId → session start time string

    std::string _dir(const std::string& key) {
        auto it = m_mapDir.find(key);
        return (it != m_mapDir.end()) ? it->second : "";
    }

    void _writeJsonl(const std::string& dir, const char* from, const char* to,
                      const char* proto, const char* label, const char* body) {
        char ts[32]; Timestamp(ts, sizeof(ts));
        std::string line =
            std::string("{\"ts\":\"") + ts + "\","
            "\"from\":\"" + (from ? from : "") + "\","
            "\"to\":\"" + (to ? to : "") + "\","
            "\"proto\":\"" + (proto ? proto : "") + "\","
            "\"label\":\"" + Esc(label ? label : "") + "\","
            "\"body\":\"" + Esc(body ? body : "") + "\"}";
        FILE* f = fopen((dir + "/" + m_strComponent + ".jsonl").c_str(), "a");
        if (f) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
    }

    void _updateCallJson(const std::string& dir, const std::string& reason, int dur) {
        std::string path = dir + "/call.json";
        std::string c = _readFile(path);
        if (c.empty()) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        _replace(c, "\"state\":\"ringing\"", "\"state\":\"ended\"");
        _replace(c, "\"state\":\"active\"", "\"state\":\"ended\"");
        _replace(c, "\"end_time\":null", std::string("\"end_time\":\"") + ts + "\"");
        _replace(c, "\"duration\":0", "\"duration\":" + std::to_string(dur));
        _replace(c, "\"end_reason\":null", std::string("\"end_reason\":\"") + reason + "\"");
        FILE* f = fopen(path.c_str(), "w");
        if (f) { fputs(c.c_str(), f); fclose(f); }
    }

    void _appendIndex(const std::string& dir, const std::string& id, const std::string& type) {
        std::string hhDir = dir;
        int levels = (type == "voip") ? 3 : 2;
        for (int i = 0; i < levels; ++i) {
            size_t pos = hhDir.rfind('/');
            if (pos != std::string::npos) hhDir = hhDir.substr(0, pos);
        }
        char ts[32]; IsoNow(ts, sizeof(ts));
        FILE* f = fopen((hhDir + "/index.json").c_str(), "a");
        if (f) {
            std::string dn = dir.substr(dir.rfind('/') + 1);
            fprintf(f, "{\"dir\":\"%s\",\"type\":\"%s\",\"id\":\"%s\",\"time\":\"%s\"}\n",
                    Esc(dn).c_str(), type.c_str(), Esc(id).c_str(), ts);
            fclose(f);
        }
    }

    static std::string _readFile(const std::string& p) {
        std::ifstream f(p); if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    static void _replace(std::string& s, const std::string& a, const std::string& b) {
        auto p = s.find(a); if (p != std::string::npos) s.replace(p, a.size(), b);
    }
    static void DateHour(std::string& y, std::string& m, std::string& d, std::string& h) {
        time_t now = time(nullptr); struct tm t; localtime_r(&now, &t); char b[8];
        snprintf(b,8,"%04d",t.tm_year+1900); y=b;
        snprintf(b,8,"%02d",t.tm_mon+1); m=b;
        snprintf(b,8,"%02d",t.tm_mday); d=b;
        snprintf(b,8,"%02d",t.tm_hour); h=b;
    }
    static void IsoNow(char* b, int l) {
        time_t n=time(nullptr); struct tm t; localtime_r(&n,&t);
        snprintf(b,l,"%04d-%02d-%02dT%02d:%02d:%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
    }
    static void Timestamp(char* b, int l) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); struct tm t; localtime_r(&ts.tv_sec,&t);
        snprintf(b,l,"%02d:%02d:%02d.%06ld",t.tm_hour,t.tm_min,t.tm_sec,ts.tv_nsec/1000);
    }
    static std::string Prefix(const std::string& s) { return s.size()<=2 ? s : s.substr(0,s.size()-2); }
    static std::string San(const std::string& s, int mx) {
        std::string r; r.reserve(s.size());
        for (char c:s) { r += (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|'||c==' ') ? '_' : c; }
        return (int)r.size()>mx ? r.substr(0,mx) : r;
    }
    static std::string Esc(const std::string& s) {
        std::string r; r.reserve(s.size()+16);
        for (unsigned char c:s) { switch(c){case '"':r+="\\\"";break;case '\\':r+="\\\\";break;
        case '\n':r+="\\n";break;case '\r':r+="\\r";break;case '\t':r+="\\t";break;
        default:if(c<0x20){char h[8];snprintf(h,8,"\\u%04x",c);r+=h;}else r+=(char)c;}}
        return r;
    }
    static bool MkdirP(const std::string& p) {
        struct stat st; if(stat(p.c_str(),&st)==0) return true;
        size_t pos=p.rfind('/');
        if(pos!=std::string::npos&&pos>0) MkdirP(p.substr(0,pos));
        return mkdir(p.c_str(),0755)==0||errno==EEXIST;
    }
};

extern CCallDir gclsCallDir;

#endif
