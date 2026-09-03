// libcimsue — 심볼 가시성 (ue_sdk.md §6)
//
// Windows SDK 는 코어를 cimsue.dll 로 배포한다(C++ 클래스 export — 앱과 같은 MSVC 툴체인·CRT 전제, C# 은 별도 C API).
// Linux/Android 는 정적 링크라 빈 매크로. 공개 표면(Engine·Listener·toString)에만 붙인다 — types.h 의 구조체는
// 인라인/헤더 전용이라 표시하지 않는다.
#pragma once

#if defined(_WIN32) && defined(CIMSUE_SHARED)
#  if defined(CIMSUE_BUILDING)
#    define CIMSUE_API __declspec(dllexport)
#  else
#    define CIMSUE_API __declspec(dllimport)
#  endif
#else
#  define CIMSUE_API
#endif
