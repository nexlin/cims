// 관제조작반 태블릿 앱 (docs/design/features/android_dispatch_tablet.md)
//
// CIMS 연동은 전부 :cimsue(SDK 파사드) 위에 있다. 이 모듈은 화면·장치·수명주기만 갖는다 —
// `com.cims.ue.sdk.jni.*` 와 `org.pjsip.*` 를 직접 쓰지 않는다(AGENTS.md §7).
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.cims.ue.dispatch"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "com.cims.ue.dispatch"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures { compose = true }

    packaging { jniLibs.useLegacyPackaging = false }
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}

dependencies {
    implementation(project(":cimsue"))                // 단말 SDK — 유일한 CIMS 연동 통로
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)

    debugImplementation(libs.androidx.ui.tooling)
    testImplementation(libs.junit)
    // 기기에서는 플랫폼이 org.json 을 제공한다(android.jar). JVM 단위시험에서만 실제 구현이 필요하다 —
    // 없으면 "not mocked" 로 떨어져 와이어 파서를 시험할 수 없다.
    testImplementation(libs.org.json)
}
