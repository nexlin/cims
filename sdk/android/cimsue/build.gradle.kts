// :cimsue — 단말 SDK 코어 AAR (docs/design/features/ue_sdk.md §5.1, android_dispatch_tablet.md §2.1)
//
// libcimsue.so(C++ 코어 + pj 정적 링크 + JNI) + SWIG 생성 Java(com.cims.ue.sdk.jni.*) +
// Kotlin 파사드(com.cims.ue.sdk.*) + Android 접점(com.cims.ue.sdk.platform.*).
//
// **앱이 보는 공개면은 com.cims.ue.sdk.* 뿐이다** — jni.* 와 org.pjsip.* 는 내부다(AGENTS.md §7).
// 엔진 .so 는 코어가 정적으로 품으므로 :cimsue-engine 에 의존하지 않는다.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.cims.ue.sdk"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()
        ndk { abiFilters += "arm64-v8a" }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // SWIG 생성 Java 는 손코드와 섞지 않게 별도 소스셋으로 격리한다(:core 의 pjsua2 소스셋과 같은 규약).
    sourceSets {
        getByName("main") {
            java.srcDir("src/swig/java")
        }
    }

    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)   // StateFlow/SharedFlow 파사드
    testImplementation(libs.junit)
}
