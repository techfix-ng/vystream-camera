package com.port80.app.service

/** A saved destination participating automatically in a multi-output session. */
data class MultiOutputTarget(
    val id: String,
    val name: String,
    val params: ConnectionParams,
)

enum class OutputConnectionState {
    CONNECTING,
    LIVE,
    RETRYING,
    UNAVAILABLE,
    AUTHENTICATION_FAILED,
    STOPPED,
}

data class OutputStatus(
    val id: String,
    val name: String,
    val state: OutputConnectionState,
    val detail: String? = null,
    val bitrateBps: Long = 0,
)
