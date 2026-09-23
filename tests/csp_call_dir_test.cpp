/*
 * csp_call_dir_test — CCallDir 사이트 영역 경로 단위시험 (site_directory_layout.md)
 *   통화·세션 기록이 녹취(recordings)·상태(state)·통계(stats) 세 영역에 나뉘어 쓰이는지, 영역 키가 비었을 때
 *   단일 루트 규칙(include/SiteLayout.h)으로 해석되는지 본다.
 *
 * 빌드 (레포 루트):
 *   g++ -std=c++17 -Icsp -Iinclude -Iext/psip/SipPlatform tests/csp_call_dir_test.cpp \
 *       build/csp/psip_build/libSipPlatform.a -lpthread -o build/csp_call_dir_test && build/csp_call_dir_test
 */
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "CallDir.h"
#include "SiteLayout.h"

static int g_iPass = 0;

#define CHECK( cond, name )                                  \
    do {                                                     \
        if ( cond ) {                                        \
            ++g_iPass;                                       \
            printf( "PASS %s\n", name );                     \
        } else {                                             \
            printf( "FAIL %s (line %d)\n", name, __LINE__ ); \
            return 1;                                        \
        }                                                    \
    } while ( 0 )

static bool Exists( const std::string &strPath ) {
    struct stat st;
    return stat( strPath.c_str(), &st ) == 0;
}

static std::string ReadAll( const std::string &strPath ) {
    std::ifstream f( strPath );
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool StartsWith( const std::string &s, const std::string &strPrefix ) {
    return s.compare( 0, strPrefix.size(), strPrefix ) == 0;
}

// strDir 아래(재귀)에서 이름이 strName 인 첫 파일 경로 — 시간 버킷 이름을 시험이 계산하지 않도록.
static std::string FindFile( const std::string &strDir, const std::string &strName ) {
    DIR *d = opendir( strDir.c_str() );
    if ( !d ) return "";
    std::string strFound;
    struct dirent *ent;
    while ( strFound.empty() && ( ent = readdir( d ) ) != nullptr ) {
        if ( strcmp( ent->d_name, "." ) == 0 || strcmp( ent->d_name, ".." ) == 0 ) continue;
        std::string strPath = strDir + "/" + ent->d_name;
        struct stat st;
        if ( stat( strPath.c_str(), &st ) != 0 ) continue;
        if ( S_ISDIR( st.st_mode ) ) {
            strFound = FindFile( strPath, strName );
        } else if ( strName == ent->d_name || ( strName[0] == '*' && strlen( ent->d_name ) >= strName.size() - 1 &&
                                                strcmp( ent->d_name + strlen( ent->d_name ) - ( strName.size() - 1 ),
                                                        strName.c_str() + 1 ) == 0 ) ) {
            strFound = strPath;
        }
    }
    closedir( d );
    return strFound;
}

static int TestSiteLayout() {
    CHECK( SiteLayout::SipLogDir( "/site/log/" ) == "/site/log/sip", "SipLogDir = <log>/sip (끝 / 제거)" );
    CHECK( SiteLayout::SipLogDir( "" ).empty(), "SipLogDir — 로그 미설정이면 빈 값" );
    CHECK( SiteLayout::RecordingsDir( "/site/recordings/", "/site/log" ) == "/site/recordings",
           "RecordingsDir 명시값" );
    CHECK( SiteLayout::RecordingsDir( "", "/logroot" ) == "/logroot", "RecordingsDir 단일 루트 = <log>" );
    CHECK( SiteLayout::ContentDir( "", "/logroot/" ) == "/logroot", "ContentDir 단일 루트 = <log>" );
    CHECK( SiteLayout::StateDir( "", "/logroot" ) == "/logroot/state", "StateDir 단일 루트 = <log>/state" );
    CHECK( SiteLayout::StatsDir( "", "/logroot" ) == "/logroot/stats", "StatsDir 단일 루트 = <log>/stats" );
    CHECK( SiteLayout::StatsDir( "/site/stats", "/logroot" ) == "/site/stats", "StatsDir 명시값" );
    CHECK( SiteLayout::StateDir( "", "" ).empty() && SiteLayout::StatsDir( "", "" ).empty(),
           "로그·영역 모두 비면 빈 값" );
    return 0;
}

static int TestAreas( const std::string &strRoot ) {
    const std::string strRec = strRoot + "/recordings";
    const std::string strState = strRoot + "/state";
    const std::string strStats = strRoot + "/stats";
    std::string strVoipDir, strPttBase;
    {
        CCallDir clsDir;
        clsDir.Init( strRec, strState, strStats, "csp", 5 );
        CHECK( clsDir.IsEnabled(), "녹취 영역이 있으면 기록 활성" );

        clsDir.MapCallToSession( "call-1", "S20260923101010000001" );
        strVoipDir = clsDir.GetVoipDir( "call-1", "+821012345678", "+821099990000" );
        clsDir.VoipCallStart( "call-1", "+821012345678", "+821099990000" );
        clsDir.VoipCallEnd( "call-1", "normal", 3, 200 );

        strPttBase = clsDir.GetPttSessionDir( "g1", "+821011112222::csp::20260923101010123456::1", "7" );
        clsDir.PttSessionStart( "g1", "call-p", "+821011112222", "{\"group_id\":\"g1\"}" );
        clsDir.PttAttempt( "g1", "7", "+821011112222", "established", "", "", 0,
                           "+821011112222::csp::20260923101010123456::1" );
        clsDir.McDataMessageLog( "g1", "{\"text\":\"hi\"}" );
        clsDir.McData1to1Log( "{\"text\":\"hi\"}" );
    }  // 소멸 = worker Stop → 큐 드레인

    CHECK( StartsWith( strVoipDir, strRec + "/volte/" ), "VoLTE 세션 디렉터리 = <recordings>/volte/…" );
    std::string strCall = FindFile( strRec + "/volte", "call.json" );
    CHECK( !strCall.empty() && StartsWith( strCall, strVoipDir ), "call.json 이 세션 디렉터리에 기록" );
    CHECK( ReadAll( strCall ).find( "\"state\":\"ended\"" ) != std::string::npos, "call.json 종료 마킹" );
    CHECK( !FindFile( strRec + "/volte", "index.json" ).empty(), "시간 버킷 index.json 기록" );
    CHECK( Exists( strState + "/volte" ) && Exists( strState + "/ptt" ), "상태 영역에 volte/·ptt/ 준비" );

    CHECK( strPttBase == strRec + "/ptt/7", "PTT 그룹 base = <recordings>/ptt/<저장 키>" );
    CHECK( Exists( strPttBase + "/group.json" ), "group.json 기록" );
    std::string strSess = FindFile( strPttBase, "session.json" );
    CHECK( strSess.find( "/S20260923101010123456_1/session.json" ) != std::string::npos,
           "PTT 세션 디렉터리 S{ts}_{n}/session.json" );
    CHECK( Exists( strState + "/ptt/+821011112222.json" ), "PTT 개시자 상태 파일 = <state>/ptt/<가입자>.json" );

    CHECK( !FindFile( strStats + "/ptt_attempts", "*.jsonl" ).empty(),
           "PTT 시도 장부 = <stats>/ptt_attempts/<일>.jsonl" );
    CHECK( !FindFile( strRec + "/message/g1", "messages.jsonl" ).empty(),
           "그룹 메시지 = <recordings>/message/<gid>/…" );
    CHECK( !FindFile( strRec + "/message_direct", "messages.jsonl" ).empty(),
           "1:1 메시지 = <recordings>/message_direct/…" );

    CHECK( !Exists( strRec + "/state" ) && !Exists( strRec + "/ptt/attempts" ),
           "녹취 영역에 상태·장부가 섞이지 않는다" );
    return 0;
}

static int TestOptionalAxes( const std::string &strRoot ) {
    const std::string strRec = strRoot + "/rec_only";
    {
        CCallDir clsDir;
        clsDir.Init( strRec, "", "", "csp", 5 );
        clsDir.MapCallToSession( "call-2", "S20260923101010000002" );
        clsDir.GetVoipDir( "call-2", "+821012345678", "+821099990000" );
        clsDir.VoipCallStart( "call-2", "+821012345678", "+821099990000" );
        clsDir.PttAttempt( "g2", "8", "+821011112222", "failed", "denied", "not_member", 403 );
    }
    CHECK( !FindFile( strRec + "/volte", "call.json" ).empty(), "상태·통계 영역 없이도 통화 기록" );
    CHECK( !Exists( strRec + "/ptt_attempts" ) && !Exists( strRec + "/state" ),
           "상태·통계 영역이 비면 그 축은 건너뛴다" );

    CCallDir clsOff;
    clsOff.Init( "", strRoot + "/s", strRoot + "/t", "csp", 5 );
    CHECK( !clsOff.IsEnabled(), "녹취 영역이 비면 기록 비활성" );
    return 0;
}

int main() {
    char szTmpl[] = "/tmp/csp_call_dir_test_XXXXXX";
    const char *pszRoot = mkdtemp( szTmpl );
    if ( !pszRoot ) {
        printf( "FAIL mkdtemp\n" );
        return 1;
    }
    std::string strRoot = pszRoot;
    int rc = TestSiteLayout();
    if ( rc == 0 ) rc = TestAreas( strRoot );
    if ( rc == 0 ) rc = TestOptionalAxes( strRoot );
    std::string strCmd = "rm -rf '" + strRoot + "'";
    if ( system( strCmd.c_str() ) != 0 ) printf( "WARN cleanup failed: %s\n", strRoot.c_str() );
    if ( rc != 0 ) return rc;
    printf( "ALL PASS (%d)\n", g_iPass );
    return 0;
}
