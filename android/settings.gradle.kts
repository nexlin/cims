@file:Suppress("UnstableApiUsage")

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0"
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "cims-android"

// 단말 SDK AAR — Gradle 루트는 android/ 라 sdk/android 는 루트 밖이다. projectDir 로 위치만 옮긴다
// (docs/design/features/android_dispatch_tablet.md §2). 소스 동기 빌드가 유지되고 publish 가 필요 없다.
include(":cimsue-engine")
project(":cimsue-engine").projectDir = file("../sdk/android/cimsue-engine")
include(":cimsue")
project(":cimsue").projectDir = file("../sdk/android/cimsue")

include(":core")
include(":cims")
include(":volte-client")
include(":ptt-client")

include(":dispatch-tablet")

// 오디오 라우팅 탐침 — F5 판정용 일회성 앱(docs/design/features/android_dispatch_tablet.md §8)
include(":audio-probe")
