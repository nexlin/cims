// SDK 엔진 점검 앱 — 개발 도구(docs/design/features/ue_sdk.md §5.3 이행 단계의 기기 확인)
//
// 로그인·등록 없이 :cimsue 엔진만 띄워 실기기에서 코어가 도는지 본다 — 영상 장치(카메라) 열거·장치 단 음량·라우트·캡처 게이트·
// 재오픈 호출. 계정을 만들지 않으므로 같은 번호의 착신을 가로채지 않는다(사내 단말에 깔아도 된다). 결과는 화면과 logcat(SdkProbe).
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.cims.ue.sdkprobe"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "com.cims.ue.sdkprobe"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        versionCode = 1
        versionName = "0.1.0-probe"
        ndk { abiFilters += "arm64-v8a" }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging { jniLibs.useLegacyPackaging = false }
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}

dependencies {
    implementation(project(":cimsue"))                  // 단말 SDK — 엔진은 이것 하나(:core 를 같이 쓰지 않는다)
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
}
