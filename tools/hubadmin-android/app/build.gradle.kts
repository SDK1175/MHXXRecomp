plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "kr.mhxx.hubadmin"
    compileSdk = 35

    defaultConfig {
        applicationId = "kr.mhxx.hubadmin"
        minSdk = 30 // (the key is kept behind the screen lock: BiometricPrompt with device credential)
        targetSdk = 35
        versionCode = (System.getenv("APP_VERSION_CODE") ?: "1").toInt()
        versionName = System.getenv("APP_VERSION_NAME") ?: "0.1"
    }

    // The same key for every build (a CI secret), so an update installs over the old app and keeps its SSH key.
    val ks = System.getenv("SIGN_KEYSTORE")
    signingConfigs {
        if (ks != null) create("release") {
            storeFile = file(ks)
            storePassword = System.getenv("SIGN_PASSWORD")
            keyAlias = System.getenv("SIGN_ALIAS") ?: "hubadmin"
            keyPassword = System.getenv("SIGN_PASSWORD")
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            // (no secret yet: the debug key, so the APK still installs; the next signed build needs a reinstall)
            signingConfig = signingConfigs.getByName(if (ks != null) "release" else "debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging { resources.excludes += setOf("META-INF/versions/**") }
}

dependencies {
    implementation("com.github.mwiede:jsch:0.2.21")
}
