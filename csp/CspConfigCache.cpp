#include "CspConfigCache.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sstream>
#include <fstream>
#include <vector>

CCspConfigCache gclsCspConfigCache;

namespace {

struct EntityMeta {
    const char* name;
    const char* file;
    const char* apiPath;
};

const EntityMeta kEntityMeta[CACHE_COUNT] = {
    {"listener", "listeners.json", "/api/internal/config/listener"},
    {"trunk",    "trunks.json",    "/api/internal/config/trunk"   },
    {"route",    "routes.json",    "/api/internal/config/route"   },
    {"access",   "access.json",    "/api/internal/config/access"  },
    {"service",  "services.json",  "/api/internal/config/service" },
};

void _mkdirP(const std::string& path) {
    std::string tmp;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            tmp = path.substr(0, i);
            if (!tmp.empty() && tmp != "/") mkdir(tmp.c_str(), 0755);
        }
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────

CCspConfigCache::CCspConfigCache() {}
CCspConfigCache::~CCspConfigCache() {}

const char* CCspConfigCache::EntityName(CspCacheEntity e) {
    return (e >= 0 && e < CACHE_COUNT) ? kEntityMeta[e].name : "unknown";
}

const char* CCspConfigCache::EntityFileName(CspCacheEntity e) {
    return (e >= 0 && e < CACHE_COUNT) ? kEntityMeta[e].file : "unknown.json";
}

bool CCspConfigCache::Init(const std::string& cacheDir,
                            const std::string& cscHost,
                            int cscPort,
                            const std::string& token) {
    m_strCacheDir = cacheDir;
    m_strCscHost  = cscHost.empty() ? "127.0.0.1" : cscHost;
    m_iCscPort    = cscPort > 0 ? cscPort : 4422;
    m_strToken    = token;

    _mkdirP(m_strCacheDir);
    CLog::Print(LOG_INFO, "CspConfigCache: init cacheDir=%s csc=%s:%d",
                m_strCacheDir.c_str(), m_strCscHost.c_str(), m_iCscPort);
    return true;
}

bool CCspConfigCache::LoadInitial() {
    // 1. 로컬 파일 로드 (즉시 사용 가능 상태 보장)
    for (int i = 0; i < CACHE_COUNT; ++i) {
        _loadFromFile(static_cast<CspCacheEntity>(i));
    }
    // 2. CSC 에서 최신 pull 시도 (실패 OK — 로컬 상태 유지)
    bool anyOk = RefreshAll();
    m_bCscReachable = anyOk;
    if (!anyOk) {
        CLog::Print(LOG_ERROR, "CspConfigCache: CSC unreachable at startup — using local cache only");
    }
    return true;
}

bool CCspConfigCache::RefreshAll() {
    bool okAny = false;
    for (int i = 0; i < CACHE_COUNT; ++i) {
        if (RefreshEntity(static_cast<CspCacheEntity>(i))) okAny = true;
    }
    m_bCscReachable = okAny;
    return okAny;
}

bool CCspConfigCache::RefreshEntity(CspCacheEntity e) {
    if (e < 0 || e >= CACHE_COUNT) return false;
    std::string ifNoneMatch;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ifNoneMatch = m_entities[e].etag;
    }
    int status = 0;
    std::string body;
    std::string etag;
    if (!_httpGet(kEntityMeta[e].apiPath, ifNoneMatch, status, body, etag)) {
        m_bCscReachable = false;
        return false;
    }
    m_bCscReachable = true;
    if (status == 304) {
        return true; // 변경 없음
    }
    if (status != 200) {
        CLog::Print(LOG_ERROR, "CspConfigCache: refresh %s status=%d", kEntityMeta[e].name, status);
        return false;
    }
    if (!_applyPullResponse(e, body, etag)) return false;
    _saveToFile(e);
    CLog::Print(LOG_INFO, "CspConfigCache: refreshed %s etag=%s",
                kEntityMeta[e].name, etag.c_str());
    return true;
}

SimpleJson::JsonNode CCspConfigCache::GetItems(CspCacheEntity e) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (e < 0 || e >= CACHE_COUNT) return SimpleJson::JsonNode();
    return m_entities[e].items; // copy
}

std::string CCspConfigCache::GetEtag(CspCacheEntity e) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (e < 0 || e >= CACHE_COUNT) return "";
    return m_entities[e].etag;
}

// ── 파일 I/O ─────────────────────────────────────────────────

bool CCspConfigCache::_loadFromFile(CspCacheEntity e) {
    std::string path = m_strCacheDir + "/" + kEntityMeta[e].file;
    std::ifstream ifs(path);
    if (!ifs) {
        std::lock_guard<std::mutex> lk(m_mutex);
        SimpleJson::JsonNode empty;
        empty.type = SimpleJson::JSON_ARRAY;
        m_entities[e].items = empty;
        m_entities[e].etag.clear();
        m_entities[e].source = "empty";
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    SimpleJson::JsonNode doc = SimpleJson::JsonNode::Parse(content);
    if (doc.type != SimpleJson::JSON_OBJECT) {
        CLog::Print(LOG_ERROR, "CspConfigCache: %s parse failed", path.c_str());
        return false;
    }
    SimpleJson::JsonNode items = doc.Get("items");
    std::string etag = doc.GetString("etag");
    std::lock_guard<std::mutex> lk(m_mutex);
    if (items.type == SimpleJson::JSON_ARRAY) {
        m_entities[e].items = items;
    } else {
        SimpleJson::JsonNode empty;
        empty.type = SimpleJson::JSON_ARRAY;
        m_entities[e].items = empty;
    }
    m_entities[e].etag = etag;
    m_entities[e].source = "file";
    m_entities[e].updatedAt = time(nullptr);
    CLog::Print(LOG_INFO, "CspConfigCache: loaded %s from file (etag=%s)",
                kEntityMeta[e].name, etag.c_str());
    return true;
}

bool CCspConfigCache::_saveToFile(CspCacheEntity e) {
    std::string path = m_strCacheDir + "/" + kEntityMeta[e].file;
    std::lock_guard<std::mutex> lk(m_mutex);
    SimpleJson::JsonNode doc;
    doc.Set("etag", m_entities[e].etag);
    doc.Set("updated_at", (long long)m_entities[e].updatedAt);
    doc.Set("source", m_entities[e].source);
    doc.Set("items", m_entities[e].items);
    return _atomicWriteJson(path, doc.ToString());
}

bool CCspConfigCache::_atomicWriteJson(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "w");
    if (!fp) {
        CLog::Print(LOG_ERROR, "CspConfigCache: open %s failed: %s", tmp.c_str(), strerror(errno));
        return false;
    }
    size_t n = fwrite(content.data(), 1, content.size(), fp);
    if (n != content.size()) {
        fclose(fp);
        unlink(tmp.c_str());
        return false;
    }
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    fclose(fp);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        CLog::Print(LOG_ERROR, "CspConfigCache: rename %s → %s failed: %s",
                    tmp.c_str(), path.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ── 최소 HTTP GET 클라이언트 (loopback plain HTTP) ──────────

bool CCspConfigCache::_httpGet(const std::string& path,
                                const std::string& ifNoneMatch,
                                int& outStatus,
                                std::string& outBody,
                                std::string& outEtag) {
    outStatus = 0;
    outBody.clear();
    outEtag.clear();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_iCscPort);
    inet_pton(AF_INET, m_strCscHost.c_str(), &addr.sin_addr);

    // 연결 타임아웃 2초
    struct timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << m_strCscHost << ":" << m_iCscPort << "\r\n"
        << "X-Csp-Internal-Token: " << m_strToken << "\r\n"
        << "Connection: close\r\n"
        << "Accept: application/json\r\n";
    if (!ifNoneMatch.empty()) {
        req << "If-None-Match: " << ifNoneMatch << "\r\n";
    }
    req << "\r\n";
    std::string reqStr = req.str();

    size_t sent = 0;
    while (sent < reqStr.size()) {
        ssize_t n = send(sock, reqStr.data() + sent, reqStr.size() - sent, 0);
        if (n <= 0) { close(sock); return false; }
        sent += n;
    }

    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);
        if (raw.size() > 16 * 1024 * 1024) break; // 16MB 상한
    }
    close(sock);

    if (raw.empty()) return false;

    // 응답 파싱
    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    std::string headers = raw.substr(0, hdrEnd);
    std::string body = raw.substr(hdrEnd + 4);

    // 첫 줄: HTTP/1.1 200 OK
    size_t firstEol = headers.find("\r\n");
    std::string statusLine = (firstEol != std::string::npos) ? headers.substr(0, firstEol) : headers;
    size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = statusLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) sp2 = statusLine.size();
    outStatus = atoi(statusLine.substr(sp1 + 1, sp2 - sp1 - 1).c_str());

    // ETag 헤더 추출
    std::istringstream iss(headers);
    std::string line;
    bool transferChunked = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 대소문자 구분 없이 ETag 찾기
        if (line.size() >= 5) {
            std::string lower;
            lower.reserve(5);
            for (int k = 0; k < 5; ++k) lower.push_back((char)tolower(line[k]));
            if (lower == "etag:") {
                size_t c = line.find(':');
                std::string v = line.substr(c + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                outEtag = v;
            }
        }
        if (line.size() >= 26) {
            std::string low;
            for (size_t k = 0; k < 26; ++k) low.push_back((char)tolower(line[k]));
            if (low == "transfer-encoding: chunked") transferChunked = true;
        }
    }

    // chunked 디코딩 (단순 처리 — loopback + 소형 payload 가정)
    if (transferChunked) {
        std::string decoded;
        size_t p = 0;
        while (p < body.size()) {
            size_t eol = body.find("\r\n", p);
            if (eol == std::string::npos) break;
            std::string szHex = body.substr(p, eol - p);
            size_t sz = strtoul(szHex.c_str(), nullptr, 16);
            if (sz == 0) break;
            p = eol + 2;
            if (p + sz > body.size()) break;
            decoded.append(body, p, sz);
            p += sz;
            if (p + 2 <= body.size()) p += 2; // CRLF after chunk
        }
        body = decoded;
    }

    outBody = body;
    return true;
}

bool CCspConfigCache::_applyPullResponse(CspCacheEntity e,
                                         const std::string& body,
                                         const std::string& etag) {
    SimpleJson::JsonNode doc = SimpleJson::JsonNode::Parse(body);
    if (doc.type != SimpleJson::JSON_OBJECT) {
        CLog::Print(LOG_ERROR, "CspConfigCache: %s response not JSON object", kEntityMeta[e].name);
        return false;
    }
    SimpleJson::JsonNode items = doc.Get("items");
    if (items.type != SimpleJson::JSON_ARRAY) {
        CLog::Print(LOG_ERROR, "CspConfigCache: %s response missing 'items' array", kEntityMeta[e].name);
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    m_entities[e].items     = items;
    m_entities[e].etag      = etag.empty() ? doc.GetString("etag") : etag;
    m_entities[e].source    = "db";
    m_entities[e].updatedAt = time(nullptr);
    return true;
}
