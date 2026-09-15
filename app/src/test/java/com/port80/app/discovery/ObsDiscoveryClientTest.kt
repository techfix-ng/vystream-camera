package com.port80.app.discovery

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ObsDiscoveryClientTest {
    @Test
    fun `V4 offer preserves stable receiver identity`() {
        val offer = ObsDiscoveryClient.parseOffer(
            "OBS_SRT_OFFER_V4|550e8400-e29b-41d4-a716-446655440000|STREAM-PC|192.168.1.100|9000|token123|Main-Cam|windows|obs|3.0.0",
            "192.168.1.114",
            "Android Camera"
        )

        requireNotNull(offer)
        assertEquals("550e8400-e29b-41d4-a716-446655440000", offer.receiverId)
        assertEquals("192.168.1.114", offer.address)
        assertEquals(9000, offer.srtPort)
        assertEquals("STREAM-PC", offer.name)
        assertEquals("Main-Cam", offer.assignedCameraName)
        assertEquals("windows", offer.platform)
        assertEquals("3.0.0", offer.pluginVersion)
    }

    @Test
    fun `same V4 receiver keeps identity after DHCP change`() {
        val first = ObsDiscoveryClient.parseOffer(
            "OBS_SRT_OFFER_V4|receiver-uuid|STREAM-PC|192.168.1.100|9000|token|Camera|windows|obs|3.0.0",
            "192.168.1.100",
            "Android Camera"
        )
        val changed = ObsDiscoveryClient.parseOffer(
            "OBS_SRT_OFFER_V4|receiver-uuid|STREAM-PC|192.168.1.115|9000|token|Camera|windows|obs|3.0.0",
            "192.168.1.115",
            "Android Camera"
        )

        assertEquals(first?.receiverId, changed?.receiverId)
        assertTrue(first?.address != changed?.address)
    }

    @Test
    fun `legacy V3 offer remains supported`() {
        val offer = ObsDiscoveryClient.parseOffer(
            "OBS_SRT_OFFER_V3|STREAM-PC|192.168.1.100|9000|token|Camera",
            "192.168.1.100",
            "Android Camera"
        )

        requireNotNull(offer)
        assertEquals("192.168.1.100:9000", offer.receiverId)
        assertEquals("STREAM-PC", offer.name)
    }

    @Test
    fun `invalid ports are rejected`() {
        assertNull(
            ObsDiscoveryClient.parseOffer(
                "OBS_SRT_OFFER_V4|receiver|PC|192.168.1.10|0|token|Camera|windows|obs|3.0.0",
                "192.168.1.10",
                "Android Camera"
            )
        )
    }
}
