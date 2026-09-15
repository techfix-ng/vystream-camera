package com.port80.app

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import com.port80.app.data.SettingsRepository
import com.port80.app.navigation.AppNavGraph
import com.port80.app.util.OrientationHelper
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import javax.inject.Inject

/**
 * Single Activity for the entire app.
 * Uses Jetpack Compose for all UI rendering.
 * @AndroidEntryPoint enables Hilt dependency injection.
 */
@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    @Inject lateinit var settingsRepository: SettingsRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // VyStream is a camera operator surface. Keep the display awake while
        // the Activity is open so framing and controls are never interrupted by
        // Android dimming or automatic sleep. A user pressing the power button
        // can still lock the phone normally.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Apply orientation lock BEFORE setContent to prevent
        // layout flicker on recreation. Uses runBlocking because
        // DataStore reads are fast local I/O and this must complete
        // before the UI is inflated.
        applyOrientationLock()

        setContent {
            MaterialTheme {
                AppNavGraph()
            }
        }
    }

    override fun onDestroy() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        super.onDestroy()
    }

    /**
     * Reads orientation preferences synchronously and applies them.
     *
     * When a stream is active, orientation is always locked (the streaming
     * service will set the preferred orientation before the Activity recreates).
     * When idle, respects the user's lock/unlock preference.
     */
    private fun applyOrientationLock() {
        // VyStream Camera is landscape-only. Do not restore the old portrait
        // preference or unlock the Activity when the app starts.
        OrientationHelper.lock(this, ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
    }
}
