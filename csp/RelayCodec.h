#ifndef __RELAY_CODEC_H__
#define __RELAY_CODEC_H__

#include <string>
#include <vector>

#include "SdpMedia.h"

/**
 * RelayCodec — VoLTE relay(B2BUA) leg 별 오디오 코덱 협상 헬퍼 (cmp.md §11 피어 leg 트랜스코딩, TS 29.162 코덱 삽입
 * 모델).
 *
 *   서비스 코덱은 AMR-WB 하나다. IP-PBX 트렁크(G.711 필수, SIPconnect 2.0)와 가입자 호는 어느 한쪽이 코덡을 바꿔 줘야
 * 성립하므로 CSP 가 오퍼에 코덱을 **끼워 넣고**(피어→가입자: 서비스 코덱, 가입자→피어: RemoteNode `transcode_codecs`),
 * answer 때 leg 별 최종 코덱이 다르면 CMP 에 `media_codec` 을 실어 변환 유닛을
 * 붙인다([cmp_media_api.md](../docs/api/cmp_media_api.md) §6.6). 같으면 종전 그대로 PT-blind relay 다.
 */
namespace RelayCodec {

    struct CodecDesc {
        std::string name;  // 대문자 정규화 (AMR-WB · PCMU · PCMA · G722 · AMR …)
        int rate = 0;      // clock
        int pt = -1;       // wire PT (-1 = 미정 — InsertCodecs 가 배정)
        std::string fmtp;  // a=fmtp 원문 (PT 뒤 부분)

        bool Valid() const {
            return !name.empty() && pt >= 0 && rate > 0;
        }
        bool Same( const CodecDesc &o ) const {
            return name == o.name && rate == o.rate;
        }
        bool IsAmrWb() const {
            return name == "AMR-WB";
        }
        bool IsG711() const {
            return name == "PCMU" || name == "PCMA";
        }
        std::string Label() const;  // "AMR-WB/16000"
    };

    /** relay leg 하나의 코덱 상태 — CallMap 에 [0]=수신(peer0) [1]=발신(peer1) 로 저장. */
    struct LegCodecs {
        std::vector<CodecDesc> offered;  // 그 leg 가 받은(=CSP 가 그 leg 에 낸) 오퍼의 오디오 코덱, 선호 순
        int tePt = -1;                   // 그 오퍼의 telephone-event PT (-1 없음)
        std::string teRtpmap;            // "telephone-event/8000" 등 rtpmap 값
        std::string teFmtp;
        CodecDesc negotiated;    // answer 뒤 확정 코덱
        bool transcode = false;  // 이 호가 leg 간 변환 중
    };

    /** 첫 active(port>0) audio m= 라인의 코덱 목록 — fmt 순서(선호 순), telephone-event·CN 제외. rtpmap 없는 정적 PT 는
     * RFC 3551 표. */
    std::vector<CodecDesc> AudioCodecs( const SDP_MEDIA_LIST &clsList, int *piTePt = NULL,
                                        std::string *pstrTeRtpmap = NULL, std::string *pstrTeFmtp = NULL );
    /** 서비스 코덱 = 코덱 테이블 top(AMR-WB, csp.json Setup.Media.Codecs) */
    CodecDesc ServiceCodec();
    /** 이름 → 코덱(정적 PT/rate: PCMU 0 · PCMA 8 · G722 9, 그 밖은 코덱 테이블 또는 pt -1). 모르는 이름은 name 비움 */
    CodecDesc ByName( const std::string &strName );
    const CodecDesc *Find( const std::vector<CodecDesc> &vec, const CodecDesc &c );
    /** audio m= 라인 끝에 코덱을 더한다(fmt + rtpmap + fmtp). pt -1 이거나 충돌하면 비어 있는 동적 PT(96..127). 반환 =
     * 더한 수 */
    int InsertCodecs( SDP_MEDIA_LIST &clsList, const std::vector<CodecDesc> &vecAdd );
    /** audio m= 라인을 코덱 하나(+telephone-event)로 다시 쓴다 — fmt 목록·rtpmap·fmtp 교체, 그 밖
     * 속성(ptime·방향·crypto) 유지. answer 를 상대 leg 코덕으로(변환 호의 A-leg 200), 또는 re-offer 를 대상 leg
     * 코덱으로 낼 때. */
    bool RewriteAudio( SDP_MEDIA_LIST &clsList, const CodecDesc &clsCodec, int iTePt, const std::string &strTeRtpmap,
                       const std::string &strTeFmtp );
    /** CMP 가 변환할 수 있는 쌍 (AMR-WB ↔ PCMU/PCMA) */
    bool TranscodablePair( const CodecDesc &a, const CodecDesc &b );
    /** answer leg(B) 가 고른 코덱 negB 와 상대 leg(A) 오퍼 → A 쪽 코덱. 반환 0 = 같은 코덱(relay, codecA = A 오퍼의 그
     * 항목), 1 = 변환(codecA = A 오퍼에서 변환 가능한 첫 코덱), -1 = 성립 불가(488). */
    int DecideLeg( const std::vector<CodecDesc> &vecOfferedA, const CodecDesc &negB, CodecDesc &codecA );

}  // namespace RelayCodec

/** CMP `media_codec`(cmp_media_api.md §6.6) — leg 별 코덱 선언. 양 leg 가 다를 때만 싣는다. */
struct CmpMediaCodec {
    bool bEnabled = false;
    std::string strName;
    int iRate = 0;
    int iPt = 0;
    std::string strFmtp;
    static CmpMediaCodec From( const RelayCodec::CodecDesc &c ) {
        CmpMediaCodec m;
        m.bEnabled = c.Valid();
        m.strName = c.name;
        m.iRate = c.rate;
        m.iPt = c.pt;
        m.strFmtp = c.fmtp;
        return m;
    }
};

#endif  // __RELAY_CODEC_H__
