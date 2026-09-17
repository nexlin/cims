// libcimsue SWIG 인터페이스 — Android(Java) 바인딩 정본 (ue_sdk.md §5.1, android_dispatch_tablet.md §3)
//
// pjsua2 가 이미 SWIG 을 쓰므로 코어도 같은 도구 한 벌로 생성한다. 공개 헤더(cimsue/*.h)만 노출하며
// pjsua2 타입은 나오지 않는다. 이벤트는 Listener director 로 Java 쪽 서브클래스에 전달된다.
//
// 생성물에 SWIGTYPE_p_* (불투명 타입) 이 남으면 안 된다 — 앱이 쓸 수 없는 껍데기이기 때문이다.
// 검사: S1-UE-ANDROID-BIND (android_dispatch_tablet.md §9).
//
//   swig -c++ -java -package com.cims.ue.sdk.jni -outdir <java-out> -o cimsue_wrap.cpp \
//        -I../include cimsue.i
%module(directors="1") cimsue

%{
#include "cimsue/cimsue.h"
using namespace cimsue;
%}

%include "std_string.i"
%include "std_vector.i"
%include "std_map.i"
%include "stdint.i"

%feature("director") cimsue::Listener;

// ── 불투명 타입이 될 인자를 먼저 걷어낸다 ──────────────────────────────────────
// 기본인자 포인터는 SWIG 이 오버로드를 하나 더 만들고 그 인자가 SWIGTYPE_p_* 로 떨어진다.
// 어느 것도 Java 에서 쓸 수 없고 값을 얻는 경로도 아니므로(오류 사유는 Result.reason 으로 온다)
// 포인터를 받는 오버로드만 지운다 — 기본인자 쪽(err 생략)은 그대로 생성된다.
%ignore cimsue::GroupDoc::parse(const std::string&, GroupDoc&, std::string*);
%ignore cimsue::CscClient::parseProfile(const std::string&, Profile&, std::string*);
// 전송 주입(http::ITransport)은 **아직 어느 플랫폼 SDK 에서도 열려 있지 않다** — 인터페이스가 내부 헤더
// (src/http/https_client.h, "libcimsue 내부")에 있고 C API 에도 진입점이 없다(cimsue_csc_create 는 endpoint
// 만 받는다). 내부 타입이 새어 나온 이 오버로드는 바인딩에 불투명 핸들만 남기므로 지운다. 주입을 열려면
// 인터페이스를 공개 헤더로 올리고 세 바인딩(SWIG director·C API·.NET)에 같이 내야 한다 — ue_sdk.md §11.
%ignore cimsue::CscClient::CscClient(const CscEndpoint&, std::shared_ptr<http::ITransport>);

// pImpl — 바인딩에 내부 타입을 내지 않는다
%ignore cimsue::Engine::Impl;
%ignore cimsue::CscClient::Impl;

// export.h 의 DLL 가시성 매크로 — SWIG 는 헤더의 #if 를 평가하지 않으므로 빈 매크로로 선언
#define CIMSUE_API

// 중첩 구조체(ServiceProfile::Endpoint)를 최상위로 펼친다 — Java 에 중첩 프록시를 만들지 않는다.
%feature("flatnested") cimsue::ServiceProfile::Endpoint;

// ── 이진 본문 ─────────────────────────────────────────────────────────────────
// HttpResult.body 는 "바이트 그대로"다(csc.h:87) — 녹취 오디오(MP4/AAC)가 이 경로로 온다
// (csc/src/handlers/dispatch_recordings.py 가 bytes 를 낸다). std_string.i 기본 typemap 은 NewStringUTF 로
// 내보내 첫 NUL 에서 잘리고 비 UTF-8 바이트를 망가뜨린다. Windows 파사드가 byte[] 로 받는 것과 같게 맞춘다
// (sdk/windows/dotnet/CimsUe/CscClient.cs 의 HttpResponse.Body). 요청 본문도 같은 이유로 byte[] 로 받는다 —
// JSON 은 파사드가 UTF-8 로 인코딩해 넘긴다.
%typemap(jni)     std::string BINARY "jbyteArray"
%typemap(jtype)   std::string BINARY "byte[]"
%typemap(jstype)  std::string BINARY "byte[]"
%typemap(javain)  std::string BINARY "$javainput"
%typemap(javaout) std::string BINARY { return $jnicall; }
%typemap(out) std::string BINARY {
    $result = jenv->NewByteArray((jsize)$1.size());
    if ($result) jenv->SetByteArrayRegion($result, 0, (jsize)$1.size(), (const jbyte*)$1.data());
}
%typemap(in) std::string BINARY {
    if ($input) {
        jsize _n = jenv->GetArrayLength($input);
        jbyte* _b = jenv->GetByteArrayElements($input, 0);
        $1.assign((const char*)_b, (size_t)_n);
        jenv->ReleaseByteArrayElements($input, _b, JNI_ABORT);
    }
}
%typemap(jni)     const std::string& BINARY "jbyteArray"
%typemap(jtype)   const std::string& BINARY "byte[]"
%typemap(jstype)  const std::string& BINARY "byte[]"
%typemap(javain)  const std::string& BINARY "$javainput"
%typemap(javaout) const std::string& BINARY { return $jnicall; }
%typemap(out) const std::string& BINARY {
    $result = jenv->NewByteArray((jsize)$1->size());
    if ($result) jenv->SetByteArrayRegion($result, 0, (jsize)$1->size(), (const jbyte*)$1->data());
}
%typemap(in) const std::string& BINARY (std::string _tmp) {
    if ($input) {
        jsize _n = jenv->GetArrayLength($input);
        jbyte* _b = jenv->GetByteArrayElements($input, 0);
        _tmp.assign((const char*)_b, (size_t)_n);
        jenv->ReleaseByteArrayElements($input, _b, JNI_ABORT);
    }
    $1 = &_tmp;
}
// 적용 대상은 **이진인 둘뿐**이다 — Windows 파사드와 같은 타입으로 맞춘다
// (HttpResponse.Body = byte[] / XcapDoc.Body = string / Account.SendRequest(body) = string).
// XcapDoc.body(XCAP XML)와 Engine::sendRequest 의 SIP 본문은 텍스트라 String 으로 둔다.
// 적용 자체는 %include 사이에 둔다(§ 아래) — SWIG 는 함수별 파라미터 한정(CscClient::request::body)을
// 지원하지 않으므로, %apply 가 "그 뒤에 파싱되는 선언에만 걸린다"는 위치 의존성을 쓴다.
%immutable cimsue::HttpResult::body;      // 코어가 채우는 산출 전용 — setter 를 내지 않는다

%include "cimsue/types.h"
%include "cimsue/listener.h"
// engine.h 를 먼저 — Engine::sendRequest 의 SIP 본문은 텍스트라 아래 이진 적용 전에 통과시킨다.
%include "cimsue/engine.h"

// 여기부터 csc.h 끝까지 `const std::string& body` 는 이진(byte[])이다 — HttpResult.body 와
// CscClient::request 의 요청 본문이 대상. XcapDoc.body 만 XML 텍스트라 멤버 한정으로 되돌린다
// (멤버는 %naturalvar 때문에 const 참조 typemap 을 타고, 이름 한정 패턴이 무한정 패턴을 이긴다).
%apply const std::string& BINARY { const std::string& body };
%apply const std::string&        { const std::string& cimsue::XcapDoc::body };
%include "cimsue/csc.h"
%clear const std::string& body;

// ── 값 컨테이너 ────────────────────────────────────────────────────────────────
// %template 은 대상 타입이 선언된 뒤에 와야 한다(위 %include 다음).
%template(IntVector)            std::vector<int>;
%template(StringVector)         std::vector<std::string>;
%template(StringMap)            std::map<std::string, std::string>;
%template(MediaSourceVector)    std::vector<cimsue::MediaSource>;
%template(AudioDeviceVector)    std::vector<cimsue::AudioDeviceInfo>;
%template(TalkerVector)         std::vector<cimsue::Talker>;
%template(RosterVector)         std::vector<cimsue::RosterEntry>;
%template(ServiceProfileVector) std::vector<cimsue::ServiceProfile>;
%template(EndpointVector)       std::vector<cimsue::ServiceProfile::Endpoint>;
%template(DispatchMemberVector) std::vector<cimsue::DispatchMember>;
%template(DispatchTargetVector) std::vector<cimsue::DispatchTarget>;
%template(GroupSummaryVector)   std::vector<cimsue::GroupSummary>;
%template(GroupMemberVector)    std::vector<cimsue::GroupMember>;
