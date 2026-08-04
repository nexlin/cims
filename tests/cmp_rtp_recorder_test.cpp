// cmp_rtp_recorder_test.cpp — 녹취 세그먼트 메타(PSyncRtpRecorder)의 tracks[] 계약 검증.
//
// 빌드: g++ -std=c++17 -I../cmp tests/cmp_rtp_recorder_test.cpp ../cmp/PSyncRtpRecorder.cpp -o /tmp/rectest
// (PSyncRtpRecorder.cpp 는 PLog.h 외 외부 의존이 없어 단독 링크 가능)
//
// 검증 항목:
//   pttSingleTalker      단일 화자 — 슬롯 0 트랙 1개, 화자 구간 1개, flat 키 호환 유지
//   pttMultiTalker       동시 발언 — 슬롯별 트랙 분리 + 슬롯마다 화자/PT 귀속
//   slotReuseSplitsSpans 한 슬롯을 두 화자가 이어 쓰면 speakers[] 가 2구간으로 갈린다
//   emptyTrackDropped    미디어 없는(keepalive-only 포함) 트랙은 파일·메타에서 제외
//   voipTrackSides       VoIP 는 slot 이 아니라 side(a/b) 로 귀속

#include "PSyncRtpRecorder.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// ── 헬퍼 ──────────────────────────────────────────────────────────

// payload 있는 RTP 패킷 (12바이트 헤더 + payload)
static void writeMedia(PSyncRtpRecorder& r, const char* track, int n = 3) {
    char pkt[60];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (char)0x80;              // V=2, CC=0
    for (int i = 0; i < n; ++i) r.writePacket(track, pkt, 40);
}

// 헤더-only keepalive (payload 없음)
static void writeKeepalive(PSyncRtpRecorder& r, const char* track, int n = 3) {
    char pkt[12];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (char)0x80;
    for (int i = 0; i < n; ++i) r.writePacket(track, pkt, 12);
}

static std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// baseDir 하위에서 segments.jsonl 을 찾아 마지막 줄 반환 (PTT 는 시간버킷 아래)
static std::string lastSegmentLine(const std::string& baseDir) {
    // 재귀 탐색 (깊이 제한 — {YYYY}/{MM}/{DD}/{HH})
    std::vector<std::string> stack{baseDir};
    std::string found;
    while (!stack.empty()) {
        std::string dir = stack.back();
        stack.pop_back();
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string full = dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) stack.push_back(full);
            else if (name == "segments.jsonl") found = full;
        }
        closedir(d);
    }
    if (found.empty()) return "";
    std::string body = readFile(found);
    // 마지막 비어있지 않은 줄
    size_t end = body.find_last_not_of("\n");
    if (end == std::string::npos) return "";
    size_t start = body.find_last_of('\n', end);
    return body.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start));
}

static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// tracks[] 안에서 prefix 가 일치하는 객체 하나를 잘라낸다 (중첩 없는 평면 객체 전제)
static std::string trackObj(const std::string& json, const std::string& prefix) {
    std::string key = "{\"prefix\":\"" + prefix + "\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return "";
    int depth = 0;
    for (size_t i = p; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            if (--depth == 0) return json.substr(p, i - p + 1);
        }
    }
    return "";
}

static std::string mkTmpDir(const char* tag) {
    char tpl[128];
    snprintf(tpl, sizeof(tpl), "/tmp/cims_rectest_%s_XXXXXX", tag);
    char* d = mkdtemp(tpl);
    return d ? std::string(d) : std::string();
}

static void rmTree(const std::string& dir) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (system(cmd.c_str()) != 0) { /* 정리 실패는 테스트 결과와 무관 */ }
}

// ── 테스트 ────────────────────────────────────────────────────────

static void pttSingleTalker() {
    printf("pttSingleTalker\n");
    std::string dir = mkTmpDir("single");
    {
        PSyncRtpRecorder r(dir, "ptt");
        r.addTrack("audio");
        r.addTrack("video");
        r.startPttSegment("01011112222", 5, false, "", 96, "AMR-WB/16000");
        r.setTrackSpeaker("audio", "01011112222");
        r.setTrackSpeaker("video", "01011112222");
        writeMedia(r, "audio");
        r.finishSegment();
    }
    std::string j = lastSegmentLine(dir);
    CHECK(!j.empty(), "segments.jsonl 이 기록되어야 한다");
    CHECK(has(j, "\"speaker_id\":\"01011112222\""), "대표 화자 flat 키 유지");
    CHECK(has(j, "\"audio_file\":"), "audio_file flat 키 유지(구 소비자 호환)");
    CHECK(has(j, "\"audio_pt\":96"), "audio_pt flat 키 유지");
    CHECK(has(j, "\"has_video\":false"), "미디어 없는 영상 트랙은 has_video=false");
    CHECK(!has(j, "\"video_file\":"), "미디어 없는 영상 트랙은 파일 참조 미기록");

    std::string t = trackObj(j, "audio");
    CHECK(!t.empty(), "tracks[] 에 audio 트랙이 있어야 한다");
    CHECK(has(t, "\"kind\":\"audio\""), "kind=audio");
    CHECK(has(t, "\"slot\":0"), "슬롯 0");
    CHECK(has(t, "\"pt\":96"), "트랙 pt");
    CHECK(has(t, "\"codec\":\"AMR-WB/16000\""), "트랙 codec");
    CHECK(has(t, "\"id\":\"01011112222\""), "화자 구간 귀속");
    CHECK(has(t, "\"offset_ms\":0"), "첫 구간 offset=0");
    CHECK(trackObj(j, "video").empty(), "미디어 없는 트랙은 tracks[] 에서 제외");
    rmTree(dir);
}

static void pttMultiTalker() {
    printf("pttMultiTalker\n");
    std::string dir = mkTmpDir("multi");
    {
        PSyncRtpRecorder r(dir, "ptt");
        r.addTrack("audio"); r.addTrack("audio1"); r.addTrack("audio2");
        r.startPttSegment("01011112222", 5, false, "", 96, "AMR-WB/16000");
        r.setTrackSpeaker("audio", "01011112222");
        writeMedia(r, "audio");
        // 동시 발언 합류 — 슬롯 1/2 에 다른 화자·다른 leg PT
        r.setTrackSpeaker("audio1", "01033334444");
        r.setTrackPtCodec("audio1", 99, "AMR-WB/16000");
        writeMedia(r, "audio1");
        r.setTrackSpeaker("audio2", "01055556666");
        r.setTrackPtCodec("audio2", 96, "AMR-WB/16000");
        writeMedia(r, "audio2");
        r.finishSegment();
    }
    std::string j = lastSegmentLine(dir);
    std::string t0 = trackObj(j, "audio"), t1 = trackObj(j, "audio1"), t2 = trackObj(j, "audio2");
    CHECK(!t0.empty() && !t1.empty() && !t2.empty(), "슬롯 3개가 각각 트랙으로 기록");
    CHECK(has(t1, "\"slot\":1") && has(t2, "\"slot\":2"), "슬롯 번호 파생");
    CHECK(has(t1, "\"id\":\"01033334444\""), "슬롯 1 화자 귀속");
    CHECK(has(t2, "\"id\":\"01055556666\""), "슬롯 2 화자 귀속");
    CHECK(has(t1, "\"pt\":99"), "슬롯마다 다른 leg PT 기록(이종 단말)");
    CHECK(has(j, "\"speaker_id_audio1\":\"01033334444\""), "flat 호환 키도 유지");
    rmTree(dir);
}

static void slotReuseSplitsSpans() {
    printf("slotReuseSplitsSpans\n");
    std::string dir = mkTmpDir("reuse");
    {
        PSyncRtpRecorder r(dir, "ptt");
        r.addTrack("audio"); r.addTrack("audio1");
        r.startPttSegment("01011112222", 5, false, "", 96, "AMR-WB/16000");
        r.setTrackSpeaker("audio", "01011112222");
        writeMedia(r, "audio");
        r.setTrackSpeaker("audio1", "01033334444");
        writeMedia(r, "audio1");
        usleep(30000);
        // 슬롯 0 화자가 선점 회수되고 같은 슬롯을 다른 화자가 이어받는다
        r.setTrackSpeaker("audio", "");                 // 구간 종료
        r.setTrackSpeaker("audio", "01099990000");      // 새 화자 진입
        r.setTrackPtCodec("audio", 99, "AMR-WB/16000");
        writeMedia(r, "audio");
        r.finishSegment();
    }
    std::string j = lastSegmentLine(dir);
    std::string t0 = trackObj(j, "audio");
    CHECK(has(t0, "\"id\":\"01011112222\""), "선행 화자 구간 보존");
    CHECK(has(t0, "\"id\":\"01099990000\""), "후행 화자 구간 기록");
    // speakers[] 원소 2개 — offset_ms 가 2번 등장
    size_t first = t0.find("offset_ms");
    size_t second = first == std::string::npos ? std::string::npos : t0.find("offset_ms", first + 1);
    CHECK(second != std::string::npos, "한 트랙에 화자 구간이 2개로 분리되어야 한다");
    CHECK(!has(t0, "\"offset_ms\":0,\"dur_ms\":0"), "선행 구간 길이가 0 이 아니어야 한다");
    // 구 flat 키는 첫 화자로 근사 (대표 화자와 같으므로 이 트랙엔 미기록)
    CHECK(!has(j, "\"speaker_id_audio\":"), "슬롯 0 은 speaker_id 와 같아 flat 키 미기록");
    rmTree(dir);
}

static void emptyTrackDropped() {
    printf("emptyTrackDropped\n");
    std::string dir = mkTmpDir("empty");
    {
        PSyncRtpRecorder r(dir, "ptt");
        r.addTrack("audio"); r.addTrack("video");
        r.startPttSegment("01011112222", 5, false, "", 96, "AMR-WB/16000");
        r.setTrackSpeaker("audio", "01011112222");
        r.setTrackSpeaker("video", "01011112222");
        writeMedia(r, "audio");
        writeKeepalive(r, "video");     // 영상 포트 keepalive 만 — 영상 없음으로 취급
        r.finishSegment();
    }
    std::string j = lastSegmentLine(dir);
    CHECK(has(j, "\"has_video\":false"), "keepalive-only 트랙은 영상 없음");
    CHECK(trackObj(j, "video").empty(), "keepalive-only 트랙은 tracks[] 제외");
    rmTree(dir);
}

static void voipTrackSides() {
    printf("voipTrackSides\n");
    std::string dir = mkTmpDir("voip");
    {
        PSyncRtpRecorder r(dir, "voip", "01011112222", "01033334444");
        r.addTrack("a"); r.addTrack("b"); r.addTrack("va");
        r.setTrackPtCodec("a", 96, "AMR-WB/16000");
        r.setTrackPtCodec("b", 99, "AMR-WB/16000");
        r.startSegment(1);
        writeMedia(r, "a");
        writeMedia(r, "b");
        r.finishSegment();
    }
    std::string j = lastSegmentLine(dir);
    CHECK(has(j, "\"caller\":\"01011112222\""), "VoIP caller 유지");
    CHECK(has(j, "\"audio_file_a\":") && has(j, "\"audio_file_b\":"), "VoIP flat 키 유지");
    std::string ta = trackObj(j, "a"), tb = trackObj(j, "b");
    CHECK(has(ta, "\"side\":\"a\"") && has(tb, "\"side\":\"b\""), "VoIP 는 side 로 귀속");
    CHECK(!has(ta, "\"slot\":"), "VoIP 트랙에 slot 은 없다");
    CHECK(has(ta, "\"pt\":96") && has(tb, "\"pt\":99"), "leg 별 PT 기록");
    CHECK(trackObj(j, "va").empty(), "미디어 없는 영상 leg 제외");
    rmTree(dir);
}

int main() {
    printf("=== CMP RTP recorder 메타 계약 테스트 ===\n");
    pttSingleTalker();
    pttMultiTalker();
    slotReuseSplitsSpans();
    emptyTrackDropped();
    voipTrackSides();
    printf("\n결과: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
