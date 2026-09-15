# VyStream Android 2.7.0 — automatic multi-output

- Every valid saved SRT, RTMP and RTMPS connection participates automatically.
- One Camera2 and MediaCodec session fans encoded frames to all destinations.
- Available destinations go live concurrently.
- Unavailable destinations retry independently with exponential backoff.
- Authentication failures pause only the affected destination.
- A failed destination never stops healthy outputs or local recording.
- When SRT participates, the shared compatibility codec is H.264.
- The camera screen labels every saved output as Auto and shows the output count.

The encoder is shared, but upload bandwidth is multiplied by the number of live
destinations. Two 6 Mbps outputs require approximately 12 Mbps plus overhead.

The implementation targets RootEncoder 2.5.5 and uses its public low-level
`RtmpClient` and `SrtClient` APIs behind one `Camera2Base` pipeline.
