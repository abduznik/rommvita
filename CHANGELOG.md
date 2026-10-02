# Changelog

Supported systems: **SNES only** for now.

## v0.2.0
- New purple theme based on the RomM logo, with the logo in the app, a LiveArea icon, background and startup image
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
