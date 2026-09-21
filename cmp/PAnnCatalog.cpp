#include "PAnnCatalog.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "PLog.h"
#include "PTranscoder.h"
#include "SimpleJson.h"

static const int kFrameG711 = 160;   // 8 kHz × 20 ms (PCMU/PCMA 1 B/sample, G.722 64 kbit/s 도 160 B/20 ms)

std::string PAnnCatalog::NormalizeCodec(const std::string& name) {
    std::string n = name;
    size_t slash = n.find('/');
    if (slash != std::string::npos) n = n.substr(0, slash);
    std::transform(n.begin(), n.end(), n.begin(), ::toupper);
    if (n == "AMRWB" || n == "AMR-WB") return "AMR-WB";
    if (n == "PCMU" || n == "PCMA" || n == "G722") return n;
    return "";
}

int PAnnCatalog::TsStep(const std::string& codec) {
    return codec == "AMR-WB" ? 320 : 160;
}

bool PAnnCatalog::Frame(const std::string& codec, const std::string& bytes, std::vector<std::string>& frames, std::string& err) {
    frames.clear();
    if (codec == "PCMU" || codec == "PCMA" || codec == "G722") {
        if (bytes.empty() || (bytes.size() % kFrameG711) != 0) {
            err = "size not a multiple of 160";
            return false;
        }
        frames.reserve(bytes.size() / kFrameG711);
        for (size_t off = 0; off + kFrameG711 <= bytes.size(); off += kFrameG711) frames.emplace_back(bytes, off, kFrameG711);
        return true;
    }
    if (codec == "AMR-WB") {
        size_t pos = 0;
        static const char kMagic[] = "#!AMR-WB\n";
        if (bytes.compare(0, sizeof(kMagic) - 1, kMagic) == 0) pos = sizeof(kMagic) - 1;
        while (pos < bytes.size()) {
            unsigned char toc = (unsigned char)bytes[pos];
            int ft = (toc >> 3) & 0x0F;
            int n = PTranscoder::amrWbBytes(ft);
            if (ft >= 10 && ft <= 13) {   // 미정의 프레임 타입 (RFC 4867 §4.4.2)
                err = "bad AMR-WB frame type";
                return false;
            }
            if (pos + 1 + (size_t)n > bytes.size()) {
                err = "truncated AMR-WB frame";
                return false;
            }
            if (ft == 15) frames.emplace_back();                 // NO_DATA — 무송신, timestamp 만 진행
            else frames.emplace_back(bytes, pos, 1 + (size_t)n); // 저장 형식 프레임(ToC + 데이터)
            pos += 1 + (size_t)n;
        }
        if (frames.empty()) {
            err = "no AMR-WB frames";
            return false;
        }
        return true;
    }
    err = "unsupported codec";
    return false;
}

bool PAnnCatalog::ParseRow(const std::string& line, PAnnMedia& out, std::string& err) {
    SimpleJson::JsonNode row = SimpleJson::JsonNode::Parse(line);
    if (row.type != SimpleJson::JSON_OBJECT) {
        err = "not a JSON object";
        return false;
    }
    out = PAnnMedia();
    out.id = row.GetString("id");
    if (out.id.empty()) {
        err = "id required";
        return false;
    }
    out.kind = row.GetString("kind", "announcement");
    out.description = row.GetString("description");
    out.durationMs = (int)row.GetInt("duration_ms", 0);
    out.loop = row.GetString("loop") == "true";
    SimpleJson::JsonNode files = row.Get("files");
    if (files.type != SimpleJson::JSON_OBJECT) {
        err = "files required";
        return false;
    }
    for (const auto& kv : files.objects) {
        std::string codec = NormalizeCodec(kv.first);
        if (codec.empty()) continue;   // 모르는 코덱 키(h264 등)는 무시
        out.files[codec] = kv.second.AsString();
    }
    if (out.files.empty()) {
        err = "no usable codec file";
        return false;
    }
    return true;
}

static bool _readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool PAnnCatalog::load(const std::string& root, const std::string& opJsonl, std::vector<std::string>& missing) {
    missing.clear();
    auto fresh = std::make_shared<std::map<std::string, std::shared_ptr<const PAnnMedia>>>();
    const std::string sources[2] = { root + "/sys/catalog.jsonl", opJsonl };
    int rows = 0;
    for (int s = 0; s < 2; ++s) {
        const std::string& path = sources[s];
        if (path.empty()) continue;
        std::ifstream f(path);
        if (!f) {
            if (s == 0) LOG_WARN("PAnnCatalog", "bundled catalog not found: %s", path.c_str());
            continue;
        }
        std::string line;
        int lineNo = 0;
        while (std::getline(f, line)) {
            ++lineNo;
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            PAnnMedia m;
            std::string err;
            if (!ParseRow(line, m, err)) {
                LOG_ERROR("PAnnCatalog", "%s:%d skipped — %s", path.c_str(), lineNo, err.c_str());
                missing.push_back(path + ":" + std::to_string(lineNo) + ": " + err);
                continue;
            }
            for (const auto& kv : m.files) {
                std::string bytes;
                const std::string full = root + "/" + kv.second;
                if (!_readFile(full, bytes)) {
                    missing.push_back(m.id + "/" + kv.first + ": file not found (" + kv.second + ")");
                    LOG_ERROR("PAnnCatalog", "%s: %s file missing: %s", m.id.c_str(), kv.first.c_str(), full.c_str());
                    continue;
                }
                std::vector<std::string> frames;
                if (!Frame(kv.first, bytes, frames, err)) {
                    missing.push_back(m.id + "/" + kv.first + ": " + err);
                    LOG_ERROR("PAnnCatalog", "%s: %s bad format: %s", m.id.c_str(), kv.first.c_str(), err.c_str());
                    continue;
                }
                if (m.durationMs <= 0) m.durationMs = (int)frames.size() * 20;
                m.frames[kv.first] = std::move(frames);
            }
            if (m.frames.empty()) {
                LOG_ERROR("PAnnCatalog", "%s: no playable codec file — dropped", m.id.c_str());
                continue;
            }
            const std::string id = m.id;   // move 전에 키를 떼어 둔다(대입식은 우변이 먼저 평가된다)
            (*fresh)[id] = std::make_shared<const PAnnMedia>(std::move(m));
            ++rows;
        }
    }
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _map = fresh;
        _missing = missing;
        _root = root;
    }
    LOG_INFO("PAnnCatalog", "loaded %d media from %s (missing/bad %d)", rows, root.c_str(), (int)missing.size());
    return true;
}

std::shared_ptr<const PAnnMedia> PAnnCatalog::find(const std::string& id) const {
    std::lock_guard<std::mutex> lk(_mtx);
    if (!_map) return nullptr;
    auto it = _map->find(id);
    return it == _map->end() ? nullptr : it->second;
}

std::vector<std::string> PAnnCatalog::ids() const {
    std::lock_guard<std::mutex> lk(_mtx);
    std::vector<std::string> out;
    if (_map) for (const auto& kv : *_map) out.push_back(kv.first);
    return out;
}

std::vector<std::string> PAnnCatalog::missing() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _missing;
}

size_t PAnnCatalog::size() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _map ? _map->size() : 0;
}

std::string PAnnCatalog::root() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _root;
}
