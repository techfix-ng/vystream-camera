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

## Tally state packets

After pairing, OBS sends the camera a UDP tally packet on the advertised talkback port:

`VYSTALLY1|<pairing-token>|LIVE` when the camera's scene/source is in Program,
`VYSTALLY1|<pairing-token>|PREVIEW` when it is in Preview, and
`VYSTALLY1|<pairing-token>|STANDBY` when it is neither. The camera should only show the green live indicator for the authenticated `LIVE` state; a connected stream by itself remains orange standby.
