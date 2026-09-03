// libcimsue SWIG 인터페이스 — Android(Java) 바인딩 정본 (ue_sdk.md §5.1)
//
// pjsua2 가 이미 SWIG 을 쓰므로 코어도 같은 도구 한 벌로 생성한다. 공개 헤더(cimsue/*.h)만 노출하며
// pjsua2 타입은 나오지 않는다. 이벤트는 Listener director 로 Java 쪽 서브클래스에 전달된다.
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
%include "stdint.i"

%feature("director") cimsue::Listener;

%template(IntVector)          std::vector<int>;
%template(StringVector)       std::vector<std::string>;
%template(MediaSourceVector)  std::vector<cimsue::MediaSource>;
%template(AudioDeviceVector)  std::vector<cimsue::AudioDeviceInfo>;

// pImpl — 바인딩에 내부 타입을 내지 않는다
%ignore cimsue::Engine::Impl;

// export.h 의 DLL 가시성 매크로 — SWIG 는 헤더의 #if 를 평가하지 않으므로 빈 매크로로 선언
#define CIMSUE_API
%include "cimsue/types.h"
%include "cimsue/listener.h"
%include "cimsue/engine.h"
