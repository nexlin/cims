#ifndef _RECORD_PATH_H_
#define _RECORD_PATH_H_

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <ctime>
#include <string>

/**
 * @brief CSP가 녹취 디렉터리 경로를 생성하는 유틸리티
 *
 * 디렉터리 구조:
 *   VoIP: {base}/voip/YYYY/MM/DD/{prefix}/{caller}/{HHmmss}_{caller}_{callee}_{id_short}.d/
 *   PTT:  {base}/ptt/YYYY/MM/DD/{prefix}/{caller}/{HHmmss}_{caller}.d/
 *
 *   prefix = caller에서 뒷 2자리를 제거한 앞자리
 */
class CRecordPath {
public:
    /**
     * @brief VoIP 1:1 통화 녹취 디렉터리 경로 생성
     * @param strBaseDir   녹취 루트 (ext_mnt/recordings)
     * @param strCaller    발신자 번호
     * @param strCallee    착신자 번호
     * @param strCallId    SIP Call-ID (짧은 해시용)
     * @return 전체 디렉터리 경로 (mkdir 전)
     */
    static std::string BuildVoipDir( const std::string &strBaseDir, const std::string &strCaller,
                                     const std::string &strCallee, const std::string &strCallId ) {
        std::string yyyy, mm, dd, hhmmss;
        GetDateTimeParts( yyyy, mm, dd, hhmmss );

        std::string prefix = CallerPrefix( strCaller );
        std::string safeCaller = Sanitize( strCaller, 20 );
        std::string safeCallee = Sanitize( strCallee, 20 );
        std::string idShort = Sanitize( strCallId, 16 );

        return strBaseDir + "/voip/" + yyyy + "/" + mm + "/" + dd + "/" + prefix + "/" + safeCaller + "/" + hhmmss +
               "_" + safeCaller + "_" + safeCallee + "_" + idShort + ".d";
    }

    /**
     * @brief PTT 그룹콜 녹취 디렉터리 경로 생성
     * @param strBaseDir   녹취 루트
     * @param strCaller    발신자(그룹콜 개시자) 번호
     * @param strGroupId   그룹 ID (디렉터리명 보충용)
     * @return 전체 디렉터리 경로
     */
    static std::string BuildPttDir( const std::string &strBaseDir, const std::string &strCaller,
                                    const std::string &strGroupId ) {
        std::string yyyy, mm, dd, hhmmss;
        GetDateTimeParts( yyyy, mm, dd, hhmmss );

        std::string prefix = CallerPrefix( strCaller );
        std::string safeCaller = Sanitize( strCaller, 20 );

        return strBaseDir + "/ptt/" + yyyy + "/" + mm + "/" + dd + "/" + prefix + "/" + safeCaller + "/" + hhmmss +
               "_" + safeCaller + ".d";
    }

    /**
     * @brief 메시지 로그 디렉터리 경로 생성
     */
    static std::string BuildMsgLogDir( const std::string &strBaseDir, const std::string &strCaller,
                                       const std::string &strCallee, const std::string &strCallId ) {
        std::string yyyy, mm, dd, hhmmss;
        GetDateTimeParts( yyyy, mm, dd, hhmmss );

        std::string prefix = CallerPrefix( strCaller );
        std::string safeCaller = Sanitize( strCaller, 20 );
        std::string safeCallee = Sanitize( strCallee, 20 );
        std::string idShort = Sanitize( strCallId, 16 );

        std::string dirName = hhmmss + "_" + safeCaller;
        if ( !safeCallee.empty() ) dirName += "_" + safeCallee;
        dirName += "_" + idShort + ".d";

        return strBaseDir + "/" + yyyy + "/" + mm + "/" + dd + "/" + prefix + "/" + safeCaller + "/" + dirName;
    }

    /** @brief 재귀적 디렉터리 생성 */
    static bool MkdirRecursive( const std::string &path ) {
        struct stat st;
        if ( stat( path.c_str(), &st ) == 0 ) return true;
        size_t pos = path.rfind( '/' );
        if ( pos != std::string::npos && pos > 0 ) {
            MkdirRecursive( path.substr( 0, pos ) );
        }
        return mkdir( path.c_str(), 0755 ) == 0 || errno == EEXIST;
    }

private:
    static void GetDateTimeParts( std::string &yyyy, std::string &mm, std::string &dd, std::string &hhmmss ) {
        time_t now = time( nullptr );
        struct tm t;
        localtime_r( &now, &t );
        char buf[8];
        snprintf( buf, sizeof( buf ), "%04d", t.tm_year + 1900 );
        yyyy = buf;
        snprintf( buf, sizeof( buf ), "%02d", t.tm_mon + 1 );
        mm = buf;
        snprintf( buf, sizeof( buf ), "%02d", t.tm_mday );
        dd = buf;
        snprintf( buf, sizeof( buf ), "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec );
        hhmmss = buf;
    }

    static std::string CallerPrefix( const std::string &caller ) {
        std::string s = Sanitize( caller, 20 );
        if ( s.size() <= 2 ) return s;
        return s.substr( 0, s.size() - 2 );
    }

    static std::string Sanitize( const std::string &s, int maxLen ) {
        std::string r;
        r.reserve( s.size() );
        for ( char c : s ) {
            if ( c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                 c == '|' || c == ' ' )
                r += '_';
            else
                r += c;
        }
        if ( (int)r.size() > maxLen ) r = r.substr( 0, maxLen );
        return r;
    }
};

#endif
