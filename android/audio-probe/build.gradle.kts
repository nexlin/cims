// 오디오 라우팅 탐침 — 관제 태블릿의 무전/통화 분리 출력이 이 기기에서 성립하는지 재는 일회성 앱.
//   정본: docs/design/features/android_dispatch_tablet.md §8·§11 (F5)
//
// pjsip 을 쓰지 않는다 — 재려는 것은 **Android 오디오 정책** 하나이기 때문이다:
//   "VOICE_CALL 스트림이 살아 있는 동안 MUSIC 스트림의 setPreferredDevice 가 존중되는가?"
// 기존 실측(MTK·퀄컴)은 앱이 둘(프로세스 둘)일 때 얻은 것이라, 한 프로세스에서의 답은 아직 없다.
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.cims.ue.probe"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "com.cims.ue.probe"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        versionCode = 1
        versionName = "0.1.0-probe"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures { compose = true }
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.material3)
}
