// Java side of the Android engine: device routes and communication mode
// (ADR D6, D15). The native library comes from the build hook.
group = "com.telosnex.audio"
version = "1.0"

buildscript {
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

plugins {
    id("com.android.library")
}

android {
    namespace = "com.telosnex.audio"

    compileSdk = 36

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        minSdk = 24
        // The engine calls AudioRoutes through JNI; R8 cannot see that.
        consumerProguardFiles("proguard-rules.pro")
    }
}
