// App-level build configuration for StreamCaster.
// This is where we configure Android SDK versions, product flavors,
// dependencies, and build features.
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.ksp)
    alias(libs.plugins.hilt)
    alias(libs.plugins.google.services)
}

android {
    namespace = "com.vystreamapp.com"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.vystreamapp.com"
        minSdk = 29
        targetSdk = 35
        versionCode = 19
        versionName = "2.7.7"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Embed the short git commit hash so the app can display it.
        val gitCommit = providers.exec {
            commandLine("git", "rev-parse", "--short", "HEAD")
        }.standardOutput.asText.get().trim()
        buildConfigField("String", "GIT_COMMIT_SHORT", "\"$gitCommit\"")
    }

    // Product flavors: "foss" for F-Droid (no Google services),
    // "gms" for Google Play Store (with Google services).
    flavorDimensions += "distribution"
    productFlavors {
        create("foss") {
            dimension = "distribution"
        }
        create("gms") {
            dimension = "distribution"
        }
    }

    val signingStore = providers.gradleProperty("VYSTREAM_KEYSTORE_FILE").orNull
        ?: System.getenv("VYSTREAM_KEYSTORE_FILE")
    val signingPassword = providers.gradleProperty("VYSTREAM_KEYSTORE_PASSWORD").orNull
        ?: System.getenv("VYSTREAM_KEYSTORE_PASSWORD")
    val signingAlias = providers.gradleProperty("VYSTREAM_KEY_ALIAS").orNull
        ?: System.getenv("VYSTREAM_KEY_ALIAS")
    val signingKeyPassword = providers.gradleProperty("VYSTREAM_KEY_PASSWORD").orNull
        ?: System.getenv("VYSTREAM_KEY_PASSWORD")
    val hasReleaseSigning = listOf(signingStore, signingPassword, signingAlias, signingKeyPassword)
        .all { !it.isNullOrBlank() }
    signingConfigs.create("vystreamRelease") {
        if (hasReleaseSigning) {
            storeFile = file(signingStore!!)
            storePassword = signingPassword
            keyAlias = signingAlias
            keyPassword = signingKeyPassword
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = if (hasReleaseSigning) signingConfigs.getByName("vystreamRelease")
            else signingConfigs.getByName("debug")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = true
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    // Resolve duplicate files from dependencies
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(platform("com.google.firebase:firebase-bom:34.18.0"))
    add("gmsImplementation", libs.firebase.analytics)
    // ── Compose (BOM manages consistent versions) ──
    val composeBom = platform(libs.compose.bom)
    implementation(composeBom)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons.extended)
    debugImplementation(libs.compose.ui.tooling)

    // ── Activity & Navigation ──
    implementation(libs.activity.compose)
    implementation(libs.navigation.compose)

    // ── Hilt dependency injection ──
    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)
    implementation(libs.hilt.navigation.compose)

    // ── AndroidX core ──
    implementation(libs.core.ktx)
    implementation(libs.lifecycle.runtime.ktx)
    implementation(libs.lifecycle.viewmodel.compose)
    implementation(libs.datastore.preferences)
    implementation(libs.security.crypto)

    // ── QR endpoint import scanner ──
    // CameraX is used only for the short-lived QR scanner in Settings. The
    // streaming camera path still belongs to RootEncoder/RtmpCamera2.
    implementation(libs.camera.core)
    implementation(libs.camera.camera2)
    implementation(libs.camera.lifecycle)
    implementation(libs.camera.view)
    implementation(libs.mlkit.barcode.scanning)

    // ── RootEncoder for RTMP streaming ──
    implementation(libs.rootencoder)

    // ── ACRA crash reporting ──
    implementation(libs.acra.http)
    implementation(libs.acra.dialog)

    // ── Testing ──
    testImplementation(libs.junit)
    testImplementation(libs.mockk)
    testImplementation(libs.coroutines.test)
    testImplementation(libs.arch.core.testing)
    // Android's org.json classes are stubs in local JVM tests. This lets the
    // QR parser tests run the same JSON logic without needing an emulator.
    testImplementation(libs.json)
}
