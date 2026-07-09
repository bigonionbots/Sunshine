plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.lizardbyte.sunshine"
    compileSdk = 34

    defaultConfig {
        applicationId = "dev.lizardbyte.sunshine"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"

        // The prebuilt libsunshine.so is built for arm64-v8a (add more ABIs as they are built).
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
        debug {
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

    // libsunshine.so is prebuilt (via the CMake/NDK flow, not AGP's externalNativeBuild); package
    // it and libc++_shared.so from src/main/jniLibs as-is (do not strip the debug build).
    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += "**/libsunshine.so"
        }
    }
}
