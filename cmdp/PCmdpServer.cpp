#include "PCmdpServer.h"
#include "FmReporter.h"
#include "PLog.h"
#include "McDataCodec.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

// 비동기 배치 writer 튜닝값 (cmp 와 동일)
static const size_t kCmdpLogNotifyThreshold = 128;
static const size_t kCmdpLogMaxQueue = 200000;
static const int kCmdpLogFlushIntervalMs = 100;

// 이벤트 재전송 정책: 1초 간격 최대 5회 (미ack 시 폐기 — CSP 재기동 구간 손실 허용, 문서화)
static const int kEventMaxAttempts = 5;
static const int kEventRetryIntervalSec = 1;

static const int kMaxConnections = 1024;

PCmdpServer::PCmdpServer(const std::string& name, const std::string& configFile)
    : PModule(name), _configFile(configFile) {
    loadConfig();

    // 이벤트 trans_id 시드 — 부팅 시각(ms) 하위 비트. 재시작 직후 발행 ID 가 구세대와
    //   겹쳐 지연 ack datagram 이 새 이벤트를 오소거하는 창을 제거 (CmpClient trans_id 와 동일 근거).
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        _eventSeq = (long)(((unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000) & 0x3FFFFFFF);
    }

    _reactors.resize(_msrpWorkerCount);
    for (int i = 0; i < _msrpWorkerCount; ++i) {
        _reactors[i].epfd = epoll_create1(0);
        if (_reactors[i].epfd < 0)
            LOG_ERROR("PCmdpServer", "epoll_create1 failed for reactor %d: %s", i, strerror(errno));
    }
}

PCmdpServer::~PCmdpServer() {
    stopServer();
    for (auto const& [id, s] : _sessions) delete s;
    _sessions.clear();
    for (auto* c : _connections) delete c;
    _connections.clear();
    for (auto* c : _tombstones) delete c;
    _tombstones.clear();
    for (auto& r : _reactors) {
        if (r.epfd >= 0) { ::close(r.epfd); r.epfd = -1; }
    }
}

// ═══════════════════════════════════════════════════════════════
//  기동/정지
// ═══════════════════════════════════════════════════════════════

bool PCmdpServer::startServer() {
    // 제어 UDP
    _udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_udpFd < 0) {
        LOG_ERROR("PCmdpServer", "socket() failed: %s", strerror(errno));
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_serverIp.c_str());
    addr.sin_port = htons(_serverPort);
    if (bind(_udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("PCmdpServer", "bind() failed on %s:%d: %s", _serverIp.c_str(), _serverPort,
                  strerror(errno));
        close(_udpFd);
        return false;
    }

    // MSRP TCP 리스너
    _listenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (_listenFd < 0) {
        LOG_ERROR("PCmdpServer", "listen socket() failed: %s", strerror(errno));
        return false;
    }
    int reuse = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = INADDR_ANY;  // 리슨은 전체 — 광고 IP 는 _msrpIp
    laddr.sin_port = htons(_msrpPort);
    if (bind(_listenFd, (struct sockaddr*)&laddr, sizeof(laddr)) < 0 || listen(_listenFd, 64) < 0) {
        LOG_ERROR("PCmdpServer", "MSRP listen failed on :%d: %s", _msrpPort, strerror(errno));
        close(_listenFd);
        _listenFd = -1;
        return false;
    }
    // 리스너를 리액터 0 에 등록
    if (!_reactors.empty() && _reactors[0].epfd >= 0) {
        struct epoll_event ev {};
        ev.events = EPOLLIN;
        ev.data.ptr = static_cast<PHandler*>(&_listenHandler);
        if (epoll_ctl(_reactors[0].epfd, EPOLL_CTL_ADD, _listenFd, &ev) < 0)
            LOG_ERROR("PCmdpServer", "epoll_ctl ADD listener failed: %s", strerror(errno));
    }

    _running = true;
    startLogWriter();
    std::thread([this]() { this->runControlLoop(); }).detach();

    _timeoutThread = std::thread([this]() { this->timeoutLoop(); });

    _reactorRunning = true;
    for (int i = 0; i < (int)_reactors.size(); ++i) {
        int w = i;
        _reactors[i].thread = std::thread([this, w]() { this->reactorLoop(w); });
    }

    LOG_INFO("PCmdpServer", "control %s:%d, MSRP tcp :%d (adv %s), store=%s",
             _serverIp.c_str(), _serverPort, _msrpPort, _msrpIp.c_str(),
             _fdStore.IsEnabled() ? "on" : "off");

    // FM 자기보고 (alarm_self_reporting.md) — OAM FM ingest 로 알람/이벤트 push.
    //   fm_catalog.json 은 설정 파일 옆(dist: config/) — 배치별 후보 순서 탐색.
    if (_fmEnable) {
        std::string confDir = _configFile;
        size_t slash = confDir.find_last_of('/');
        confDir = (slash == std::string::npos) ? "." : confDir.substr(0, slash);
        std::string catalog;
        const std::string cands[2] = {confDir + "/fm_catalog.json", confDir + "/config/fm_catalog.json"};
        for (int i = 0; i < 2; ++i) {
            if (access(cands[i].c_str(), R_OK) == 0) { catalog = cands[i]; break; }
        }
        if (catalog.empty()) catalog = cands[0];  // 부재 시 FmReporter 가 로그로 드러냄
        gclsFmReporter.Init(_fmOamIp, _fmOamPort, _systemId, _nodeName, catalog, _fmSyncSec);
        gclsFmReporter.SendEvent("process_started", "stateChange", "cims/" + _nodeName + "/" + _systemId);
    }
    return true;
}

void PCmdpServer::stopServer() {
    if (gclsFmReporter.IsEnabled())
        gclsFmReporter.SendEvent("process_stopping", "stateChange", "cims/" + _nodeName + "/" + _systemId);
    _running = false;
    _reactorRunning = false;
    for (auto& r : _reactors) {
        if (r.thread.joinable()) r.thread.join();
    }
    if (_timeoutThread.joinable()) _timeoutThread.join();
    gclsFmReporter.Stop();
    stopLogWriter();
    if (_listenFd >= 0) { ::close(_listenFd); _listenFd = -1; }
    if (_udpFd >= 0) { ::close(_udpFd); _udpFd = -1; }
    LOG_INFO("PCmdpServer", "Server stopped");
}

// ═══════════════════════════════════════════════════════════════
//  제어채널 (UDP JSON)
// ═══════════════════════════════════════════════════════════════

void PCmdpServer::runControlLoop() {
    char buf[8192];
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    while (_running) {
        int len = recvfrom(_udpFd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&clientAddr, &addrLen);
        if (len > 0) {
            buf[len] = '\0';
            std::string ip = inet_ntoa(clientAddr.sin_addr);
            int port = ntohs(clientAddr.sin_port);
            handlePacket(buf, len, ip, port);
        }
    }
}

void PCmdpServer::handlePacket(char* buf, int len, const std::string& ip, int port) {
    if (len <= 0) return;
    std::string strPacket(buf, len);

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strPacket);
    if (root.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmdpServer", "Invalid JSON Packet from %s:%d: %s", ip.c_str(), port,
                  strPacket.c_str());
        return;
    }

    std::string peerStr = ip + ":" + std::to_string(port);
    _lastRxSeq = writeMsgLine("RX", peerStr.c_str(), "JSON", strPacket.c_str());

    // envelope v2 강제 — hdr 없는 패킷(구 v1 포함)은 BAD_REQUEST (cmp 와 동일 계약)
    SimpleJson::JsonNode hdr = root.Get("hdr");
    if (hdr.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmdpServer", "Missing hdr from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        sendErr(ip, port, (int)root.GetInt("trans_id", 0), "", "", "BAD_REQUEST", "missing hdr");
        return;
    }

    int ver = (int)hdr.GetInt("ver", 0);
    int transId = (int)hdr.GetInt("trans_id", 0);
    std::string type = hdr.GetString("type");
    std::string cmdUpper = hdr.GetString("cmd");
    std::transform(cmdUpper.begin(), cmdUpper.end(), cmdUpper.begin(), ::toupper);

    // 이벤트 ack — 동일 trans_id 의 type:"response" (cmdp 는 요청을 이벤트로만 발신)
    if (type == "response") {
        PAutoLock lock(_mutex);
        _pendingEvents.erase((long)transId);
        return;
    }

    if (ver != 2) {
        sendErr(ip, port, transId, cmdUpper, hdr.GetString("sesid"),
                "UNSUPPORTED_VER", "supported ver: 2");
        return;
    }
    if (type != "request") {
        sendErr(ip, port, transId, cmdUpper, hdr.GetString("sesid"),
                "BAD_REQUEST", "type must be request");
        return;
    }

    std::string sesid = hdr.GetString("sesid");
    SimpleJson::JsonNode payload = root.Get("payload");
    if (payload.type != SimpleJson::JSON_OBJECT) {
        payload = SimpleJson::JsonNode();
        payload.type = SimpleJson::JSON_OBJECT;
    }

    // 이벤트 회신처 학습 — CSP CmdpClient 소켓 주소
    {
        PAutoLock lock(_mutex);
        _cspIp = ip;
        _cspPort = port;
    }

    if (cmdUpper == "MSRP_ADD") {
        std::string mode = payload.GetString("mode");
        if (mode == "recv")
            processAddRecvSession(payload, ip, port, transId, sesid);
        else if (mode == "send")
            processAddSendSession(payload, ip, port, transId, sesid);
        else
            sendErr(ip, port, transId, cmdUpper, sesid, "BAD_REQUEST", "mode must be recv|send");
    } else if (cmdUpper == "MSRP_MODIFY")
        processModify(payload, ip, port, transId, sesid);
    else if (cmdUpper == "MSRP_REMOVE")
        processRemoveSession(payload, ip, port, transId, sesid);
    else if (cmdUpper == "HEARTBEAT")
        processAlive(payload, ip, port, transId);
    else if (cmdUpper == "STATS")
        processStats(payload, ip, port, transId);
    else {
        LOG_WARN("PCmdpServer", "Unknown CMD: %s from %s:%d", cmdUpper.c_str(), ip.c_str(), port);
        sendErr(ip, port, transId, cmdUpper, sesid, "UNKNOWN_CMD", "unknown command");
    }
}

// v2 응답 빌더 — {hdr:{ver,trans_id,node,cmd,type,status[,code,reason][,sesid,service]}[,payload]}
// sesid 는 호 문맥 명령에서만(빈 값이면 생략), CORE(HEARTBEAT/STATS)는 빈 값 전달.
int PCmdpServer::sendOk(const std::string& ip, int port, int transId, const std::string& cmd,
                        const std::string& sesid, const SimpleJson::JsonNode* body,
                        const char* caller, const char* callee) {
    SimpleJson::JsonNode hdr;
    hdr.Set("ver", 2);
    hdr.Set("trans_id", transId);
    hdr.Set("node", _systemId);
    hdr.Set("cmd", cmd);
    hdr.Set("type", "response");
    hdr.Set("status", "OK");
    if (!sesid.empty()) {
        hdr.Set("sesid", sesid);
        hdr.Set("service", "mcdata");
    }
    SimpleJson::JsonNode env;
    env.Set("hdr", hdr);
    if (body) env.Set("payload", *body);
    return sendResponse(ip, port, env.ToString(), caller, callee);
}

int PCmdpServer::sendErr(const std::string& ip, int port, int transId, const std::string& cmd,
                         const std::string& sesid, const char* code, const char* reason) {
    SimpleJson::JsonNode hdr;
    hdr.Set("ver", 2);
    hdr.Set("trans_id", transId);
    hdr.Set("node", _systemId);
    hdr.Set("cmd", cmd);
    hdr.Set("type", "response");
    hdr.Set("status", "ERROR");
    hdr.Set("code", code);
    hdr.Set("reason", reason);
    if (!sesid.empty()) {
        hdr.Set("sesid", sesid);
        hdr.Set("service", "mcdata");
    }
    SimpleJson::JsonNode env;
    env.Set("hdr", hdr);
    return sendResponse(ip, port, env.ToString());
}

int PCmdpServer::sendResponse(const std::string& ip, int port, const std::string& msg,
                              const char* caller, const char* callee) {
    std::string peerStr = ip + ":" + std::to_string(port);
    int txSeq = writeMsgLine("TX", peerStr.c_str(), "JSON", msg.c_str(), caller, callee);
    if (_udpFd != -1) {
        struct sockaddr_in cliaddr;
        memset(&cliaddr, 0, sizeof(cliaddr));
        cliaddr.sin_family = AF_INET;
        cliaddr.sin_port = htons(port);
        cliaddr.sin_addr.s_addr = inet_addr(ip.c_str());
        if (sendto(_udpFd, msg.c_str(), msg.length(), 0, (struct sockaddr*)&cliaddr,
                   sizeof(cliaddr)) < 0)
            LOG_ERROR("PCmdpServer", "sendto failed to %s:%d: %s", ip.c_str(), port,
                      strerror(errno));
    }
    return txSeq;
}

std::string PCmdpServer::localMsrpPath(const std::string& sessionId) const {
    return "msrp://" + _msrpIp + ":" + std::to_string(_msrpPort) + "/" + sessionId + ";tcp";
}

void PCmdpServer::processAddRecvSession(const SimpleJson::JsonNode& payload, const std::string& ip,
                                        int port, int transId, const std::string& sesid) {
    std::string sessionId = payload.GetString("session_id");
    std::string caller = payload.GetString("caller");
    std::string groupId = payload.GetString("group_id");
    std::string remotePath = payload.GetString("remote_path");
    long long maxSize = payload.GetInt("max_size", 0);

    if (sessionId.empty()) {
        sendErr(ip, port, transId, "MSRP_ADD", sesid, "BAD_REQUEST", "session_id required");
        return;
    }

    PAutoLock lock(_mutex);
    PMsrpSession* s;
    auto it = _sessions.find(sessionId);
    if (it != _sessions.end()) {
        s = it->second;  // 멱등 재-ADD: 기존 경로 반환 (cmp processAdd 계약과 동일)
    } else {
        s = new PMsrpSession(sessionId, PMsrpSession::MODE_RECV);
        s->_caller = caller;
        s->_groupId = groupId;
        s->_callee = "";
        s->_sesid = sesid;
        s->_service = "mcdata";
        s->_remotePath = remotePath;
        s->_localPath = localMsrpPath(sessionId);
        // 유효 상한 = min(cmdp 절대상한, CSP 지정 max_size)
        s->_maxSize = _maxMessageBytes;
        if (maxSize > 0 && maxSize < s->_maxSize) s->_maxSize = maxSize;
        _sessions[sessionId] = s;
    }

    SimpleJson::JsonNode body;
    body.Set("msrp_path", s->_localPath);
    body.Set("local_ip", _msrpIp);
    body.Set("local_port", _msrpPort);
    int txSeq = sendOk(ip, port, transId, "MSRP_ADD", sesid, &body, caller.c_str(), groupId.c_str());
    logFlow(sessionId, "cmdp", "csp", "JSON", "MSRP_ADD recv OK", s->_localPath.c_str(),
            std::to_string(transId).c_str(), sesid.c_str(), txSeq, caller.c_str(), groupId.c_str());
    LOG_INFO("PCmdpServer", "MSRP_ADD recv session=%s caller=%s group=%s max=%lld", sessionId.c_str(),
             caller.c_str(), groupId.c_str(), s->_maxSize);
}

void PCmdpServer::processAddSendSession(const SimpleJson::JsonNode& payload, const std::string& ip,
                                        int port, int transId, const std::string& sesid) {
    std::string sessionId = payload.GetString("session_id");
    std::string fileId = payload.GetString("file_id");
    std::string caller = payload.GetString("caller");
    std::string callee = payload.GetString("callee");
    std::string contentType = payload.GetString("content_type");

    if (sessionId.empty() || fileId.empty()) {
        sendErr(ip, port, transId, "MSRP_ADD", sesid, "BAD_REQUEST", "session_id/file_id required");
        return;
    }

    std::string body, storedCt;
    if (!_fdStore.LoadRaw(fileId, body, storedCt)) {
        sendErr(ip, port, transId, "MSRP_ADD", sesid, "NOT_FOUND", "file not found");
        LOG_WARN("PCmdpServer", "MSRP_ADD send session=%s file=%s not found", sessionId.c_str(),
                 fileId.c_str());
        return;
    }

    PAutoLock lock(_mutex);
    PMsrpSession* s;
    auto it = _sessions.find(sessionId);
    if (it != _sessions.end()) {
        s = it->second;
    } else {
        s = new PMsrpSession(sessionId, PMsrpSession::MODE_SEND);
        s->_caller = caller;
        s->_callee = callee;
        s->_sesid = sesid;
        s->_service = "mcdata";
        s->_fileId = fileId;
        s->_sendBody = body;
        s->_sendContentType = contentType.empty() ? storedCt : contentType;
        s->_sendMsgId = MsrpNewTransId();
        s->_localPath = localMsrpPath(sessionId);
        _sessions[sessionId] = s;
    }

    SimpleJson::JsonNode respBody;
    respBody.Set("msrp_path", s->_localPath);
    respBody.Set("local_ip", _msrpIp);
    respBody.Set("local_port", _msrpPort);
    int txSeq = sendOk(ip, port, transId, "MSRP_ADD", sesid, &respBody, caller.c_str(), callee.c_str());
    logFlow(sessionId, "cmdp", "csp", "JSON", "MSRP_ADD send OK", fileId.c_str(),
            std::to_string(transId).c_str(), sesid.c_str(), txSeq, caller.c_str(), callee.c_str());
    LOG_INFO("PCmdpServer", "MSRP_ADD send session=%s file=%s bytes=%zu callee=%s", sessionId.c_str(),
             fileId.c_str(), s->_sendBody.size(), callee.c_str());
}

// MSRP_MODIFY — 수신자 answer 후 remote_path 확정. 소실 세션 부활 금지 → NOT_FOUND
// (RELAY_MODIFY 와 동일 계약 — 부활 시 UE 가 이미 구 경로를 광고 중이라 정합 불가).
void PCmdpServer::processModify(const SimpleJson::JsonNode& payload, const std::string& ip,
                                int port, int transId, const std::string& sesid) {
    std::string sessionId = payload.GetString("session_id");
    std::string remotePath = payload.GetString("remote_path");

    PAutoLock lock(_mutex);
    auto it = _sessions.find(sessionId);
    if (it == _sessions.end()) {
        sendErr(ip, port, transId, "MSRP_MODIFY", sesid, "NOT_FOUND", "session not found");
        return;
    }
    it->second->_remotePath = remotePath;
    sendOk(ip, port, transId, "MSRP_MODIFY", sesid);
    LOG_INFO("PCmdpServer", "MSRP_MODIFY session=%s path=%s", sessionId.c_str(),
             remotePath.c_str());
    // 수신자 연결이 이미 바인딩돼 있으면 즉시 송신 개시
    if (it->second->_mode == PMsrpSession::MODE_SEND) pumpSendSession(it->second);
}

void PCmdpServer::processRemoveSession(const SimpleJson::JsonNode& payload, const std::string& ip,
                                       int port, int transId, const std::string& sesid) {
    std::string sessionId = payload.GetString("session_id");

    PAutoLock lock(_mutex);
    removeSessionLocked(sessionId);  // 자연 멱등 — 소실 세션 재전송도 OK (RELAY_REMOVE 와 동일)
    sendOk(ip, port, transId, "MSRP_REMOVE", sesid);
    LOG_INFO("PCmdpServer", "MSRP_REMOVE session=%s", sessionId.c_str());
}

// CORE — sesid/service 미포함. 응답 payload.resource 키 목록이 곧 기능 광고 (msrp).
void PCmdpServer::processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port,
                               int transId) {
    (void)payload;
    SimpleJson::JsonNode msrp;
    {
        PAutoLock lock(_mutex);
        msrp.Set("sessions", (int)_sessions.size());
        msrp.Set("connections", (int)_connections.size());
    }
    SimpleJson::JsonNode resource;
    resource.Set("msrp", msrp);
    SimpleJson::JsonNode body;
    body.Set("resource", resource);
    sendOk(ip, port, transId, "HEARTBEAT", "", &body);
}

void PCmdpServer::processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port,
                               int transId) {
    (void)payload;
    PAutoLock lock(_mutex);
    SimpleJson::JsonNode body;
    body.Set("sessions", (int)_sessions.size());
    body.Set("connections", (int)_connections.size());
    body.Set("recv_messages", (long long)_statRecvMessages);
    body.Set("send_messages", (long long)_statSendMessages);
    body.Set("aborted", (long long)_statAborted);
    body.Set("orphan_reclaim", (long long)_statOrphanReclaim);
    body.Set("pending_events", (int)_pendingEvents.size());
    body.Set("log_dropped", (long long)_logDropped.load());
    sendOk(ip, port, transId, "STATS", "", &body);
}

// 세션 제거 공통 — 연결 소켓은 shutdown 만 한다. 삭제는 리액터가 RDHUP 이벤트로
// markClosed → tombstone 경유 (제어 스레드가 직접 delete 하면 리액터 proc 과 UAF 경쟁).
void PCmdpServer::removeSessionLocked(const std::string& sessionId) {
    auto it = _sessions.find(sessionId);
    if (it == _sessions.end()) return;
    PMsrpSession* s = it->second;
    if (s->_conn) {
        s->_conn->_session = nullptr;
        ::shutdown(s->_conn->fd(), SHUT_RDWR);
        s->_conn = nullptr;
    }
    _sessions.erase(it);
    delete s;
}

// ═══════════════════════════════════════════════════════════════
//  cmdp → CSP 이벤트 (ack + 재전송)
// ═══════════════════════════════════════════════════════════════

// v2 이벤트 — {hdr:{ver,trans_id,node,cmd,type:"event",sesid,service},payload}.
// ack = 동일 trans_id 의 type:"response" (cmp_media_api.md §8 과 동형).
void PCmdpServer::emitEvent(const char* name, const SimpleJson::JsonNode& payload,
                            const std::string& sesid) {
    if (_cspIp.empty() || _cspPort <= 0) {
        LOG_WARN("PCmdpServer", "event %s dropped — CSP endpoint unknown", name);
        return;
    }
    long id = ++_eventSeq;
    SimpleJson::JsonNode hdr;
    hdr.Set("ver", 2);
    hdr.Set("trans_id", (long long)id);
    hdr.Set("node", _systemId);
    hdr.Set("cmd", name);
    hdr.Set("type", "event");
    if (!sesid.empty()) {
        hdr.Set("sesid", sesid);
        hdr.Set("service", "mcdata");
    }
    SimpleJson::JsonNode env;
    env.Set("hdr", hdr);
    env.Set("payload", payload);
    std::string json = env.ToString();

    PendingEvent pe;
    pe.json = json;
    pe.attempts = 1;
    pe.nextAt = time(nullptr) + kEventRetryIntervalSec;
    _pendingEvents[id] = pe;

    sendResponse(_cspIp, _cspPort, json);
    LOG_INFO("PCmdpServer", "event %s id=%ld -> %s:%d", name, id, _cspIp.c_str(), _cspPort);
}

void PCmdpServer::retransmitEvents() {
    time_t now = time(nullptr);
    for (auto it = _pendingEvents.begin(); it != _pendingEvents.end();) {
        if (it->second.nextAt > now) { ++it; continue; }
        if (it->second.attempts >= kEventMaxAttempts) {
            LOG_WARN("PCmdpServer", "event id=%ld dropped after %d attempts", it->first,
                     it->second.attempts);
            it = _pendingEvents.erase(it);
            continue;
        }
        it->second.attempts++;
        it->second.nextAt = now + kEventRetryIntervalSec;
        sendResponse(_cspIp, _cspPort, it->second.json);
        ++it;
    }
}

// ═══════════════════════════════════════════════════════════════
//  MSRP 처리 (리액터 스레드)
// ═══════════════════════════════════════════════════════════════

void PCmdpServer::doAccept() {
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept4(_listenFd, (struct sockaddr*)&peer, &plen, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            LOG_WARN("PCmdpServer", "accept failed: %s", strerror(errno));
            return;
        }
        std::string peerStr = std::string(inet_ntoa(peer.sin_addr)) + ":" +
                              std::to_string(ntohs(peer.sin_port));

        PAutoLock lock(_mutex);
        if ((int)_connections.size() >= kMaxConnections) {
            LOG_WARN("PCmdpServer", "connection cap reached — reject %s", peerStr.c_str());
            ::close(fd);
            continue;
        }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        PMsrpConnection* conn = new PMsrpConnection(fd, _reactors[0].epfd, this, peerStr);
        struct epoll_event ev {};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.ptr = static_cast<PHandler*>(conn);
        if (epoll_ctl(_reactors[0].epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR("PCmdpServer", "epoll ADD conn fd=%d failed: %s", fd, strerror(errno));
            delete conn;
            continue;
        }
        _connections.push_back(conn);
        LOG_INFO("PCmdpServer", "MSRP connect from %s (fd=%d, total=%zu)", peerStr.c_str(), fd,
                 _connections.size());
    }
}

bool PCmdpServer::onMsrpFrame(PMsrpConnection* conn, const PMsrpMessage& msg) {
    PAutoLock lock(_mutex);

    // ── 응답 (SEND 세션의 청크 200 등) ──────────────────────────────
    if (msg.kind == PMsrpMessage::MSRP_RESPONSE) {
        PMsrpSession* s = conn->_session;
        if (!s) return true;  // 바인딩 전 응답 — 무시
        s->onSendResponse(msg);
        if (s->_aborted) {
            _statAborted++;
            SimpleJson::JsonNode p;
            p.Set("session_id", s->_sessionId);
            p.Set("status", "failed");
            p.Set("reason", s->_abortReason);
            p.Set("bytes", (long long)s->_sendOffset);
            emitEvent("MSRP_SEND_RESULT", p, s->_sesid);
            return false;  // 연결 종료
        }
        if (s->_completed) {
            _statSendMessages++;
            SimpleJson::JsonNode p;
            p.Set("session_id", s->_sessionId);
            p.Set("status", "ok");
            p.Set("bytes", (long long)s->_sendOffset);
            emitEvent("MSRP_SEND_RESULT", p, s->_sesid);
            logFlow(s->_sessionId, "cmdp", "ue", "MSRP", "SEND complete",
                    std::to_string(s->_sendOffset).c_str(), "", s->_sesid.c_str(), 0,
                    s->_caller.c_str(), s->_callee.c_str());
            return true;  // BYE/REMOVE 는 CSP 몫
        }
        // stop-and-wait: 다음 청크
        std::string frame;
        if (s->nextSendChunk(frame)) conn->queueWrite(frame);
        return true;
    }

    // ── 요청 ────────────────────────────────────────────────────────
    if (msg.method == "REPORT") return true;  // REPORT 는 무응답 (RFC 4975 §7.1.2)

    if (msg.method != "SEND") {
        // 미지원 메서드 — 501
        std::string from = msg.GetHeader("From-Path");
        std::string to = msg.GetHeader("To-Path");
        conn->queueWrite(MsrpBuildResponse(msg.transId, 501, "Not Implemented", from, to));
        return true;
    }

    // 연결→세션 바인딩: 첫 요청의 To-Path 세션부
    if (!conn->_session) {
        std::string toPath = msg.GetHeader("To-Path");
        size_t sp = toPath.find(' ');
        std::string firstUri = (sp == std::string::npos) ? toPath : toPath.substr(0, sp);
        std::string host, sessPart;
        int port = 0;
        bool ok = MsrpParseUri(firstUri, host, port, sessPart);
        PMsrpSession* s = nullptr;
        if (ok) {
            auto it = _sessions.find(sessPart);
            if (it != _sessions.end()) s = it->second;
        }
        if (!s || (s->_conn && !s->_conn->closed() && s->_conn != conn)) {
            LOG_WARN("PCmdpServer", "unbound SEND to %s from %s — 481", firstUri.c_str(),
                     conn->peer().c_str());
            conn->queueWrite(MsrpBuildResponse(msg.transId, 481, "Session Does Not Exist",
                                               msg.GetHeader("From-Path"), toPath));
            return false;
        }
        conn->_session = s;
        s->_conn = conn;
        s->touch();
        logFlow(s->_sessionId, "ue", "cmdp", "MSRP", "bind", conn->peer().c_str(), "",
                s->_sesid.c_str(), 0, s->_caller.c_str(), s->_groupId.c_str());
    }

    PMsrpSession* s = conn->_session;
    std::string respTo = msg.GetHeader("From-Path");

    if (s->_mode == PMsrpSession::MODE_SEND) {
        // 수신자 바인딩용 bodiless SEND — 200 후 송신 개시
        conn->queueWrite(MsrpBuildResponse(msg.transId, 200, "OK", respTo, s->_localPath));
        pumpSendSession(s);
        return true;
    }

    // RECV: 청크 수용
    bool setComplete = false;
    int rc = s->acceptChunk(msg, setComplete);
    conn->queueWrite(MsrpBuildResponse(msg.transId, rc, rc == 200 ? "OK" : "", respTo,
                                       s->_localPath));
    if (rc != 200) {
        _statAborted++;
        SimpleJson::JsonNode p;
        p.Set("session_id", s->_sessionId);
        p.Set("reason", rc == 413 ? "size_exceeded" : "unsupported_media");
        emitEvent("MSRP_ABORTED", p, s->_sesid);
        return false;  // 연결 종료
    }

    // Success-Report 요청 시 메시지('$') 단위 REPORT
    if (msg.contFlag == '$' && msg.GetHeader("Success-Report") == "yes") {
        PMsrpByteRange br = MsrpParseByteRange(msg.GetHeader("Byte-Range"));
        long long total = (br.valid && !br.totalStar) ? br.total : (long long)msg.body.size();
        conn->queueWrite(MsrpBuildReport(MsrpNewTransId(), respTo, s->_localPath,
                                         msg.GetHeader("Message-ID"), 1, total, total, 200, "OK"));
    }

    if (setComplete) finishRecvSession(s);
    return true;
}

// RECV 세트 완성: multipart 정규화 → TLV 파싱 → 저장 → MSG_RECEIVED
void PCmdpServer::finishRecvSession(PMsrpSession* s) {
    std::string body, ct;
    if (!s->buildCombinedBody(body, ct)) return;

    CMcDataSdsInfo info;
    if (!McDataParseBody(ct, body, info)) {
        LOG_WARN("PCmdpServer", "session=%s TLV parse failed (ct=%s, %zu bytes)",
                 s->_sessionId.c_str(), ct.c_str(), body.size());
        _statAborted++;
        SimpleJson::JsonNode p;
        p.Set("session_id", s->_sessionId);
        p.Set("reason", "parse_error");
        emitEvent("MSRP_ABORTED", p, s->_sesid);
        return;
    }

    // 폴백(HTTP) 다운로드 대상 = payload 내용. 텍스트 SDS 면 텍스트 그대로.
    std::string binContent = !info.m_strText.empty() ? info.m_strText : s->_payloadPart;
    std::string mime = !info.m_strText.empty() ? "text/plain; charset=utf-8"
                                               : "application/octet-stream";
    std::string name = "sds_" + (info.m_strMsgId.size() >= 8 ? info.m_strMsgId.substr(0, 8)
                                                             : s->_sessionId) +
                       (!info.m_strText.empty() ? ".txt" : ".bin");

    std::string fileId;
    if (!_fdStore.Store(binContent, body, ct, name, mime, s->_groupId, s->_caller, fileId)) {
        LOG_ERROR("PCmdpServer", "session=%s store failed", s->_sessionId.c_str());
        // FM 자기보고 — 저장 실패 전이 (성공 경로가 close, AlarmOpen/Close 는 멱등)
        gclsFmReporter.AlarmOpen("CIMS-PRC-002", _fmStoreMo);
        SimpleJson::JsonNode p;
        p.Set("session_id", s->_sessionId);
        p.Set("reason", "store_error");
        emitEvent("MSRP_ABORTED", p, s->_sesid);
        return;
    }
    gclsFmReporter.AlarmClose("CIMS-PRC-002", _fmStoreMo);

    _statRecvMessages++;
    SimpleJson::JsonNode p;
    p.Set("session_id", s->_sessionId);
    p.Set("file_id", fileId);
    p.Set("size", (long long)binContent.size());
    p.Set("content_type", ct);
    p.Set("conv_id", info.m_strConvId);
    p.Set("msg_id", info.m_strMsgId);
    p.Set("disposition_req", info.m_iDispositionReq);
    p.Set("msg_type", info.m_iMsgType);
    std::string text = info.m_strText.size() > 512 ? info.m_strText.substr(0, 512) : info.m_strText;
    p.Set("text", text);
    p.Set("file_name", name);
    p.Set("file_type", mime);
    p.Set("caller", s->_caller);
    p.Set("group_id", s->_groupId);
    emitEvent("MSRP_MSG_RECEIVED", p, s->_sesid);
    logFlow(s->_sessionId, "ue", "cmdp", "MSRP", "message received",
            (name + " " + std::to_string(binContent.size()) + "B").c_str(), "",
            s->_sesid.c_str(), 0, s->_caller.c_str(), s->_groupId.c_str());
}

void PCmdpServer::pumpSendSession(PMsrpSession* s) {
    if (s->_mode != PMsrpSession::MODE_SEND || s->_sendStarted) return;
    if (!s->_conn || s->_remotePath.empty()) return;
    std::string frame;
    if (s->nextSendChunk(frame)) {
        s->_conn->queueWrite(frame);
        logFlow(s->_sessionId, "cmdp", "ue", "MSRP", "SEND start",
                std::to_string(s->_sendBody.size()).c_str(), "", s->_sesid.c_str(), 0,
                s->_caller.c_str(), s->_callee.c_str());
    }
}

void PCmdpServer::onConnectionClosed(PMsrpConnection* conn) {
    PAutoLock lock(_mutex);

    if (conn->fd() >= 0 && !_reactors.empty() && _reactors[0].epfd >= 0)
        epoll_ctl(_reactors[0].epfd, EPOLL_CTL_DEL, conn->fd(), nullptr);

    PMsrpSession* s = conn->_session;
    if (s && s->_conn == conn) {
        s->_conn = nullptr;
        if (!s->_completed && !s->_aborted) {
            // 전송/수신 도중 연결 유실 — CSP 에 통지 (다이얼로그 정리 책임은 CSP)
            s->_aborted = true;
            s->_abortReason = "conn_reset";
            _statAborted++;
            SimpleJson::JsonNode p;
            p.Set("session_id", s->_sessionId);
            if (s->_mode == PMsrpSession::MODE_SEND && s->_sendStarted) {
                p.Set("status", "failed");
                p.Set("reason", "conn_reset");
                p.Set("bytes", (long long)s->_sendOffset);
                emitEvent("MSRP_SEND_RESULT", p, s->_sesid);
            } else {
                p.Set("reason", "conn_reset");
                emitEvent("MSRP_ABORTED", p, s->_sesid);
            }
        }
    }
    conn->_session = nullptr;

    auto it = std::find(_connections.begin(), _connections.end(), conn);
    if (it != _connections.end()) _connections.erase(it);
    _tombstones.push_back(conn);
    LOG_INFO("PCmdpServer", "MSRP disconnect %s (remain=%zu)", conn->peer().c_str(),
             _connections.size());
}

// 리액터 스레드 전용 — 이벤트 배치 처리 후 호출 (배치 내 잔여 이벤트의 UAF 방지)
void PCmdpServer::drainTombstones() {
    std::vector<PMsrpConnection*> dead;
    {
        PAutoLock lock(_mutex);
        dead.swap(_tombstones);
    }
    for (auto* c : dead) delete c;
}

void PCmdpServer::reactorLoop(int widx) {
    int epfd = _reactors[widx].epfd;
    if (epfd < 0) return;
    const int MAXEV = 64;
    struct epoll_event evs[MAXEV];
    while (_reactorRunning.load()) {
        int n = epoll_wait(epfd, evs, MAXEV, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("PCmdpServer", "epoll_wait reactor %d failed: %s", widx, strerror(errno));
            break;
        }
        if (n > 0) {
            std::unordered_set<PHandler*> handled;
            for (int i = 0; i < n; ++i) {
                PHandler* h = static_cast<PHandler*>(evs[i].data.ptr);
                if (h && handled.insert(h).second) h->proc();
            }
        }
        drainTombstones();  // 배치 종료 후 지연 삭제
    }
    drainTombstones();
}

// ═══════════════════════════════════════════════════════════════
//  스위퍼 — orphan/idle 세션 회수 + 이벤트 재전송
// ═══════════════════════════════════════════════════════════════

void PCmdpServer::timeoutLoop() {
    int tick = 0;
    while (_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++tick;

        PAutoLock lock(_mutex);
        retransmitEvents();

        if (tick % 5 != 0) continue;
        time_t now = time(nullptr);
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            PMsrpSession* s = it->second;
            const char* reason = nullptr;
            if (!s->_conn && !s->_completed &&
                _orphanReclaimSec > 0 && now - s->_createdAt > _orphanReclaimSec) {
                reason = "orphan";  // TCP 미접속 (setup 실패)
            } else if (_sessionTimeout > 0 && now - s->_lastActivity > _sessionTimeout) {
                reason = "idle";    // REMOVE 누락 (CSP crash/BYE 유실) 안전망
            }
            if (!reason) { ++it; continue; }

            _statOrphanReclaim++;
            LOG_WARN("PCmdpServer", "session=%s reclaimed (%s, age=%lds)", s->_sessionId.c_str(),
                     reason, (long)(now - s->_createdAt));
            if (!s->_completed) {
                SimpleJson::JsonNode p;
                p.Set("session_id", s->_sessionId);
                p.Set("reason", reason);
                emitEvent("MSRP_ABORTED", p, s->_sesid);
            }
            if (s->_conn) {
                s->_conn->_session = nullptr;
                ::shutdown(s->_conn->fd(), SHUT_RDWR);
                s->_conn = nullptr;
            }
            it = _sessions.erase(it);
            delete s;
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  설정
// ═══════════════════════════════════════════════════════════════

// flat dot-path key → root 중첩 경로에 set (cmp/CSP 와 동일 overlay 규칙)
static void _cmdpSetByDotPath(SimpleJson::JsonNode& parent, const std::string& dotPath,
                              const SimpleJson::JsonNode& value) {
    size_t pos = dotPath.find('.');
    if (pos == std::string::npos) {
        parent.Set(dotPath, value);
        return;
    }
    std::string head = dotPath.substr(0, pos);
    std::string rest = dotPath.substr(pos + 1);
    SimpleJson::JsonNode sub = parent.Has(head) ? parent.Get(head) : SimpleJson::JsonNode();
    if (sub.type != SimpleJson::JSON_OBJECT) {
        sub = SimpleJson::JsonNode();
        sub.type = SimpleJson::JSON_OBJECT;
    }
    _cmdpSetByDotPath(sub, rest, value);
    parent.Set(head, sub);
}

void PCmdpServer::loadConfig() {
    std::ifstream t(_configFile);
    if (!t.is_open()) {
        LOG_ERROR("PCmdpServer", "Failed to open config file: %s", _configFile.c_str());
        return;
    }
    std::stringstream buffer;
    buffer << t.rdbuf();
    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(buffer.str());

    // Deployment overlay: install_path/config.json (CIMS_DEPLOYMENT_CONFIG 우선)
    do {
        std::string overlayPath;
        if (const char* env = getenv("CIMS_DEPLOYMENT_CONFIG")) {
            if (*env) overlayPath = env;
        }
        if (overlayPath.empty()) {
            std::string dir = _configFile;
            size_t s = dir.find_last_of('/');
            if (s != std::string::npos) dir = dir.substr(0, s);
            std::string cand = dir + "/../../config.json";
            std::ifstream f(cand);
            if (f) overlayPath = cand;
        }
        if (overlayPath.empty()) break;
        std::ifstream of(overlayPath);
        if (!of) break;
        std::stringstream ob;
        ob << of.rdbuf();
        SimpleJson::JsonNode over = SimpleJson::JsonNode::Parse(ob.str());
        if (over.type != SimpleJson::JSON_OBJECT) break;
        int applied = 0;
        for (const auto& kv : over.objects) {
            _cmdpSetByDotPath(root, kv.first, kv.second);
            ++applied;
        }
        LOG_INFO("PCmdpServer", "overlay applied: %s (%d keys)", overlayPath.c_str(), applied);
    } while (false);

    if (root.Has("ServerIp")) _serverIp = root.GetString("ServerIp");
    if (root.Has("ServerPort")) _serverPort = (int)root.GetInt("ServerPort");
    if (root.Has("MsrpIp")) _msrpIp = root.GetString("MsrpIp");
    if (root.Has("MsrpPort")) _msrpPort = (int)root.GetInt("MsrpPort");
    if (root.Has("MaxMessageBytes")) _maxMessageBytes = root.GetInt("MaxMessageBytes");
    if (root.Has("SessionTimeout")) _sessionTimeout = (int)root.GetInt("SessionTimeout");
    if (root.Has("OrphanReclaimSec")) _orphanReclaimSec = (int)root.GetInt("OrphanReclaimSec");
    if (root.Has("MsrpWorkerCount")) {
        int w = (int)root.GetInt("MsrpWorkerCount");
        if (w >= 1 && w <= 8) _msrpWorkerCount = w;
    }
    if (_msrpIp.empty()) _msrpIp = "127.0.0.1";

    // McDataFd.Dir — CSC 콘텐츠 서버와 공유하는 스토어
    if (root.Has("McDataFd")) {
        SimpleJson::JsonNode fd = root.Get("McDataFd");
        if (fd.Has("Dir") && !fd.GetString("Dir").empty()) _fdStore.Init(fd.GetString("Dir"));
    }

    // 시스템 로그
    std::string logDir = root.Has("LogDir") ? root.GetString("LogDir") : "";
    int logMaxSizeMB = root.Has("LogMaxSizeMB") ? (int)root.GetInt("LogMaxSizeMB") : 10;
    int logMaxFiles = root.Has("LogMaxFiles") ? (int)root.GetInt("LogMaxFiles") : 5;
    if (!logDir.empty()) PLog::Instance().InitFile(logDir, "cmdp", logMaxSizeMB, logMaxFiles);
    if (root.Has("LogLevel")) {
        std::string lvl = root.GetString("LogLevel");
        if (lvl == "DEBUG" || lvl == "debug") PLog::Instance().SetLevel(CMP_LOG_DEBUG);
        else if (lvl == "WARN" || lvl == "warn") PLog::Instance().SetLevel(CMP_LOG_WARN);
        else if (lvl == "ERROR" || lvl == "error") PLog::Instance().SetLevel(CMP_LOG_ERROR);
        else PLog::Instance().SetLevel(CMP_LOG_INFO);
    }

    // 서비스 로그 (flow/msg jsonl)
    if (root.Has("ServiceLogging")) {
        SimpleJson::JsonNode sl = root.Get("ServiceLogging");
        if (sl.Has("Dir")) _serviceLogDir = sl.GetString("Dir");
    }
    if (root.Has("SystemId")) _systemId = root.GetString("SystemId");
    _nodeName = _systemId;
    auto upos = _nodeName.find('_');
    if (upos != std::string::npos) _nodeName = _nodeName.substr(0, upos);

    // FM 자기보고 (alarm_self_reporting.md)
    if (root.Has("Fm")) {
        SimpleJson::JsonNode fm = root.Get("Fm");
        if (fm.Has("Enable")) _fmEnable = (fm.GetString("Enable") == "true");
        if (fm.Has("OamIp")) _fmOamIp = fm.GetString("OamIp");
        if (fm.Has("OamPort")) _fmOamPort = (int)fm.GetInt("OamPort", 9010);
        if (fm.Has("SyncSec")) _fmSyncSec = (int)fm.GetInt("SyncSec", 60);
    }
    _fmStoreMo = "cims/" + _nodeName + "/" + _systemId + "/fd_store";

    // 스토어 미설정 시 ServiceLogging.Dir 기반 기본 경로 (csc 기본값과 동일 규칙)
    if (!_fdStore.IsEnabled() && !_serviceLogDir.empty())
        _fdStore.Init(_serviceLogDir + "/mcdata_fd");

    LOG_INFO("PCmdpServer",
             "Config: control=%s:%d msrp=%s:%d max=%lld timeout=%d orphan=%d workers=%d systemId=%s",
             _serverIp.c_str(), _serverPort, _msrpIp.c_str(), _msrpPort, _maxMessageBytes,
             _sessionTimeout, _orphanReclaimSec, _msrpWorkerCount, _systemId.c_str());
}

// ═══════════════════════════════════════════════════════════════
//  서비스 로그 (cmp 패턴 — 5분 버킷 + 비동기 배치 writer)
// ═══════════════════════════════════════════════════════════════

std::string PCmdpServer::getTimestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06ld", t.tm_hour, t.tm_min, t.tm_sec,
             ts.tv_nsec / 1000);
    return buf;
}

bool PCmdpServer::mkdirP(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return true;
    size_t pos = p.rfind('/');
    if (pos != std::string::npos && pos > 0) mkdirP(p.substr(0, pos));
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string PCmdpServer::bucketSuffix() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", (t.tm_min / 5) * 5);
    return buf;
}

std::string PCmdpServer::flowFilePath() {
    if (_currentHourDir.empty()) return "";
    return _currentHourDir + "/" + _systemId + ".flow." + bucketSuffix() + ".jsonl";
}

std::string PCmdpServer::msgFilePath() {
    if (_currentHourDir.empty()) return "";
    return _currentHourDir + "/" + _systemId + "_csp.msg." + bucketSuffix() + ".jsonl";
}

int PCmdpServer::countFileLines(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int n = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) n++;
    fclose(f);
    return n;
}

void PCmdpServer::ensureBucket() {
    if (_serviceLogDir.empty()) return;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char hourDir[512];
    snprintf(hourDir, sizeof(hourDir), "%s/%04d/%02d/%02d/%02d", _serviceLogDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    std::string bucketKey = std::string(hourDir) + "/" + bucketSuffix();
    if (bucketKey == _currentBucketKey) return;
    _currentBucketKey = bucketKey;
    mkdirP(hourDir);
    _currentHourDir = hourDir;
    _msgSeq = -1;
}

static std::string _jsonEscC(const char* s) {
    if (!s) return "";
    std::string r;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            default:   r += *p;
        }
    }
    return r;
}

int PCmdpServer::writeMsgLine(const char* dir, const char* peer, const char* proto,
                              const char* msg, const char* caller, const char* callee) {
    if (!msg || !msg[0] || _serviceLogDir.empty()) return 0;
    std::lock_guard<std::mutex> lk(_logMtx);
    ensureBucket();
    std::string path = msgFilePath();
    if (path.empty()) return 0;
    if (_msgSeq < 0) _msgSeq = countFileLines(path);
    _msgSeq++;
    int seq = _msgSeq;
    std::string line = "{\"ts\":\"" + getTimestamp() + "\",\"dir\":\"" + (dir ? dir : "") +
                       "\",\"peer\":\"" + (peer ? peer : "") + "\"";
    if (caller && caller[0]) line += ",\"caller\":\"" + _jsonEscC(caller) + "\"";
    if (callee && callee[0]) line += ",\"callee\":\"" + _jsonEscC(callee) + "\"";
    line += ",\"proto\":\"" + std::string(proto ? proto : "") + "\",\"msg\":\"" + _jsonEscC(msg) +
            "\"}\n";
    enqueueLine(path, std::move(line));
    return seq;
}

void PCmdpServer::logFlow(const std::string& key, const char* from, const char* to,
                          const char* proto, const char* label, const char* detail,
                          const char* txId, const char* sesid, int seq,
                          const char* caller, const char* callee) {
    if (_serviceLogDir.empty()) return;
    std::lock_guard<std::mutex> lk(_logMtx);
    ensureBucket();
    std::string flowPath = flowFilePath();
    if (flowPath.empty()) return;

    std::string line = "{";
    bool bFirst = true;
    auto emit = [&](const char* kname, const std::string& val, bool isNum = false, int iNum = 0) {
        if (!isNum && val.empty()) return;
        if (!bFirst) line += ",";
        bFirst = false;
        if (isNum) {
            char nb[64];
            snprintf(nb, sizeof(nb), "\"%s\":%d", kname, iNum);
            line += nb;
        } else {
            line += "\"" + std::string(kname) + "\":\"" + val + "\"";
        }
    };
    emit("ts", getTimestamp());
    emit("service", "mcdata");
    emit("caller", caller ? _jsonEscC(caller) : "");
    emit("callee", callee ? _jsonEscC(callee) : "");
    emit("sesid", sesid ? _jsonEscC(sesid) : "");
    emit("subid", _jsonEscC(key.c_str()));
    emit("node", _nodeName);
    emit("from", from ? std::string(from) : "");
    emit("to", to ? std::string(to) : "");
    emit("proto", proto ? std::string(proto) : "");
    emit("method", label ? _jsonEscC(label) : "");
    emit("detail", detail ? _jsonEscC(detail) : "");
    emit("mid", txId ? _jsonEscC(txId) : "");
    if (seq > 0) emit("seq", "", true, seq);
    line += "}\n";
    enqueueLine(flowPath, std::move(line));
}

void PCmdpServer::enqueueLine(const std::string& path, std::string&& line) {
    if (path.empty() || line.empty()) return;
    bool bNotify = false;
    {
        std::lock_guard<std::mutex> lk(_logQMtx);
        if (_logQueue.size() >= kCmdpLogMaxQueue) {
            _logQueue.pop_front();
            _logDropped.fetch_add(1);
        }
        _logQueue.push_back(LogItem{path, std::move(line)});
        if (_logQueue.size() >= kCmdpLogNotifyThreshold) bNotify = true;
    }
    if (bNotify) _logQCv.notify_one();
}

void PCmdpServer::flushLogBatch(std::deque<LogItem>& batch) {
    if (batch.empty()) return;
    std::unordered_map<std::string, std::string> groups;
    for (auto& item : batch) {
        auto it = groups.find(item.path);
        if (it == groups.end())
            groups.emplace(std::move(item.path), std::move(item.line));
        else
            it->second += item.line;
    }
    batch.clear();
    for (auto& kv : groups) {
        FILE* f = fopen(kv.first.c_str(), "a");
        if (!f) continue;
        fwrite(kv.second.data(), 1, kv.second.size(), f);
        fclose(f);
    }
}

void PCmdpServer::logWriterLoop() {
    const auto interval = std::chrono::milliseconds(kCmdpLogFlushIntervalMs);
    while (_logWriterRunning.load()) {
        std::deque<LogItem> batch;
        {
            std::unique_lock<std::mutex> lk(_logQMtx);
            _logQCv.wait_for(lk, interval,
                             [this] { return !_logQueue.empty() || !_logWriterRunning.load(); });
            batch.swap(_logQueue);
        }
        flushLogBatch(batch);
    }
    for (;;) {
        std::deque<LogItem> batch;
        {
            std::lock_guard<std::mutex> lk(_logQMtx);
            batch.swap(_logQueue);
        }
        if (batch.empty()) break;
        flushLogBatch(batch);
    }
}

void PCmdpServer::startLogWriter() {
    bool bExpected = false;
    if (_logWriterRunning.compare_exchange_strong(bExpected, true))
        _logWriterThread = std::thread(&PCmdpServer::logWriterLoop, this);
}

void PCmdpServer::stopLogWriter() {
    if (_logWriterRunning.exchange(false)) {
        _logQCv.notify_all();
        if (_logWriterThread.joinable()) _logWriterThread.join();
    }
}
