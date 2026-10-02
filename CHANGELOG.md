# Changelog

Supported systems: **SNES** and **Game Boy Advance (experimental)**. More are planned.

## v0.3.0
- **Platform manager:** after connecting you choose a platform. Only the systems we plan to support are listed;
  SNES and Game Boy Advance work, the rest are marked "Planned".
- **Game Boy Advance (experimental):** library, downloads and RetroArch launch with gpSP (falls back to mGBA, then
  VBA Next), with save and save-state sync. Tracked in issue #2; it stays open until tested end-to-end.
- **Offline mode:** if the server cannot be reached, open the downloaded games without logging in again. A panel lists
  saves and states that changed on the Vita and will sync when you are back online.
- Library caches are now kept per platform.
- Plain navy LiveArea background, and the version number is shown on the connect screen.
- Saves and states are uploaded as **new, timestamped versions** (`Game [2026-09-20_19-49-15].srm`) instead of
  overwriting the previous one, so RomM keeps a history. The file name carries only the date and time; the app marks
  its uploads with the tag `rommvita` in RomM's slot and emulator fields. Only the newest 10 versions per game are
  kept, and only this app's own versions are ever pruned. Pulled files are saved under the game's own local name
  without the date and time.

## v0.2.0
- New navy/slate theme (same palette as Freegosy) with a half-cropped Vita logo in the app, LiveArea icon, background and startup image
- **SNES library**: browse, search (local and instant) and an "installed only" view (Select)
- Library **cache** with background refresh, so it opens instantly
- **Downloads** ROMs to `ux0:data/RomMVita/roms/snes/` with a progress bar
- **Launches games in RetroArch** (needs RetroArch for Vita installed separately)
- **Save and state sync** with RomM, with backups and core-compatibility checks for states
  ("Incompatible cores" popup)
- App is now an *unsafe* homebrew so it can see RetroArch's folders
- Setup guide in the README

## v0.1.0
- Connection test: server URL with automatic http/https detection
- Pair with a RomM client token by pairing code (`XXXX-XXXX`) or by scanning the QR with the Vita camera
- Green/red connection indicator, token saved and re-verified on launch
