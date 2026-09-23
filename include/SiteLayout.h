#ifndef __SITE_LAYOUT_H__
#define __SITE_LAYOUT_H__

#include <string>

// 사이트 디렉터리 영역 경로 — 모듈 공용 규칙(docs/design/features/site_directory_layout.md).
//   영역 경로는 OAM 실체화가 base oam 의 사이트 디렉터리에서 유도해 각 모듈 config.json 에 넣는다
//   (ems/core/oam/src/services/paths.py 가 유도 정본). 모듈은 자기 영역의 루트만 받고 그 아래 고정
//   하위 이름만 쓴다:
//     log/         sip/<연>/<월>/<일>/<시>/<sysid>*.{msg,flow}.<mm5>.jsonl · leak_reclaim/
//     recordings/  volte/ · ptt/<그룹>/ · message/<gid>/ · message_direct/
//     state/       volte/ · ptt/ (진행 중 세션) · .probe
//     stats/       ptt_attempts/<일>.jsonl
//     content/     announcements/{op,sub}/ · mcdata_fd/
//   영역 키가 비어 있으면 단일 루트 레이아웃(사이트 디렉터리 없는 구성)이다 — 녹취·콘텐츠는 서비스
//   로그 루트, 통계·상태는 그 아래 stats/·state/. paths.py 와 같은 규칙이다.
namespace SiteLayout {

    // log 영역 안 SIP/Flow 5분 버킷 루트의 이름
    static const char *const kSipLogSubdir = "sip";

    // 끝의 '/' 를 떼어 경로 조립 규칙을 하나로 맞춘다 ("/" 자체는 그대로)
    inline std::string Norm( const std::string &strDir ) {
        std::string s = strDir;
        while ( s.size() > 1 && s[s.size() - 1] == '/' ) s.erase( s.size() - 1 );
        return s;
    }

    // SIP/Flow 5분 버킷 루트 — <log>/sip. 서비스 로그가 꺼져 있으면(빈 값) 빈 문자열.
    inline std::string SipLogDir( const std::string &strLogDir ) {
        std::string strLog = Norm( strLogDir );
        return strLog.empty() ? std::string() : strLog + "/" + kSipLogSubdir;
    }

    // 녹취·통신 기록 영역 — Recording.Dir > <log>
    inline std::string RecordingsDir( const std::string &strExplicit, const std::string &strLogDir ) {
        std::string s = Norm( strExplicit );
        return s.empty() ? Norm( strLogDir ) : s;
    }

    // 서비스 콘텐츠 영역 — Content.Dir > <log>
    inline std::string ContentDir( const std::string &strExplicit, const std::string &strLogDir ) {
        std::string s = Norm( strExplicit );
        return s.empty() ? Norm( strLogDir ) : s;
    }

    // 통계·색인 영역 — Stats.Dir > <log>/stats
    inline std::string StatsDir( const std::string &strExplicit, const std::string &strLogDir ) {
        std::string s = Norm( strExplicit );
        if ( !s.empty() ) return s;
        std::string strLog = Norm( strLogDir );
        return strLog.empty() ? std::string() : strLog + "/stats";
    }

    // 휘발성 상태 영역 — State.Dir > <log>/state
    inline std::string StateDir( const std::string &strExplicit, const std::string &strLogDir ) {
        std::string s = Norm( strExplicit );
        if ( !s.empty() ) return s;
        std::string strLog = Norm( strLogDir );
        return strLog.empty() ? std::string() : strLog + "/state";
    }

}  // namespace SiteLayout

#endif
