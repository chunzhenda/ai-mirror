# AI Mirror · AI 智能魔镜

**English** | [简体中文](./README.md)

---

> An open-source **ESP32-S3** smart mirror hardware prototype that pairs a round LCD face animation with an ES8311 audio codec, WS2812 RGB LED, on-device Wi-Fi provisioning, and an onboard acoustic-echo-cancellation (AEC) demo. Firmware and PCB manufacturing files are both open-sourced.

---

## Features

### Display & Face Animation
- **240×240 GC9A01 round LCD** driven over SPI, rendered through LVGL + `esp_lvgl_port`, double-buffered.
- **Multi-scene animated cute face** with 6 cycling scenes: `IDLE`, `HAPPY`, `SAD`, `ANGRY`, `LISTENING`, `BLINK`, switching every 2.6s.
- **Procedural motion** - smoothstep easing, eye-tracking drift, idle/fast blink, floating sparkles, cheek blush, tear drops, eyebrows, and listening sound-wave bars, all computed with integer-only math on the LVGL refresh path.

### Audio System
- **ES8311 codec** + **I²S** full-duplex link at 16 kHz / 16-bit, configurable mic gain (0–42 dB) and output volume (0–100).
- Two selectable audio modes (via `menuconfig`):
  - **Music mode** - loops the embedded `canon.pcm` track.
  - **AEC demo mode** (default) - an Acoustic Echo Cancellation demo built on **esp-sr** `aec_pro`: plays a 330 Hz reference tone, captures the mic, runs AEC to remove the echo, and records the cleaned audio while holding **BTN1**; ~2 s after release it plays back a peak-normalized version so you can hear your voice with the tone suppressed.
- Graceful degradation: if audio init fails, the rest of the device keeps running.

### Wi-Fi Provisioning
- **Dual transport** selectable on-device: **SoftAP** or **BLE**, using the ESP-IDF `wifi_provisioning` manager with Security-1 (PoP) handshake.
- **QR code on the round display** - scan with the *ESP BLE Provisioning* / *Espressif Provisioning* phone app to configure Wi-Fi, no PC needed.
- **Boot decision state machine**:
  - No saved credentials -> enters provisioning automatically.
  - Has saved credentials -> auto-connects; **hold BTN1 for 2s** during the boot window to force re-provisioning.
- **Internet connectivity check** (HTTP probe to `connectivitycheck.gstatic.com` / `espressif.com`) with on-screen retry/continue prompt.
- Saved Wi-Fi credentials persist in NVS across reboots.

### On-screen UI Screens
All built with LVGL on the round display, sharing a consistent visual language (accent dot, halo, centered labels):
- Boot / status messages
- Provisioning transport selection (AP / BLE toggle)
- QR code provisioning screen with service name & transport
- Internet check success / warning (retry vs continue)
- Setup-failure screen with button navigation

### Buttons & RGB LED
- **2 GPIO buttons** (BTN1 = GPIO13, BTN2 = GPIO14, active-high) with a 10 ms scan / 30 ms debounce state machine, supporting press/release callbacks and long-press detection.
- **WS2812 RGB LED** (GPIO48) driven via RMT with a custom WS2812 encoder, HSV->RGB conversion, and a boot-time rainbow demo animation.

### Hardware & Manufacturing Files
- **PCB Gerber** archives + **BOM** provided for fabrication and reproduction.
- **AXP2101** power-management-IC datasheet included.
- Optional **BSP** (Board Support Package) support for the ESP-BOX board.

---

## Repository Structure

```
.
├── Firmware/esp32_ai_mirror/        # ESP-IDF firmware project
│   ├── main/
│   │   ├── ai_mirror_main.c         # Entry point: boot flow, audio/display/network orchestration
│   │   ├── ai_mirror_ui.c           # LVGL face animation + provisioning UI screens
│   │   ├── ai_mirror_config.h       # Audio & I²C/I²S pin definitions
│   │   ├── board_buttons.c/h        # Debounced 2-button input driver
│   │   ├── board_rgb.c/h            # WS2812 RGB LED driver + rainbow demo
│   │   ├── board_wifi_prov.c/h      # Wi-Fi provisioning (SoftAP/BLE) + saved-credential connect
│   │   ├── canon.pcm                # Embedded 16 kHz/16-bit music clip
│   │   ├── Kconfig.projbuild        # Audio mode / mic gain / volume / BSP menuconfig
│   │   └── idf_component.yml        # Component Manager dependencies
│   ├── components/                  # Local components: esp_lcd_gc9a01, esp-sr
│   ├── managed_components/          # Auto-fetched IDF components
│   └── sdkconfig*                   # ESP32-S3 build configuration
├── PCB/
│   ├── Gerber_PCB1_*.zip            # PCB manufacturing files
│   └── BOM_Board1_PCB1_*.xlsx       # Bill of materials
└── datasheet/
    └── AXP2101.pdf                  # PMIC datasheet
```

---

## Hardware Overview

Default firmware targets **ESP32-S3** with the following peripherals:

| Module | Interface / Pins |
| --- | --- |
| MCU | ESP32-S3 |
| Display | GC9A01, SPI, 240×240 |
| Display pins | SCLK GPIO18, MOSI GPIO19, MISO GPIO21, DC GPIO5, RST GPIO3, CS GPIO4, Backlight GPIO2 |
| Audio codec | ES8311, I²C + I²S |
| ES8311 I²C | SCL GPIO9, SDA GPIO10 |
| ES8311 I²S | MCLK GPIO6, BCLK GPIO7, WS GPIO8, DOUT GPIO11, DIN GPIO12 |
| Buttons | BTN1 GPIO13, BTN2 GPIO14 (active-high) |
| RGB LED | WS2812, GPIO48 (RMT-driven) |

Pin definitions live in [`Firmware/esp32_ai_mirror/main/ai_mirror_config.h`](Firmware/esp32_ai_mirror/main/ai_mirror_config.h), [`board_buttons.h`](Firmware/esp32_ai_mirror/main/board_buttons.h), and [`board_rgb.h`](Firmware/esp32_ai_mirror/main/board_rgb.h). Verify against your schematic before flashing a different board.

---

## Getting Started

### 1. Prepare the environment
Install ESP-IDF 5.x and load its environment in your terminal.

### 2. Build
```bash
cd Firmware/esp32_ai_mirror
idf.py set-target esp32s3
idf.py build
```
The ESP-IDF Component Manager auto-fetches dependencies listed in `main/idf_component.yml` on first build.

### 3. Configure (optional)
```bash
idf.py menuconfig
```
Under **AI Mirror Configuration** choose:
- Audio mode: **music playback** or **microphone echo test (AEC demo)**
- MIC gain (0–42 dB, echo mode only)
- Voice volume (0–100)
- Enable Board Support Package (BSP)

### 4. Flash & monitor
Replace `PORT` with your board's serial port (e.g. `COM3` on Windows):
```bash
idf.py -p PORT flash monitor
```
Exit the monitor with `Ctrl-]`.

---

## First-boot Wi-Fi Setup

1. On first boot (no saved Wi-Fi), the mirror enters provisioning and shows a **transport selector** - use **BTN2** to toggle SoftAP/BLE, **BTN1** to confirm.
2. A **QR code** appears on the round display. Scan it with the *ESP BLE Provisioning* app (Android/iOS) and follow the app to pick your Wi-Fi and enter the password.
   - **PoP (Proof of Possession)**: `abcd1234`
3. The device saves credentials to NVS, connects, and runs an internet check. On success the face animation starts.
4. On later boots the device auto-connects. **Hold BTN1 for 2s** during boot to re-enter provisioning and change networks.

---

## Customization

### Replace the face animation
The face is fully procedural in [`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c) - scenes, easing, and elements are defined in code (no image assets). Edit the `face_scene_t` enum and `draw_scene()` to change expressions.

### Replace the music
Music mode embeds `main/canon.pcm`. Prepare 16 kHz / 16-bit PCM, replace the file, and keep the `EMBED_FILES` entry in `main/CMakeLists.txt` and the `_binary_*_start/end` symbols in `ai_mirror_main.c` consistent.

### Convert any audio to PCM
```bash
ffmpeg -i input.mp3 -ss 00:00:00 -t 00:00:20 -f s16le -ar 16000 -ac 1 -acodec pcm_s16le canon.pcm
```

---

## PCB Files

The `PCB/` directory contains Gerber archives and the BOM. Review the version, stack-up, impedance, and footprints before manufacturing - these files are provided as-is.

---

## Development Notes

- Entry point: [`ai_mirror_main.c`](Firmware/esp32_ai_mirror/main/ai_mirror_main.c) - `app_main()` boot flow.
- Face + UI: [`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c).
- Wi-Fi provisioning: [`board_wifi_prov.c`](Firmware/esp32_ai_mirror/main/board_wifi_prov.c).
- Buttons: [`board_buttons.c`](Firmware/esp32_ai_mirror/main/board_buttons.c).
- RGB LED: [`board_rgb.c`](Firmware/esp32_ai_mirror/main/board_rgb.c).
- Menuconfig: [`Kconfig.projbuild`](Firmware/esp32_ai_mirror/main/Kconfig.projbuild).

Issues and Pull Requests to improve hardware, UI, and firmware are welcome.

---

## Roadmap (not yet implemented)

Cloud AI dialogue (ASR / LLM / TTS), WebSocket communication, VAD (voice activity detection), and OTA updates are planned but not yet implemented - see the internal analysis docs `小智项目分析.md` and `WiFi配网功能计划书.md`.

---

## License

No license file is included yet. Contact the maintainer before using, modifying, or distributing this project.
