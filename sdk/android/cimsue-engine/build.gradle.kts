// :cimsue-engine — 엔진(pjproject) 바인딩 AAR (docs/design/features/android_dispatch_tablet.md §2.2)
//
// org.pjsip.** (SWIG 생성 Java) + libpjsua2.so 를 **독점 제공**한다. 이 모듈이 서기 전에는 같은 산출물이
// android/core/src/pjsua2/ 에 커밋돼 있었고, ext/pjproject 에 패치가 들어가도 그 사본은 따라가지 않았다.
// 이제 산출물은 sdk/android/build-native.sh 만 만들고 커밋하지 않는다.
//
// 소비자 = :core (기존 VoLTE/PTT 앱). 관제 태블릿은 :cimsue(코어 파사드)만 쓴다 — 한 앱이 두 모듈을
// 같이 쓰지 않으므로 libc++_shared.so 가 겹치지 않는다.
plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.cims.ue.sdk.engine"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()
        ndk { abiFilters += "arm64-v8a" }          // PJSIP 단일 ABI
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs.useLegacyPackaging = false          // 미압축 .so (16KB page 정렬 유지)
    }
}
