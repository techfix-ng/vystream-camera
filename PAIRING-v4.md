# Pairing behaviour

OBS listens on `0.0.0.0` so every active Ethernet, Wi-Fi or hotspot adapter can receive SRT. That bind address is not sent to or saved by the phone.

Discovery offers contain:

`OBS_SRT_OFFER_V3|computer-name|routable-ip|port|stable-pairing-id|camera-name`

The pairing ID is persisted per phone in the plugin configuration. Android matches it to the saved SRT profile and silently replaces only the IP and port when DHCP changes them.

The Android endpoint QR scanner accepts the equivalent payload:

```json
{"v":1,"name":"Camera name","url":"srt://192.168.1.10:9000","videoCodec":"H264","srtLatencyMs":200,"srtMode":"CALLER","srtStreamId":"stable-pairing-id","isDefault":true}
```

The listener shown in OBS will still read `srt://0.0.0.0:PORT?mode=listener`; this is intentional. The phone-facing address is the routable IP from the offer/QR payload.
