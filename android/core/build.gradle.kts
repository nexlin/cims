// :core — 단말 앱 공용 조각(계정·SSO·프로비저닝·설정·연락처·통화 기록·메시지·기기 신원·TLS 신뢰·전원·부팅·오버레이·앱 간 신호)
//
// **SIP·엔진이 없다.** 기존 앱의 자체 pjsua2 래퍼는 :core-sip(이행용)에 있고, SDK 로 옮긴 앱은 :cimsue 를 쓴다
// (docs/design/features/ue_sdk.md §5.3). 그래서 로그인 앱(:cims)처럼 이 모듈만 쓰는 앱은 libpjsua2.so 를 싣지 않는다.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.cims.ue.core"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)   // 저장소 StateFlow
    implementation(libs.okhttp)                        // 로그인·프로비저닝(CSC HTTPS)

    testImplementation(libs.junit)                     // Pkce 등 JVM 단위테스트
}
