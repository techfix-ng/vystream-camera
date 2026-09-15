package com.port80.app.service

import android.content.ContentValues
import android.content.Context
import android.provider.MediaStore
import android.os.Environment
import com.port80.app.util.RedactingLogger
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Finalizes to a seekable temp file, then publishes a playable MP4 to Movies. */
class LocalRecordingManager(private val context: Context) {
    companion object { private const val TAG = "LocalRecording" }

    data class RecordingTarget(val file: File, val displayName: String)
    private var activeTarget: RecordingTarget? = null

    fun createRecordingTarget(): RecordingTarget? = try {
        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val displayName = "VyStream_$timestamp.mp4"
        val stagingDir = File(
            context.getExternalFilesDir(Environment.DIRECTORY_MOVIES) ?: context.filesDir,
            "recording-staging"
        ).apply { mkdirs() }
        RecordingTarget(File(stagingDir, displayName), displayName).also {
            it.file.delete()
            activeTarget = it
        }
    } catch (e: Exception) {
        RedactingLogger.e(TAG, "Could not create recording in MediaStore", e)
        null
    }

    fun finishRecording(success: Boolean): String? {
        val target = activeTarget ?: return null
        activeTarget = null
        if (!success || !target.file.isFile || target.file.length() < 1024L) {
            target.file.delete()
            RedactingLogger.e(TAG, "Recording did not finalize or is empty")
            return null
        }
        return runCatching {
            val resolver = context.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, target.displayName)
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/VyStream")
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
            val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                ?: error("MediaStore insert failed")
            try {
                resolver.openOutputStream(uri, "w")!!.use { output ->
                    target.file.inputStream().use { it.copyTo(output) }
                }
                resolver.update(uri, ContentValues().apply {
                    put(MediaStore.Video.Media.IS_PENDING, 0)
                }, null, null)
            } catch (e: Exception) {
                resolver.delete(uri, null, null)
                throw e
            }
            RedactingLogger.i(TAG, "Recording saved: ${target.displayName}")
            target.displayName
        }.onFailure { RedactingLogger.e(TAG, "Could not publish finalized MP4; staging copy retained", it) }
            .onSuccess { target.file.delete() }.getOrNull()
    }
}
