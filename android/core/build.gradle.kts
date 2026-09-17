plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.cims.ue.core"
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
    // 엔진(org.pjsip.**, libpjsua2.so) — 종전엔 core/src/pjsua2/ 에 커밋된 산출물이었다.
    // api 로 내보낸다: SipController 등이 pjsua2 타입을 공개 시그니처에 쓰고 있어 소비자도 봐야 한다
    // (docs/design/features/android_dispatch_tablet.md §2.2 엔진 단일화).
    api(project(":cimsue-engine"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)   // StateFlow 노출
    implementation(libs.okhttp)                        // 로그인·프로비저닝(CSC HTTPS)

    testImplementation(libs.junit)                     // Pkce 등 JVM 단위테스트
}
