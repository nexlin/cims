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

    // ── VoIP ─────────────────────────────────────────────
    std::string GetVoipDir(const std::string& strCallId,
                            const std::string& strCaller,
                            const std::string& strCallee = "") {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapDir.find(strCallId);
        if (it != m_mapDir.end()) return it->second;
        std::string yyyy, mm, dd, hh;
        DateHour(yyyy, mm, dd, hh);
        std::string sc = San(strCaller, 20);
        std::string dir = m_strCallsDir + "/voip/" + yyyy + "/" + mm + "/" + dd + "/" + hh
                        + "/" + Prefix(sc) + "/" + sc + "/" + San(strCallId, 80) + ".d";
        MkdirP(dir);
        m_mapDir[strCallId] = dir;
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
    std::string GetPttDir(const std::string& strGroupId) {
        std::string yyyy, mm, dd, hh;
        DateHour(yyyy, mm, dd, hh);
        std::string key = strGroupId + "_" + hh;
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_mapDir.find(key);
        if (it != m_mapDir.end()) return it->second;
        std::string sg = San(strGroupId, 20);
        std::string dir = m_strCallsDir + "/ptt/" + yyyy + "/" + mm + "/" + dd + "/" + hh
                        + "/" + Prefix(sg) + "/" + sg + ".d";
        MkdirP(dir);
        m_mapDir[key] = dir;
        return dir;
    }

    void PttSessionStart(const std::string& strGroupId,
                          const std::string& strCallId,
                          const std::string& strInitiator) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _pttDir(strGroupId);
        if (dir.empty()) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        FILE* f = fopen((dir + "/call.jsonl").c_str(), "a");
        if (f) {
            fprintf(f,
                "{\"call_id\":\"%s\",\"group_id\":\"%s\","
                "\"initiator\":\"%s\",\"state\":\"active\","
                "\"start_time\":\"%s\"}\n",
                Esc(strCallId).c_str(), Esc(strGroupId).c_str(),
                Esc(strInitiator).c_str(), ts);
            fclose(f);
        }
    }

    void PttSessionEnd(const std::string& strGroupId, const std::string& strCallId) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _pttDir(strGroupId);
        if (dir.empty()) return;
        _appendIndex(dir, strGroupId, "ptt");
    }

    void PttAddParticipant(const std::string& strGroupId,
                            const std::string& strMsisdn, const std::string& strRole) {
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _pttDir(strGroupId);
        if (dir.empty()) return;
        char ts[32]; IsoNow(ts, sizeof(ts));
        FILE* f = fopen((dir + "/participants.jsonl").c_str(), "a");
        if (f) {
            fprintf(f, "{\"msisdn\":\"%s\",\"role\":\"%s\",\"join_time\":\"%s\",\"leave_time\":null}\n",
                    Esc(strMsisdn).c_str(), strRole.c_str(), ts);
            fclose(f);
        }
    }

    // ── Flow 메시지 기록 ────────────────────────────
    void LogVoip(const std::string& strCallId,
                  const char* from, const char* to,
                  const char* proto, const char* label, const char* body) {
        if (m_strCallsDir.empty()) return;
        GetVoipDir(strCallId, "unknown");
        std::lock_guard<std::mutex> lock(m_mtx);
        std::string dir = _dir(strCallId);
        if (dir.empty()) return;
        _writeJsonl(dir, from, to, proto, label, body);
    }

    void LogPtt(const std::string& strGroupId,
                 const char* from, const char* to,
                 const char* proto, const char* label, const char* body) {
        if (m_strCallsDir.empty()) return;
        std::string dir = GetPttDir(strGroupId);
        if (dir.empty()) return;
        std::lock_guard<std::mutex> lock(m_mtx);
        _writeJsonl(dir, from, to, proto, label, body);
    }

private:
    std::string m_strCallsDir;
    std::string m_strComponent;
    std::mutex  m_mtx;
    std::map<std::string, std::string> m_mapDir;

    std::string _dir(const std::string& key) {
        auto it = m_mapDir.find(key);
        return (it != m_mapDir.end()) ? it->second : "";
    }

    std::string _pttDir(const std::string& strGroupId) {
        std::string yyyy, mm, dd, hh;
        DateHour(yyyy, mm, dd, hh);
        std::string key = strGroupId + "_" + hh;
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
