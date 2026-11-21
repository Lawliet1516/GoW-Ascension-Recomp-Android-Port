plugins { id("com.android.application") }

android {
    namespace = "com.gowa.app"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.gowa.app"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.1-bringup"
        ndk { abiFilters += listOf("arm64-v8a") }
    }
    // The native libraries are built with the NDK by android/app/build_native.sh and copied to src/main/jniLibs (not built by Gradle:
    // the runtime is one CMake project shared with the on-device tests).
    buildTypes {
        debug { isDebuggable = true }
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
