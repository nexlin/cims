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

    // PJSIP 빌드 산출물(SWIG Java + .so)은 손코드와 섞지 않게 별도 소스셋으로 격리(설계서 §2.7).
    //   core/src/pjsua2/java     ← org/pjsip/pjsua2/*.java (+ org/pjsip/PjCamera*.java)
    //   core/src/pjsua2/jniLibs  ← arm64-v8a/{libpjsua2,libc++_shared}.so
    sourceSets {
        getByName("main") {
            java.srcDir("src/pjsua2/java")
            jniLibs.srcDir("src/pjsua2/jniLibs")
        }
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
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)   // StateFlow 노출
    implementation(libs.okhttp)                        // 로그인·프로비저닝(CSC HTTPS)

    testImplementation(libs.junit)                     // Pkce 등 JVM 단위테스트
}
