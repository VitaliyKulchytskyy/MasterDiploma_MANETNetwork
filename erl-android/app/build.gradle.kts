import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import java.util.Locale

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Versioned like the ERL firmware (emergencyRadioLink/cmake/GenerateBuildInfo.cmake): major.minor
// set here, a build number bumped once per build and kept in .build_number, and the build's date
// and time in the C compiler's __DATE__ / __TIME__ format ("Sep 27 2026", "14:23:01").
val appMajor = 1
val appMinor = 0

val buildNumberFile: File = rootProject.file(".build_number")
// Only an actual build bumps the counter, not an IDE sync or `gradlew tasks`
val building = gradle.startParameter.taskNames.any { task ->
    listOf("assemble", "install", "bundle", "build").any { task.substringAfterLast(':').startsWith(it) }
}
val buildNumber: Int = run {
    val last = buildNumberFile.takeIf { it.exists() }?.readText()?.trim()?.toIntOrNull() ?: 0
    if (building) (last + 1).also { buildNumberFile.writeText("$it\n") } else maxOf(last, 1)
}
val builtAt: LocalDateTime = LocalDateTime.now()
val buildDate: String = builtAt.format(DateTimeFormatter.ofPattern("MMM ppd yyyy", Locale.US))
val buildTime: String = builtAt.format(DateTimeFormatter.ofPattern("HH:mm:ss"))
if (building) {
    println("\u001b[1m\u001b[32m-- [BUILD INFO]: v$appMajor.$appMinor, build #$buildNumber, $buildDate $buildTime\u001b[m")
}

android {
    namespace = "com.erl.node"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.erl.node"
        minSdk = 26
        targetSdk = 35
        versionCode = buildNumber
        versionName = "$appMajor.$appMinor"

        buildConfigField("int", "VERSION_MAJOR", "$appMajor")
        buildConfigField("int", "VERSION_MINOR", "$appMinor")
        buildConfigField("int", "BUILD_NUMBER", "$buildNumber")
        buildConfigField("String", "BUILD_DATE", "\"$buildDate\"")
        buildConfigField("String", "BUILD_TIME", "\"$buildTime\"")
        buildConfigField(
            "String", "BUILT", "\"${builtAt.format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"))}\""
        )
    }

    buildFeatures {
        buildConfig = true
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}
