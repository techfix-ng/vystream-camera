package com.port80.app.service

import android.media.MediaCodec
import android.os.Handler
import android.os.Looper
import com.pedro.common.AudioCodec
import com.pedro.common.ConnectChecker
import com.pedro.common.VideoCodec
import com.pedro.library.base.Camera2Base
import com.pedro.library.util.streamclient.StreamBaseClient
import com.pedro.library.view.OpenGlView
import com.pedro.rtmp.rtmp.RtmpClient
import com.pedro.srt.srt.SrtClient
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap

/**
 * One Camera2/MediaCodec pipeline feeding every configured RTMP(S) and SRT client.
 * A failed client reconnects independently and never stops the shared encoder.
 */
class MultiCamera2(openGlView: OpenGlView) : Camera2Base(openGlView) {
    private sealed class ClientHolder(
        val target: MultiOutputTarget,
        val checker: DestinationChecker,
    ) {
        abstract val streaming: Boolean
        abstract fun connect()
        abstract fun disconnect()
        abstract fun sendVideo(buffer: ByteBuffer, info: MediaCodec.BufferInfo)
        abstract fun sendAudio(buffer: ByteBuffer, info: MediaCodec.BufferInfo)
        abstract fun setVideoInfo(sps: ByteBuffer, pps: ByteBuffer?, vps: ByteBuffer?)
        abstract fun setAudioInfo(sampleRate: Int, stereo: Boolean)
        abstract fun setVideoCodec(codec: VideoCodec)
        abstract fun setAudioCodec(codec: AudioCodec)
        open fun setVideoGeometry(width: Int, height: Int, fps: Int) = Unit
        abstract fun reconnect(delayMs: Long)
        abstract fun bitrateStats(): Pair<Long, Long>
    }

    private class RtmpHolder(
        target: MultiOutputTarget,
        checker: DestinationChecker,
        private val client: RtmpClient,
        private val url: String,
    ) : ClientHolder(target, checker) {
        override val streaming: Boolean get() = client.isStreaming
        override fun connect() = client.connect(url)
        override fun disconnect() = client.disconnect()
        override fun sendVideo(buffer: ByteBuffer, info: MediaCodec.BufferInfo) = client.sendVideo(buffer, info)
        override fun sendAudio(buffer: ByteBuffer, info: MediaCodec.BufferInfo) = client.sendAudio(buffer, info)
        override fun setVideoInfo(sps: ByteBuffer, pps: ByteBuffer?, vps: ByteBuffer?) = client.setVideoInfo(sps, pps, vps)
        override fun setAudioInfo(sampleRate: Int, stereo: Boolean) = client.setAudioInfo(sampleRate, stereo)
        override fun setVideoCodec(codec: VideoCodec) = client.setVideoCodec(codec)
        override fun setAudioCodec(codec: AudioCodec) = client.setAudioCodec(codec)
        override fun setVideoGeometry(width: Int, height: Int, fps: Int) {
            client.setVideoResolution(width, height)
            client.setFps(fps)
        }
        override fun reconnect(delayMs: Long) = client.reConnect(delayMs)
        override fun bitrateStats(): Pair<Long, Long> = client.sentVideoFrames to client.droppedVideoFrames
    }

    private class SrtHolder(
        target: MultiOutputTarget,
        checker: DestinationChecker,
        private val client: SrtClient,
        private val url: String,
    ) : ClientHolder(target, checker) {
        override val streaming: Boolean get() = client.isStreaming
        override fun connect() = client.connect(url)
        override fun disconnect() = client.disconnect()
        override fun sendVideo(buffer: ByteBuffer, info: MediaCodec.BufferInfo) = client.sendVideo(buffer, info)
        override fun sendAudio(buffer: ByteBuffer, info: MediaCodec.BufferInfo) = client.sendAudio(buffer, info)
        override fun setVideoInfo(sps: ByteBuffer, pps: ByteBuffer?, vps: ByteBuffer?) = client.setVideoInfo(sps, pps, vps)
        override fun setAudioInfo(sampleRate: Int, stereo: Boolean) = client.setAudioInfo(sampleRate, stereo)
        override fun setVideoCodec(codec: VideoCodec) = client.setVideoCodec(codec)
        override fun setAudioCodec(codec: AudioCodec) = client.setAudioCodec(codec)
        override fun reconnect(delayMs: Long) = client.reConnect(delayMs)
        override fun bitrateStats(): Pair<Long, Long> = client.sentVideoFrames to client.droppedVideoFrames
    }

    private inner class DestinationChecker(
        private val id: String,
        private val name: String,
    ) : ConnectChecker {
        var holder: ClientHolder? = null
        private var retryAttempt = 0
        private var authFailed = false

        override fun onConnectionStarted(url: String) = update(id, name, OutputConnectionState.CONNECTING)
        override fun onConnectionSuccess() {
            retryAttempt = 0
            authFailed = false
            update(id, name, OutputConnectionState.LIVE)
        }
        override fun onConnectionFailed(reason: String) {
            if (authFailed || reason.contains("auth", true) || reason.contains("credential", true)) {
                update(id, name, OutputConnectionState.AUTHENTICATION_FAILED, reason)
                return
            }
            retryAttempt++
            val delayMs = (1_000L shl retryAttempt.coerceAtMost(5)).coerceAtMost(30_000L)
            update(id, name, OutputConnectionState.RETRYING, "Retrying in ${delayMs / 1000}s")
            handler.postDelayed({
                if (running) holder?.reconnect(0)
            }, delayMs)
        }
        override fun onDisconnect() {
            if (running && !authFailed) onConnectionFailed("Destination unavailable")
            else update(id, name, OutputConnectionState.STOPPED)
        }
        override fun onAuthError() {
            authFailed = true
            update(id, name, OutputConnectionState.AUTHENTICATION_FAILED, "Check credentials or stream key")
        }
        override fun onAuthSuccess() { authFailed = false }
        override fun onNewBitrate(bitrate: Long) = update(id, name, OutputConnectionState.LIVE, bitrate = bitrate)
    }

    private val handler = Handler(Looper.getMainLooper())
    private val clients = mutableListOf<ClientHolder>()
    private val statuses = ConcurrentHashMap<String, OutputStatus>()
    private val aggregateClient = AggregateStreamClient { requestKeyFrame() }
    private var statusListener: (List<OutputStatus>) -> Unit = {}
    private var running = false
    private var selectedVideoCodec = VideoCodec.H264
    private var selectedAudioCodec = AudioCodec.AAC
    private var sampleRate = 44_100
    private var stereo = true

    fun configure(targets: List<MultiOutputTarget>, listener: (List<OutputStatus>) -> Unit) {
        require(!isStreaming) { "Outputs must be configured before starting the encoder" }
        clients.forEach { it.disconnect() }
        clients.clear()
        statuses.clear()
        statusListener = listener
        targets.forEach { target ->
            val checker = DestinationChecker(target.id, target.name)
            val holder = when (val params = target.params) {
                is ConnectionParams.Rtmp -> {
                    val client = RtmpClient(checker)
                    client.setAuthorization(params.username, params.password)
                    val fullUrl = if (params.streamKey.isNotBlank()) "${params.baseUrl.trimEnd('/')}/${params.streamKey}" else params.baseUrl
                    RtmpHolder(target, checker, client, fullUrl)
                }
                is ConnectionParams.Srt -> {
                    val client = SrtClient(checker)
                    if (!params.passphrase.isNullOrBlank()) client.setPassphrase(params.passphrase, params.srtKeyLength.toRootEncoder())
                    SrtHolder(target, checker, client, buildSrtUrl(params))
                }
            }
            checker.holder = holder
            holder.setVideoCodec(selectedVideoCodec)
            holder.setAudioCodec(selectedAudioCodec)
            holder.setAudioInfo(sampleRate, stereo)
            clients += holder
            update(target.id, target.name, OutputConnectionState.CONNECTING)
        }
    }

    fun outputStatuses(): List<OutputStatus> = statuses.values.sortedBy { it.name }

    override fun getStreamClient(): StreamBaseClient = aggregateClient
    override fun setVideoCodecImp(codec: VideoCodec) { selectedVideoCodec = codec; clients.forEach { it.setVideoCodec(codec) } }
    override fun setAudioCodecImp(codec: AudioCodec) { selectedAudioCodec = codec; clients.forEach { it.setAudioCodec(codec) } }
    override fun onAudioInfoImp(isStereo: Boolean, sampleRate: Int) {
        this.stereo = isStereo; this.sampleRate = sampleRate
        clients.forEach { it.setAudioInfo(sampleRate, isStereo) }
    }
    override fun startStreamImp(url: String) {
        running = true
        clients.forEach { holder ->
            val width = if (videoEncoder.rotation == 90 || videoEncoder.rotation == 270) videoEncoder.height else videoEncoder.width
            val height = if (videoEncoder.rotation == 90 || videoEncoder.rotation == 270) videoEncoder.width else videoEncoder.height
            holder.setVideoGeometry(width, height, videoEncoder.fps)
            holder.connect()
        }
    }
    override fun stopStreamImp() { running = false; handler.removeCallbacksAndMessages(null); clients.forEach { it.disconnect() } }
    override fun getAudioDataImp(audioBuffer: ByteBuffer, info: MediaCodec.BufferInfo) { clients.filter { it.streaming }.forEach { it.sendAudio(audioBuffer, info) } }
    override fun onVideoInfoImp(sps: ByteBuffer, pps: ByteBuffer?, vps: ByteBuffer?) { clients.forEach { it.setVideoInfo(sps, pps, vps) } }
    override fun getVideoDataImp(videoBuffer: ByteBuffer, info: MediaCodec.BufferInfo) { clients.filter { it.streaming }.forEach { it.sendVideo(videoBuffer, info) } }

    private fun update(id: String, name: String, state: OutputConnectionState, detail: String? = null, bitrate: Long = 0) {
        statuses[id] = OutputStatus(id, name, state, detail, bitrate)
        statusListener(outputStatuses())
    }

    private fun buildSrtUrl(params: ConnectionParams.Srt): String {
        val streamId = params.streamId?.takeIf { it.isNotBlank() } ?: "#!::m=publish"
        return "srt://${params.host}:${params.port}?mode=${params.mode.toUrlParam()}&latency=${params.latencyMs}&streamid=$streamId"
    }
}

/** Aggregate required by Camera2Base; destination retry is handled independently above. */
private class AggregateStreamClient(private val requestKeyframe: () -> Unit) : StreamBaseClient() {
    override fun setAuthorization(user: String?, password: String?) = Unit
    override fun reTry(delay: Long, reason: String, backupUrl: String?): Boolean { requestKeyframe(); return true }
    override fun setReTries(reTries: Int) = Unit
    override fun hasCongestion(percentUsed: Float): Boolean = false
    override fun setLogs(enabled: Boolean) = Unit
    override fun setCheckServerAlive(enabled: Boolean) = Unit
    override fun resizeCache(newSize: Int) = Unit
    override fun clearCache() = Unit
    override fun getCacheSize(): Int = 0
    override fun getItemsInCache(): Int = 0
    override fun getSentAudioFrames(): Long = 0
    override fun getSentVideoFrames(): Long = 0
    override fun getDroppedAudioFrames(): Long = 0
    override fun getDroppedVideoFrames(): Long = 0
    override fun resetSentAudioFrames() = Unit
    override fun resetSentVideoFrames() = Unit
    override fun resetDroppedAudioFrames() = Unit
    override fun resetDroppedVideoFrames() = Unit
    override fun setOnlyAudio(onlyAudio: Boolean) = Unit
    override fun setOnlyVideo(onlyVideo: Boolean) = Unit
    override fun setBitrateExponentialFactor(factor: Float) = Unit
    override fun getBitrateExponentialFactor(): Float = 1f
}
