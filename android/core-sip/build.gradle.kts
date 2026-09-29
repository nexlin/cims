// :core-sip — 기존 앱의 자체 pjsua2 래퍼(SipController·CimsCall·CimsAccount·CimsEndpoint·PjLib·CodecConfig) — **이행용**
//
// 단말 SDK 이행(docs/design/features/ue_sdk.md §5.3)에서 아직 SDK(:cimsue)로 옮기지 않은 앱만 쓴다. 한 앱이 이 모듈과
// :cimsue 를 같이 쓰지 않는다 — pj 기호를 둘 다 내 한 프로세스에 엔진이 둘이 된다. 모든 앱이 옮겨지면 지운다(P5).
// SIP 가 없는 공용 조각(계정·프로비저닝·저장소·전원 …)은 :core 에 있다 — 로그인 앱(:cims)은 그것만 써 엔진을 싣지 않는다.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.cims.ue.core.sip"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()
        ndk { abiFilters += "arm64-v8a" }          // PJSIP 단일 ABI (설계서 §2.7)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs.useLegacyPackaging = false          // 미압축 .so (16KB page 정렬 유지)
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    api(project(":core"))                              // 모델·설정·신뢰 앵커(SipModels·SipAccountConfig·CimsTrustStore …)
    // 엔진(org.pjsip.**, libpjsua2.so) — api 로 내보낸다: SipController 등이 pjsua2 타입을 공개 시그니처에 쓰고 있어
    // 소비자도 봐야 한다(docs/design/features/android_dispatch_tablet.md §2.2 엔진 단일화).
    api(project(":cimsue-engine"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)   // StateFlow 노출
}
