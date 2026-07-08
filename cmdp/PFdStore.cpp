#include "PFdStore.h"
#include "PLog.h"
#include "SimpleJson.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

static bool _mkdirP(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return true;
    size_t pos = p.rfind('/');
    if (pos != std::string::npos && pos > 0) _mkdirP(p.substr(0, pos));
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

static std::string _jsonEsc(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            default:   r += c;
        }
    }
    return r;
}

std::string PFdStore::newFileId() {
    unsigned char raw[16];
    bool ok = false;
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        ok = (fread(raw, 1, sizeof(raw), f) == sizeof(raw));
        fclose(f);
    }
    if (!ok) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        srandom((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ getpid()));
        for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = (unsigned char)(random() & 0xff);
    }
    char hex[33];
    for (size_t i = 0; i < sizeof(raw); ++i) snprintf(hex + i * 2, 3, "%02x", raw[i]);
    return std::string(hex, 32);
}

bool PFdStore::Store(const std::string& binContent, const std::string& rawMsrpBody,
                     const std::string& msrpContentType, const std::string& name,
                     const std::string& mime, const std::string& group,
                     const std::string& uploader, std::string& outId) {
    if (_dir.empty()) return false;

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char dateDir[512];
    snprintf(dateDir, sizeof(dateDir), "%s/%04d/%02d/%02d", _dir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    if (!_mkdirP(dateDir) || !_mkdirP(_dir + "/index")) {
        LOG_ERROR("PFdStore", "mkdir failed under %s: %s", _dir.c_str(), strerror(errno));
        return false;
    }

    std::string id = newFileId();
    std::string binPath = std::string(dateDir) + "/" + id + ".bin";
    {
        std::ofstream f(binPath, std::ios::binary);
        if (!f) {
            LOG_ERROR("PFdStore", "open failed: %s", binPath.c_str());
            return false;
        }
        f.write(binContent.data(), (std::streamsize)binContent.size());
    }
    if (!rawMsrpBody.empty()) {
        std::string msrpPath = std::string(dateDir) + "/" + id + ".msrp";
        std::ofstream f(msrpPath, std::ios::binary);
        if (f) f.write(rawMsrpBody.data(), (std::streamsize)rawMsrpBody.size());
    }

    // 메타 — csc mcdata_fd.py 스키마와 키 일치 (+ msrp_content_type 확장). temp+rename 원자 기록.
    char tsBuf[32];
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    std::string meta = "{";
    meta += "\"id\": \"" + id + "\"";
    meta += ", \"name\": \"" + _jsonEsc(name) + "\"";
    meta += ", \"size\": " + std::to_string((long long)binContent.size());
    meta += ", \"type\": \"" + _jsonEsc(mime) + "\"";
    meta += ", \"group\": \"" + _jsonEsc(group) + "\"";
    meta += ", \"uploader\": \"" + _jsonEsc(uploader) + "\"";
    meta += ", \"ts\": \"" + std::string(tsBuf) + "\"";
    meta += ", \"path\": \"" + _jsonEsc(binPath) + "\"";
    if (!msrpContentType.empty())
        meta += ", \"msrp_content_type\": \"" + _jsonEsc(msrpContentType) + "\"";
    meta += "}";

    std::string metaPath = _dir + "/index/" + id + ".json";
    std::string tmpPath = metaPath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        if (!f) {
            LOG_ERROR("PFdStore", "open failed: %s", tmpPath.c_str());
            return false;
        }
        f.write(meta.data(), (std::streamsize)meta.size());
    }
    if (rename(tmpPath.c_str(), metaPath.c_str()) != 0) {
        LOG_ERROR("PFdStore", "rename %s failed: %s", metaPath.c_str(), strerror(errno));
        return false;
    }

    outId = id;
    return true;
}

static bool _loadMetaNode(const std::string& dir, const std::string& id,
                          SimpleJson::JsonNode& node) {
    std::ifstream f(dir + "/index/" + id + ".json");
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    node = SimpleJson::JsonNode::Parse(ss.str());
    return node.type == SimpleJson::JSON_OBJECT;
}

bool PFdStore::LoadRaw(const std::string& id, std::string& body, std::string& contentType) {
    SimpleJson::JsonNode meta;
    if (!_loadMetaNode(_dir, id, meta)) return false;
    std::string binPath = meta.GetString("path");
    contentType = meta.GetString("msrp_content_type");
    if (binPath.size() < 4) return false;
    std::string msrpPath = binPath.substr(0, binPath.size() - 4) + ".msrp";
    std::ifstream f(msrpPath, std::ios::binary);
    if (!f) {
        // MSRP 원문이 없으면(예: HTTP 업로드본) bin 을 그대로 전달
        std::ifstream fb(binPath, std::ios::binary);
        if (!fb) return false;
        std::stringstream ss;
        ss << fb.rdbuf();
        body = ss.str();
        if (contentType.empty()) contentType = meta.GetString("type");
        return true;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    body = ss.str();
    return !contentType.empty();
}

bool PFdStore::LoadMeta(const std::string& id, std::string& name, long long& size,
                        std::string& mime) {
    SimpleJson::JsonNode meta;
    if (!_loadMetaNode(_dir, id, meta)) return false;
    name = meta.GetString("name");
    size = meta.GetInt("size", 0);
    mime = meta.GetString("type");
    return true;
}
