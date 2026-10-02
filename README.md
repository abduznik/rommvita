# RomM Vita

A [RomM](https://github.com/rommapp/romm) client for the PlayStation Vita (VitaSDK homebrew).

**Status: MVP - connection only.** The app pairs with a RomM server and verifies the
connection. ROM browsing/downloading and RetroArch launching are planned.

## Features
- Server URL entry with **automatic http/https detection** (probes `/api/heartbeat`, follows redirects)
- Pairing with RomM **client API tokens**: type the code (`XXXX-XXXX`) or **scan the QR** with the Vita camera
- Verifies the token (`GET /api/users/me`) and shows a green/red status indicator
- Saves the server + token to `ux0:data/RomMVita/config.txt` and re-verifies on launch

## Usage
1. In the RomM web UI create a Client API Token and generate its pairing code / QR.
2. Open RomM Vita, enter the server address (scheme optional) and the code, or choose **Scan QR**.
3. Select **Connect**. Green = connected.

Controls: D-pad select, Cross edit/confirm, Start exit. In the scanner: Triangle switches camera, Circle cancels.

## Building
Requires [VitaSDK](https://vitasdk.org/). Note: libcurl in VitaSDK links against OpenSSL 1.0.2
(the package that conflicts with openssl-1.1.1 in vdpm), so install that one.

```sh
mkdir build && cd build
cmake .. && make
```
Output: `build/rommvita.vpk`. Install with VitaShell.

## Notes
- TLS certificates are **not verified** (the Vita has no usable CA store); traffic is encrypted but the server is not authenticated. TLS is limited to 1.2.
- QR decoding uses [quirc](https://github.com/dlbeer/quirc) (ISC license, in `src/quirc/`).

## Roadmap
- Browse platforms/ROMs, cover art
- ROM downloads to per-platform folders
- Launch games via a RetroArch wrapper
