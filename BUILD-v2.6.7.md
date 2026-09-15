# VyStream Android 2.6.7 — multi-destination endpoint fix

## Fixed

- OBS 1, OBS 2, vMix and manually configured destinations can remain saved at
  the same time.
- Selecting a destination updates the active in-memory endpoint immediately,
  eliminating the Select -> Go Live race with encrypted preference storage.
- Go Live now starts the endpoint visibly selected on the camera screen.
- A newly discovered OBS/vMix desktop becomes active without deleting or
  overwriting older profiles.
- Deleting the active profile safely falls back to the saved default or first
  remaining endpoint.

## Connection types

- Explicit SRT, RTMP and RTMPS selection in the endpoint editor.
- RTMP/RTMPS continue to use the existing encoder implementation and require an
  ingest-server URL plus stream key.
- OMT is shown as Coming Soon until the native Android OMT sender is integrated;
  no nonfunctional OMT transport is advertised.

Compatible with VyStream OBS Dock 2.9.0 compact-interface release.
