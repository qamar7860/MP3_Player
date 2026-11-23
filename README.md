# ESP32 Cyberpunk / Retro-Tech Portable MP3 & Bluetooth Hi-Fi Player

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x%20%7C%20v6.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/Platform-ESP32--D0WD--V3-orange.svg)](https://www.espressif.com/)
[![GUI](https://img.shields.io/badge/GUI-LVGL%20v9-brightgreen.svg)](https://lvgl.io/)
[![Bluetooth](https://img.shields.io/badge/Audio-A2DP%20Source%20%2B%20AVRCP-blueviolet.svg)](https://www.bluetooth.com/)
[![Audio](https://img.shields.io/badge/Decoder-minimp3%20%2B%20WAV-red.svg)](https://github.com/lieff/minimp3)
[![License](https://img.shields.io/badge/License-MIT%20%2F%20CC0-lightgrey.svg)](LICENSE)

An advanced, battery-powered portable music player built around the **ESP32** dual-core microcontroller. It streams decoded audio over **Bluetooth Classic (A2DP Source)** to wireless headphones or Bluetooth speakers with full **AVRCP hardware button control**, drives a **2.4" / 2.8" SPI TFT LCD (240x320)** with a sleek **Cyberpunk / Retro-Tech UI powered by LVGL 9**, and indexes your music collection directly from a **FAT32 MicroSD card**.

---

## Table of Contents

- [Key Features](#key-features)
- [System Architecture & Dual-Core Threading](#system-architecture--dual-core-threading)
- [Hardware & Pinout](#hardware--pinout)
- [Audio Engine & Pipeline](#audio-engine--pipeline)
- [User Interface & Screens](#user-interface--screens)
- [Controls & Navigation](#controls--navigation)
  - [Physical Button Mapping](#physical-button-mapping)
  - [Bluetooth Headphone Gestures](#bluetooth-headphone-gestures)
- [Power & Battery Management](#power--battery-management)
- [Repository Structure](#repository-structure)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [MicroSD Card Preparation](#microsd-card-preparation)
  - [Build and Flash](#build-and-flash)
- [Troubleshooting & Diagnostics](#troubleshooting--diagnostics)
- [License & Acknowledgments](#license--acknowledgments)

---

## Key Features

- **Hi-Fi Bluetooth Audio Streaming (A2DP Source)**:
  - Streams uncompressed 44.1 kHz 16-bit stereo PCM audio to any standard Bluetooth headset, TWS earbuds, or wireless speaker.
  - Automatic reconnection to the last paired audio device persisted in NVS flash.
- **Full AVRCP Media & Gesture Control**:
  - Responds directly to headphone tap gestures: Play/Pause (single tap), Next Track (double tap), Previous Track (triple tap), and Volume Adjustment (hold).
  - Software debouncing and state-machine filtering to eliminate ghost triggers and accidental skips.
- **Dual-Format Audio Decoder**:
  - **MP3**: Full MPEG-1/2/2.5 Layer III decoding powered by `minimp3`, supporting bitrates from 32 kbps up to 320 kbps (CBR and VBR).
  - **WAV**: Direct streaming for 16-bit uncompressed studio PCM WAV audio (Mono/Stereo, automatic channel upmixing).
- **In-RAM Zero-Contention MicroSD Playlist Indexer**:
  - Recursively scans the SD card on boot and caches the entire playlist in RAM ($O(1)$ instantaneous lookups).
  - Eliminates secondary SD file access during playback, guaranteeing **zero SPI bus contention** and preventing audio stutter or "Unknown Track" faults.
- **Cyberpunk / Retro-Tech LVGL 9 Interface**:
  - Smooth 240x320 TFT display with a dark navy & neon cyan aesthetic.
  - Circular marquee text scrolling for long track names (`LV_LABEL_LONG_SCROLL_CIRCULAR`).
  - Interactive playback timeline showing elapsed time, duration, and remaining countdown (`-MM:SS`).
  - Ephemeral vertical Volume HUD pop-up overlay with auto-hide.
  - Real-time status bar displaying Date & Clock, Shuffle badge, Bluetooth connection indicator, and Battery icon.
- **NVS Playback State Persistence**:
  - Automatically saves the active track, playback elapsed time (within 4 seconds), master volume, and shuffle state to Non-Volatile Storage (NVS).
  - Seamlessly resumes playback where you left off after power cycles.
- **Deep Power Optimization & Battery Saver**:
  - **Power Off**: One-touch entry into ultra-low power ESP32 Deep Sleep (~10 $\mu$A).
  - **Battery Saver Mode**: Turn off TFT backlight and display scanning while keeping background Bluetooth audio streaming active.
  - Onboard 12-bit ADC battery voltage measurement (3.3V–4.2V LiPo curve) and internal ESP32 SoC temperature monitoring.

---

## System Architecture & Dual-Core Threading

To achieve glitch-free audio decoding alongside an interactive GUI and Bluetooth protocol stack, the system is strictly decoupled across the ESP32's dual Tensilica Xtensa LX6 cores:

```
                  ESP32 DUAL-CORE ARCHITECTURE
       ===================================================

                CORE 0                           CORE 1
       +-----------------------+       +-----------------------+
       |   Bluetooth Classic   |       |      Player Task      |
       |  Controller & Modem   |       |   (Priority 6, 24KB)  |
       +-----------+-----------+       +-----------+-----------+
                   |                               |
       +-----------v-----------+                   | Reads & decodes
       |   Bluedroid Host:     |                   | MP3 / WAV
       |  A2DP Source (SBC)    |                   v
       |   AVRCP CT / TG       |       +-----------------------+
       +-----------+-----------+       |  16 KB StreamBuffer   |
                   ^                   |     (PCM Ring)        |
                   |                   +-----------+-----------+
       Pulls audio |                               |
       non-blocking|                               |
                   +-------------------------------+
                                                   |
                                       +-----------v-----------+
                                       |      LVGL Engine      |
                                       |   (Priority 3, 8KB)   |
                                       |      Display SPI      |
                                       +-----------+-----------+
                                                   |
                                       +-----------v-----------+
                                       |    3-Button Poller    |
                                       |      (Timer 20ms)     |
                                       +-----------------------+
```

| Task | Core | Priority | Stack | Description |
| :--- | :---: | :---: | :---: | :--- |
| **BT Controller** | Core 0 | High (OS) | — | Hardware radio controller, frequency hopping, and baseband. |
| **Bluedroid / A2DP** | Core 0 | OS Managed | — | SBC encoder and A2DP source transmission pipeline. |
| **`player_task`** | Core 1 | **6** | 24 KB | Reads audio from SD, runs `minimp3` decoding, feeds ring buffer. |
| **`lvgl_handler`** | Core 1 | **3** | 8 KB | Executes LVGL display rendering and screen transitions. |
| **`lvgl_tick`** | Core 1 | **5** | 2 KB | Increments LVGL timebase every 5 ms. |
| **`button_poller`** | Core 1 | Timer | — | Debounces physical buttons (Power, Nav, Action) every 20 ms. |

---

## Hardware & Pinout

### Recommended Components
- **Microcontroller**: ESP32 DevKit v1 / ESP32-WROOM-32 / ESP32-D0WD (4MB Flash).
- **Display**: 2.4" or 2.8" SPI TFT LCD (ILI9341 or ST7789, 240x320 pixels).
- **Storage**: MicroSD Card Module (SPI interface, with 3.3V level conversion or direct 3.3V).
- **Power**: 3.7V Single-Cell LiPo / 18650 Battery + TP4056 USB-C charging module.
- **Controls**: 3x Tactile push buttons (Active Low with internal pull-ups).

### Wiring Map

#### 1. Display Interface (`SPI3_HOST` / VSPI)
| Display Pin | ESP32 GPIO | Description |
| :--- | :---: | :--- |
| **VCC** | 3.3V / 5V | Display Power (check module regulator) |
| **GND** | GND | Common Ground |
| **CS** | **GPIO 5** | SPI Chip Select |
| **RESET** | **GPIO 4** | Hardware Reset |
| **DC** / RS | **GPIO 2** | Data / Command Select |
| **MOSI** / SDI | **GPIO 23** | SPI Master-Out Slave-In |
| **SCK** / CLK | **GPIO 18** | SPI Clock |
| **MISO** / SDO | **GPIO 19** | SPI Master-In Slave-Out |
| **LED** / BL | 3.3V | Backlight Power |

#### 2. MicroSD Card Interface (`SPI2_HOST` / HSPI)
| MicroSD Pin | ESP32 GPIO | Notes |
| :--- | :---: | :--- |
| **VCC** | 3.3V | Clean 3.3V Power Rail |
| **GND** | GND | Common Ground |
| **CS** | **GPIO 15** | Dedicated SD SPI Chip Select (Internal Pullup) |
| **MOSI** / DI | **GPIO 13** | SD SPI MOSI (Internal Pullup) |
| **SCK** / CLK | **GPIO 14** | SD SPI Clock (10 MHz) |
| **MISO** / DO | **GPIO 12** | SD SPI MISO (Internal Pullup) |

> **Note on GPIO 12:** GPIO 12 is an ESP32 strapping pin (controls flash voltage VDD_SDIO on boot). Ensure your SD module does not pull this pin high on boot if your ESP32 uses 3.3V flash (the firmware configures internal pull-ups after boot).

#### 3. Physical Buttons & Telemetry
| Component | ESP32 GPIO | Mode | Description |
| :--- | :---: | :---: | :--- |
| **Button 1 (Power)** | **GPIO 32** | Input (Pull-up) | Power Off (Short click) / Battery Saver (Hold) |
| **Button 2 (Nav)** | **GPIO 33** | Input (Pull-up) | Navigate Screen / Advance List Selection |
| **Button 3 (Action)**| **GPIO 25** | Input (Pull-up) | Play/Pause, Select Item, Toggle Shuffle |
| **Battery ADC** | **GPIO 34** | ADC1 Channel 6 | Voltage Divider input for LiPo monitoring |

---

## Audio Engine & Pipeline

```
 [MicroSD Card]
       │
       ▼ (10 MHz SDSPI)
 [Read Buffer: 4 KB]
       │
       ▼
 [minimp3 / WAV Decoder]
       │ (16-bit 44.1kHz PCM)
       ▼
 [Software Volume Attenuator (0-100%)]
       │
       ▼
 [PCM Ring Buffer: 16 KB (~93 ms latency margin)]
       │
       ▼ (Non-blocking callback)
 [Bluedroid A2DP Source]
       │
       ▼ (SBC Radio Stream)
 [Bluetooth Headphones / Speaker]
```

1. **Pre-buffering**: When a song starts or resumes, 6 KB of PCM audio (~35 ms) is pre-buffered into the FreeRTOS StreamBuffer before Bluetooth transmission engages, preventing startup clicks or initial audio dropout.
2. **Glitch-Resilient Decoding**: The decoder gracefully handles ID3v2 metadata frames, corrupted sync bytes, and variable bitrates by auto-resynchronizing rather than dropping the track.
3. **WAV Streaming**: Uncompressed 16-bit PCM WAV audio is decoded on the fly with zero MP3 decompression overhead, converting mono tracks to stereo in-place.

---

## User Interface & Screens

The player features a four-screen carousel designed with LVGL 9:

### 1. Home Screen (Now Playing)
- **Top Status Bar**: Live Date & Clock (`DD Mon  HH:MM`), Shuffle badge (`[SHUF]`), Bluetooth link indicator (glowing cyan when paired), and Battery percentage with dynamic icon.
- **Center Art**: Cyberpunk retro-cassette background with a glowing neon accent card.
- **Track Information**:
  - Song title with continuous horizontal marquee scrolling for long filenames.
  - Subtitle metadata: Codec, Sample Rate, and Play/Pause state.
- **Timeline & Scrubber**: Real-time progress bar with current elapsed time and remaining time countdown (`-MM:SS`).
- **Media Controls**: Circular illuminated Prev, Play/Pause, and Next buttons.
- **Volume HUD**: Vertical overlay pops up automatically whenever volume is modified and dismisses after 1.5 seconds.

### 2. Library Screen (Playlist Browser)
- Displays all audio files found on the MicroSD card.
- Uses a **6-slot sliding viewport** that smoothly scrolls through hundreds of songs with minimal memory footprint.
- Highlights the currently active track in glowing cyan.

### 3. Bluetooth Screen (Device Manager)
- Scans for nearby discoverable Bluetooth audio sinks (headphones, soundbars, TWS earbuds).
- Lists discovered devices by name with signal strength.
- One-click pairing and automatic persistent storage in NVS for instant reconnection on subsequent power-ons.

### 4. Settings Screen (Diagnostic Console)
- Displays live hardware telemetry:
  - Master Volume level.
  - Active Bluetooth target MAC & connection state.
  - SD Card status and indexed song count.
  - Battery voltage (mV) and remaining capacity (%).
  - Real-time ESP32 internal DRAM heap and largest free block.
  - Internal SoC silicon temperature in °C.
  - Battery Saver mode toggle.

---

## Controls & Navigation

### Physical Button Mapping

| Action | Button 1 (GPIO 32 - Power) | Button 2 (GPIO 33 - Nav) | Button 3 (GPIO 25 - Action) |
| :--- | :--- | :--- | :--- |
| **Short Press** | **Power Off** (Enters Deep Sleep) | Advance cursor / Next screen | **Play / Pause** (or Confirm selection) |
| **Long Hold (>450ms)**| **Battery Saver** (Screen Off / Audio On)| Quick Jump between screens | **Toggle Shuffle** (Home) / Return to Home |

### Bluetooth Headphone Gestures

If your Bluetooth headphones or TWS earbuds support AVRCP media buttons, you can control playback without touching the device:

| Headphone Tap Gesture | Triggered Action |
| :--- | :--- |
| **Single Tap** | Play / Pause toggle |
| **Double Tap (Right Earbud)** | Next Track (`HP_ACTION_NEXT_SONG`) |
| **Double Tap (Left Earbud)** | Previous Track (`HP_ACTION_PREV_SONG`) |
| **Long Press / Hold** | Volume Up / Volume Down |

---

## Power & Battery Management

- **LiPo Voltage Curve**: The battery monitor reads the single-cell LiPo voltage through an ADC voltage divider on GPIO 34. The percentage is calculated using a calibrated 3.3V (0%) to 4.2V (100%) curve.
- **Battery Saver Mode**: Holding the Power button or toggling Battery Saver in Settings puts the ILI9341 display into hardware sleep (`0x10 Enter Sleep`) and shuts off frame updates. Background audio playback and Bluetooth streaming continue smoothly, drastically extending battery runtime.
- **Deep Sleep**: Clicking the Power button turns off the display and puts the ESP32 into deep sleep. Pressing the button again wakes the device via an RTC GPIO wake interrupt.

---

## Repository Structure

```
MP3_Player/
├── CMakeLists.txt              # Top-level project CMake file
├── partitions.csv              # Custom flash partition table (4MB Flash)
├── sdkconfig.defaults          # Optimized ESP-IDF defaults (CPU 160MHz, Bluetooth, FatFS)
├── README.md                   # This documentation
│
└── main/
    ├── CMakeLists.txt          # Main component build configuration
    ├── idf_component.yml       # Component dependencies (LVGL v9)
    ├── lv_conf.h               # LVGL configuration (color depth, memory)
    ├── minimp3.h               # High-performance single-header MP3 decoder
    │
    ├── main.c                  # System entrypoint (app_main), task initialization
    ├── Player.h / Player.c     # Audio pipeline, minimp3 decoding, FreeRTOS ring buffer
    ├── Bluetooth.h / .c        # Classic Bluetooth A2DP Source & AVRCP controller
    ├── SD_Card.h / SD_Card.c   # In-RAM playlist indexing, SDSPI driver
    ├── SD_Bench.c              # Startup MicroSD read-speed benchmark
    ├── Battery.h / Battery.c   # 12-bit ADC battery monitor & percentage calculation
    ├── Temperature.h / .c      # Internal ESP32 SoC temperature sensor driver
    ├── BG.c / wallpaper.c      # Compressed RGB565 UI background art assets
    │
    ├── Display/
    │   ├── Display.h           # Display pin definitions and prototypes
    │   └── Display.c           # ILI9341 SPI driver, LVGL display buffer flusher
    │
    └── UI.h / UI.c             # LVGL 9 GUI: Home, Library, Bluetooth, and Settings
```

---

## Getting Started

### Prerequisites

1. **ESP-IDF**: Install ESP-IDF **v5.1 or newer** (tested and verified on **ESP-IDF v6.1-dev / v6.2**):
   ```bash
   git clone -b release/v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
   cd ~/esp/esp-idf
   ./install.sh esp32
   source ./export.sh
   ```
2. **Serial Port Permissions** (Linux):
   ```bash
   sudo usermod -a -G dialout $USER
   ```

### MicroSD Card Preparation

1. Format your MicroSD card as **FAT32** with a standard allocation unit size (32 KB recommended).
2. Copy your music collection directly onto the card. Supported formats:
   - **`.mp3`**: Standard MP3 files (any bitrate up to 320 kbps, 44.1 kHz recommended).
   - **`.wav`**: 16-bit PCM WAV audio.
3. Subfolders are automatically scanned up to 3 directory levels deep.

### Build and Flash

1. **Clone the repository**:
   ```bash
   git clone https://github.com/qamar7860/ESP32_MP3_Player.git
   cd ESP32_MP3_Player
   ```

2. **Set target to ESP32**:
   ```bash
   idf.py set-target esp32
   ```

3. **Build the firmware**:
   ```bash
   idf.py build
   ```

4. **Flash and Monitor**:
   Connect your ESP32 via USB and run:
   ```bash
   idf.py -p /dev/ttyUSB0 -b 115200 flash monitor
   ```
   *(Replace `/dev/ttyUSB0` with your serial port, e.g., `COM3` on Windows).*

5. Press `Ctrl + ]` to exit the serial monitor.

---

## Troubleshooting & Diagnostics

- **SD Card Init Failure**:
  - Check the SPI pin connections (MISO GPIO 12, MOSI GPIO 13, SCK GPIO 14, CS GPIO 15).
  - Ensure the SD card is formatted as FAT32, not exFAT or NTFS.
  - If using jumper wires on a breadboard, ensure wire lengths are under 10 cm to prevent signal degradation at 10 MHz SPI clock.
- **Display Shows Inverted or Tinted Colors**:
  - The driver sets `MADCTL = 0x00` (RGB mode) in [`Display.c`](file:///home/qamar/MP3_Player/main/Display/Display.c#L132). If your display uses BGR panel layout, change this to `0x08`.
- **Bluetooth Headphones Won't Connect**:
  - Ensure your headphones are in **Pairing Mode** (flashing LED).
  - Navigate to the **Bluetooth** screen on the player, click to start scanning, and select your headset from the list once detected.
  - The player will remember your device and automatically reconnect on future boots.
- **Audio Stutter / Underruns**:
  - The built-in startup benchmark (`SD_Bench.c`) requires at least 40 KB/s read throughput for 256 kbps audio. Normal cards deliver 400–800 KB/s over SPI. Use a Class 10 or UHS-1 MicroSD card for optimal performance.

---

## License & Acknowledgments

- **minimp3**: Fast, lightweight MP3 decoder created by [Lieff](https://github.com/lieff/minimp3) (CC0 / Public Domain).
- **LVGL**: Light and Versatile Graphics Library under the [MIT License](https://github.com/lvgl/lvgl).
- **Espressif Systems**: For the ESP-IDF framework, FreeRTOS implementation, and Bluetooth stack.
- **License**: This project is licensed under the MIT License — feel free to modify, build, and adapt it for your own embedded audio projects!
