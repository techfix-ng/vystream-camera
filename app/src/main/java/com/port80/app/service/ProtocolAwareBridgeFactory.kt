package com.port80.app.service

import com.pedro.common.ConnectChecker
import com.port80.app.data.model.StreamProtocol
import javax.inject.Inject

/**
 * Creates the shared multi-output bridge. The protocol argument remains for API
 * compatibility; the returned bridge hosts RTMP(S) and SRT clients together.
 */
class ProtocolAwareBridgeFactory @Inject constructor() : EncoderBridge.Factory {
    override fun create(connectChecker: ConnectChecker, protocol: StreamProtocol): EncoderBridge =
        RtmpCamera2Bridge(connectChecker)
}
