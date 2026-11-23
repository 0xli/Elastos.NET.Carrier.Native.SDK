# Session memory

Last updated: 2026-11-20

- Branch `codex/carrier-idle-suspend` adds `carrier_set_network_suspended`.
  The run loop drops peer sessions, UDP, and TCP relays, then waits for a
  resume request. Resume reestablishes the network path and pulls Express mail.
- Android callers resume on foreground entry or a wake push. The Carrier
  handle and locally cached friend metadata remain available while idle.
- Build notes are in `docs/REBUILD-IOS-ANDROID.md`. The Android JNI binding
  lives in Elastos.NET.Carrier.Android.SDK on the branch of the same name.
