#ifndef __PANN_CATALOG_H__
#define __PANN_CATALOG_H__

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * 안내음·신호음·보류 음악 카탈로그 (docs/design/features/announcements.md §4.2·§7.1).
 *
 *   음원 하나 = id(`sys:busy_kr` / `op:…` / `sub:…`) + 코덱별 사전 인코딩 파일. 파일은 20 ms 프레임으로 잘라 **전량 메모리에
 *   상주**시킨다(수 초 분량 — 재생 경로에 디스크 I/O 없음). 재생기(PAnnPlayer)는 shared_ptr 스냅샷을 들고 있어 재적재(SIGUSR1·
 *   ANN_RELOAD) 중에도 진행 중 재생이 끊기지 않는다.
 *
 *   프레임 규칙 — PCMU/PCMA/G722 = 160 B/20 ms(G.722 는 RFC 3551 §4.5.2 클록 8000 표기), AMR-WB = RFC 4867 저장 형식 프레임(ToC 1 B +
 *   데이터, "#!AMR-WB\n" 매직 유무 모두 수용). AMR-WB NO_DATA(FT 15) 프레임은 빈 문자열로 남겨 재생기가 패킷을 내지 않고 timestamp
 *   만 진행한다(libcsim 송출기와 같은 규약).
 *
 *   카탈로그는 두 곳을 합친다 — 패키지 동봉 `announcements/sys/catalog.jsonl`(불변) + agent 가 배포한 `config/announcements.jsonl`
 *   (운영자 등록 — collection). id 네임스페이스(sys:/op:/sub:)가 다르므로 충돌하지 않는다. 파일 경로는 announcements 루트 기준 상대 경로.
 */
struct PAnnMedia {
    std::string id;
    std::string kind;         // tone | announcement | music
    std::string description;
    int durationMs = 0;       // 카탈로그 값(없으면 프레임 수 × 20)
    bool loop = false;        // 카탈로그 힌트(재생 여부는 RELAY_PLAY repeat 가 정한다)
    // 코덱 이름(대문자 정규화: PCMU · PCMA · G722 · AMR-WB) → 20 ms 프레임 열
    std::map<std::string, std::vector<std::string>> frames;
    std::map<std::string, std::string> files;   // 코덱 → 파일 상대 경로 (관측·대조용)

    bool hasCodec(const std::string& codec) const { return frames.count(codec) > 0 && !frames.at(codec).empty(); }
    int frameCount(const std::string& codec) const {
        auto it = frames.find(codec);
        return it == frames.end() ? 0 : (int)it->second.size();
    }
};

class PAnnCatalog {
public:
    /** 카탈로그 적재/재적재 — root 아래 `sys/catalog.jsonl` + opJsonl(없으면 무시). 결과는 원자적으로 교체된다.
     *  missing 에는 참조 파일이 없거나 형식이 깨진 항목("id/codec: reason")이 쌓인다 — 그 코덱만 빼고 나머지는 적재. */
    bool load(const std::string& root, const std::string& opJsonl, std::vector<std::string>& missing);

    std::shared_ptr<const PAnnMedia> find(const std::string& id) const;
    std::vector<std::string> ids() const;
    std::vector<std::string> missing() const;
    size_t size() const;
    std::string root() const;

    // ── 순수 함수(단위시험 공용) ──
    /** 코덱 문자열 정규화 — "amr-wb"/"AMRWB"/"AMR-WB/16000" → "AMR-WB", "pcmu"→"PCMU" … 모르는 이름은 빈 문자열 */
    static std::string NormalizeCodec(const std::string& name);
    /** 파일 바이트열 → 20 ms 프레임 열. 반환 false = 형식 오류(길이 불일치·잘린 AMR 프레임) */
    static bool Frame(const std::string& codec, const std::string& bytes, std::vector<std::string>& frames, std::string& err);
    /** 카탈로그 jsonl 한 줄 파싱(프레임 적재 없음) */
    static bool ParseRow(const std::string& line, PAnnMedia& out, std::string& err);
    /** 코덱의 RTP 클록 per 20 ms 프레임 — G.711/G.722 160, AMR-WB 320 */
    static int TsStep(const std::string& codec);

private:
    mutable std::mutex _mtx;
    std::shared_ptr<const std::map<std::string, std::shared_ptr<const PAnnMedia>>> _map;
    std::vector<std::string> _missing;
    std::string _root;
};

#endif  // __PANN_CATALOG_H__
