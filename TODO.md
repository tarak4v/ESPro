# ESPro SmartWatch — Actionable Roadmap

> **Last updated:** 2026-03-12  
> **Overall completion:** ~85%  
> **Legend:** ⬜ Not started · 🟡 Needs discussion · ✅ Done  

---

## 0. EXPAND SPIFFS PARTITION (Top Priority — Next Item)

### 0.1 Reclaim Unused Flash for Storage
- **File:** `partitions.csv`
- **Issue:** Board has **16 MB flash** but only ~8.5 MB is used (8M app + 512K SPIFFS + 28K NVS/PHY). **~7 MB of flash is wasted.**
- **Board note:** No SD card slot — SPIFFS is the only file storage available.
- **Fix:** Expand SPIFFS partition from 512K → 7M:
  ```csv
  nvs,       data, nvs,     ,       0x6000,
  phy_init,  data, phy,     ,       0x1000,
  factory,   app,  factory, ,       8M,
  storage,   data, spiffs,  ,       7M,
  ```
- **Impact:** 14× more storage (512K → 7 MB) — unlocks offline music cache, game assets, voice memos, enhanced logging, and custom watch faces.
- **Risk:** Requires full flash erase and re-flash (`idf.py erase-flash flash`). NVS settings will be lost.
- **Effort:** Tiny (1-line change + re-flash)
- **Status:** ✅ Done — partition expanded to 7M with `littlefs` subtype

### 0.2 Consider LittleFS Instead of SPIFFS
- **Issue:** SPIFFS has no directory support, poor wear leveling, and degrades on large partitions. LittleFS is better suited for 7 MB.
- **Fix:** Switch to `esp_littlefs` component — drop-in replacement with directory support, better performance, and proper wear leveling.
- **Effort:** Small (add component, change mount calls in `sd_log.c`)
- **Status:** ✅ Done — migrated to `joltwallet/littlefs` v1.20.4 (`sd_log.c`, `CMakeLists.txt`, `idf_component.yml`)

### 0.3 Enable PSRAM for WiFi/LWIP Buffers
- **File:** `sdkconfig`
- **Issue:** `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is disabled. WiFi/TLS buffers consume ~50 KB of precious internal RAM.
- **Fix:** Set `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` to move WiFi buffers to the 8 MB Octal PSRAM.
- **Effort:** Tiny (menuconfig change)
- **Status:** ✅ Done — enabled in `sdkconfig.defaults` and `sdkconfig`

---

## 1. FINISH INCOMPLETE FEATURES (Quick Wins)

These items have partial implementations — just need wiring or small fixes.

### 1.1 Brightness Persistence
- **File:** `main/screens/screen_settings.c`
- **Issue:** Brightness slider works in real-time (`setUpduty()`) but value is NOT saved to NVS. Resets on reboot.
- **Fix:** Add `nvs_set_u8()` in the brightness slider callback (same pattern as volume/theme). Load on boot with `nvs_get_u8()`.
- **Effort:** Small (10 lines)
- **Status:** ⬜

### 1.2 Error Logging Integration
- **File:** `main/services/sd_log.c` + all service files
- **Issue:** `sd_log_write()` exists but is never called from outside `sd_log.c`. WiFi failures, music stream errors, assistant API failures go unlogged.
- **Fix:** Add `sd_log_write()` calls in key error paths: `wifi_time.c` (connection failure), `music_player.c` (stream error), `screen_assistant.c` (API error), `weather.c` (HTTP failure).
- **Bonus:** Add log rotation (max 64KB, truncate oldest entries).
- **Effort:** Medium
- **Status:** ⬜

### 1.3 Log Viewer in Settings
- **File:** `main/screens/screen_settings.c`
- **Issue:** Logs written to SPIFFS `/log/exceptions.txt` but only readable via serial console.
- **Fix:** Add a "View Logs" option in settings that reads and displays last 10 log entries in a scrollable LVGL label.
- **Effort:** Medium
- **Status:** ⬜

---

## 2. COMPASS / IMU ENHANCEMENTS

### 2.1 Compass Screen — Visual Compass (No Magnetometer)
- **File:** `main/screens/screen_compass.c`
- **Issue:** Currently shows raw accel/gyro numbers + artificial horizon line. No magnetometer means no true heading.
- **Options to discuss:**
  - (a) Keep as "IMU Sensor" screen — show tilt angles visually with a 3D cube or virtual level
  - (b) Add a digital spirit level (bubble level) using pitch/roll — useful and fun
  - (c) Remove compass name, rename to "Sensors" or "Motion"
- **Status:** 🟡

### 2.2 IMU Accelerometer Calibration
- **File:** New utility `main/core/imu_cal.c` or integrate into `game_maze.c`
- **Issue:** Accelerometer has bias offset — ball drifts when device is flat on table.
- **Fix:**
  - Sample ~50 readings with device flat (detect flat: `az ≈ 1.0g`, `ax/ay ≈ 0.0g`)
  - Store `ax_avg`, `ay_avg` as bias offsets, subtract from every reading
  - Persist offsets to NVS (survive reboot)
  - Add "Calibrate IMU" button in settings or prompt at game start
- **Effort:** Small-Medium
- **Status:** ⬜

### 2.3 IMU Gesture Recognition
- **File:** New utility or extend `main/core/main.c`
- **Possible gestures using QMI8658 accel/gyro:**
  - **Shake** → shuffle music station / dismiss notification
  - **Tilt-to-scroll** → scroll long text (assistant response, logs)
  - **Double-tap** → wake from sleep / toggle torch
  - **Wrist-raise** → turn on display (smartwatch classic)
- **Status:** 🟡

---

## 3. POWER MANAGEMENT

### 3.1 Sleep Mode
- **Issue:** Device never sleeps — AMOLED always on, WiFi always connected.
- **Plan:**
  - Light sleep after 30s inactivity (configurable in settings)
  - Deep sleep after 5min (wake on button/touch/IMU tap)
  - AMOLED display off during sleep (AXS15231B sleep command)
  - WiFi disconnect during deep sleep, reconnect on wake
- **Effort:** Large
- **Status:** ⬜

### 3.2 Display Auto-Brightness
- **Issue:** Brightness is manual slider only.
- **Idea:** Use ambient light estimation (no dedicated sensor, but could use mic noise floor or just a time-based schedule).
- **Status:** 🟡

---

## 4. UI / UX POLISH

### 4.1 Screen Transition Animations
- **Issue:** Screen switches are instant (destroy old → create new). No visual feedback.
- **Fix:** Add slide animation (LVGL `lv_scr_load_anim()`) for swipe transitions between clock ↔ menu.
- **Effort:** Small-Medium
- **Status:** ✅ Done — 300ms slide-left/right animations in `app_manager.c`, deferred old-screen destroy via LVGL timer

### 4.2 Flip Clock — Full Animation
- **File:** `main/screens/screen_clock.c`
- **Issue:** Flip clock toggle exists in settings and digit frames are defined, but the actual flip animation (top-half / bottom-half split with rotation) may need polish.
- **Fix:** Verify the flip animation renders properly. Test switching between digital/flip modes.
- **Status:** ✅ Done — opacity-flash animation on digit change via `lv_anim_t`, tracks previous digits

### 4.3 Light Theme Rendering
- **File:** Multiple screen files
- **Issue:** Theme toggle (dark/light) saves to NVS, but all screen UIs are hard-coded with dark colors. Light theme may look broken (white text on white background).
- **Fix:** Audit each screen's `_create()` function — use `g_theme_dark` to pick colors. Define a color palette struct.
- **Effort:** Medium-Large
- **Status:** ✅ Done — `screen_compass.c` fully themed, `screen_menu.c` title uses `th_text`, `screen_tamafi.c` bg/back-button uses theme vars

### 4.4 Long Text Scrolling
- **Issue:** AI assistant responses can be long. The response label on the 640×172 screen may truncate.
- **Fix:** Auto-scroll long responses, or add swipe-up/down within the response panel.
- **Effort:** Small
- **Status:** ⬜

### 4.5 Touch Feedback
- **Issue:** No visual or audio feedback on button press.
- **Options:**
  - Brief color flash on tap (LVGL pressed style)
  - Short click sound via speaker (10ms 2kHz pulse)
- **Status:** 🟡

---

## 5. VOICE ASSISTANT IMPROVEMENTS

### 5.1 Conversation History
- **File:** `main/screens/screen_assistant.c`
- **Issue:** Each interaction is stateless — no memory of previous questions.
- **Fix:** Maintain a rolling context buffer (last 3-5 exchanges) sent as chat history to Groq LLM.
- **Effort:** Medium
- **Status:** ⬜

### 5.2 Wake Word Detection
- **Issue:** Voice assistant requires manual tap to start listening.
- **Idea:** Lightweight keyword detection ("Hey ESPro") running on mic input task. Could use simple energy + pattern matching (not full ML).
- **Effort:** Large
- **Status:** 🟡

### 5.3 Streaming TTS
- **Issue:** Current TTS downloads full WAV before playing. Noticeable delay for long responses.
- **Idea:** Stream-and-play: start playback as soon as first audio chunk arrives (similar to music player ring buffer approach).
- **Effort:** Large
- **Status:** ✅ Done — rewrote TTS to stream WAV from HTTP directly to I2S (1KB header + 4KB streaming buffer vs 512KB)

### 5.4 Error Recovery & Retry
- **File:** `main/screens/screen_assistant.c`
- **Issue:** If Groq API times out or returns error, user gets stuck.
- **Fix:** Add timeout handling, retry once, then show error message and return to IDLE state.
- **Effort:** Small
- **Status:** ✅ Done — LLM & TTS retry once on failure, processing timeout bumped to 60s, errors return to IDLE

---

## 6. MUSIC PLAYER ENHANCEMENTS

### 6.0 JioSaavn Curated Streams (Priority)
- **File:** `main/audio/music_player.c`
- **Status:** ✅ — **Implemented**
- **What was done:**
  - Removed Radio Browser API + all 8 hardcoded fallback station arrays
  - Added JioSaavn search API integration with DES-ECB URL decryption
  - 5 curated categories: **BW Latest**, **BW 90s**, **Bhajans**, **Dance Mix**, **Lofi**
  - Songs fetched via `search.getResults`, URLs decrypted (mbedtls DES + Base64), quality set to 160kbps MP3
  - Song-by-song playback with auto-advance and playlist looping
  - Shuffle on load for variety
  - UI updated: "Song X/Y", "Playing" status, song title + artist display
- **To customize:** Edit the `genres[]` search queries at the top of `music_player.c`
- **To test:** Flash and verify songs play from each category

### 6.1 Now-Playing Info Display
- **Issue:** Station name shown but no artist/title metadata from ICY stream headers.
- **Fix:** Parse `icy-metaint` header in HTTP stream, extract `StreamTitle` metadata, display on screen.
- **Effort:** Medium
- **Status:** ⬜

### 6.2 Favorites / Bookmarks
- **Issue:** No way to save favorite stations.
- **Fix:** Let user long-press or tap a star to save current station URL to NVS. Add "Favorites" genre option.
- **Effort:** Medium
- **Status:** ⬜

### 6.3 Audio Visualizer
- **Issue:** Music player screen is static during playback.
- **Fix:** Show real-time waveform or bar spectrum using decoded PCM amplitude data (sample the PCM buffer in decode task).
- **Effort:** Medium
- **Status:** ⬜

### 6.4 Equalizer / Bass Boost
- **Issue:** Flat audio output.
- **Idea:** Simple 3-band EQ (bass/mid/treble) via digital filter on PCM samples before I2S write.
- **Effort:** Medium
- **Status:** 🟡

---

## 7. VIRTUAL PET (TAMAFI) ENHANCEMENTS

### 7.1 Smoother Animations
- **File:** `main/screens/screen_tamafi.c`
- **Issue:** Animations are frame-based (swap images). Could be smoother with interpolation.
- **Fix:** Add easing/tweening for eye blinks, body bounce, mood transitions.
- **Effort:** Medium
- **Status:** ⬜

### 7.2 Personality / Learning
- **Issue:** Pet behavior is random — no long-term memory of user interaction patterns.
- **Idea:** Track interaction frequency, preferred gestures, time of day patterns. Pet "learns" user habits and adjusts mood accordingly.
- **Effort:** Large
- **Status:** 🟡

### 7.3 Mini-Games with Pet
- **Idea:** Simple tap/shake mini-games (feed the pet, play fetch) that affect mood/happiness.
- **Effort:** Medium
- **Status:** 🟡

---

## 8. MACROPAD ENHANCEMENTS

### 8.1 Custom Key Mapping
- **File:** `main/services/macropad.c`
- **Issue:** Key actions are hard-coded for Google Meet / Teams only.
- **Fix:** Add option to define custom key mappings (stored in NVS) — user selectable generic shortcuts.
- **Effort:** Medium
- **Status:** ⬜

### 8.2 Zoom Profile
- **Issue:** Only Google Meet and Teams profiles exist.
- **Fix:** Add Zoom meeting controls (Alt+A = mute, Alt+V = video, Alt+Q = end).
- **Effort:** Small
- **Status:** ⬜

### 8.3 PC Listener App (Python)
- **File:** `tools/pc_monitor.py`
- **Issue:** WiFi UDP mode needs a PC-side listener. It exists but may not be tested.
- **Fix:** Verify/update `tools/pc_monitor.py`, add setup instructions to README.
- **Effort:** Small
- **Status:** ⬜

### 8.4 Background Key-Send (Jabra-style)
- **File:** `main/services/macropad.c`, `main/core/app_manager.c`
- **Issue:** Macro keys only work while the macropad overlay is open. BLE connection tears down when leaving the screen. Jabra headsets work from any screen because hardware buttons are always available.
- **Fix:**
  - Keep BLE HID connection alive in background (don't disconnect on overlay close)
  - Add boot button gestures routed through `app_manager.c`:
    - **Double-press** → mic toggle (send BLE HID keypress from any screen)
    - **Long-press (2s)** → end meeting
  - Keep `key_send_task` + `s_key_queue` running even when overlay is closed
  - Optional: tilt gesture (quick wrist flip) → video toggle
- **Effort:** Medium
- **Status:** ⬜

### 8.5 Live App Status Sync (Jabra-style)
- **File:** `main/services/macropad.c`, `main/screens/screen_clock.c`
- **Issue:** Mic/video status indicators only visible inside macropad overlay. No way to see meeting state from clock or other screens.
- **Fix:**
  - **WiFi UDP status receiver always-on:** Run `state_rx_task` (port 13580) at all times, not just when overlay is open. PC listener sends `"mic:0"`, `"video:1"` etc.
  - **BLE+WiFi hybrid mode:** Use BLE for key-send (no PC software needed) + WiFi UDP for status feedback from PC listener
  - **Persistent status on clock face:** Show small mic/video icons on clock screen using global `g_mic_on` / `g_video_on` flags updated by `state_rx_task`
  - **Note:** Standard BLE HID is one-way (watch→PC). True bidirectional BLE would need a custom GATT characteristic + companion app on PC.
- **Effort:** Medium-Large
- **Status:** ⬜

---

## 9. STORAGE-ENABLED FEATURES (Unlocked by Section 0)

### 9.0 Offline Music Cache
- **File:** `main/audio/music_player.c`
- **Prereq:** Section 0.1 (expanded SPIFFS/LittleFS)
- **Idea:** Cache recently streamed JioSaavn songs to flash. 5-7 MB can hold 1-2 compressed songs for offline playback when WiFi is unavailable.
- **Approach:** Save decoded audio chunks to files, detect WiFi-down and play from cache.
- **Effort:** Medium-Large
- **Status:** ⬜

### 9.01 Custom Watch Faces / Themes
- **Prereq:** Section 0.1
- **Idea:** Store multiple bitmap/image assets for user-selectable watch face skins. LVGL image assets (PNG/BIN) loaded from the expanded partition.
- **Effort:** Medium
- **Status:** ✅ Done — 5 face skins (Default, Ember, Forest, Royal, Minimal) as JSON on LittleFS, selector UI in settings, clock screen uses `g_face` colors

### 9.02 Voice Memos
- **Prereq:** Section 0.1
- **Idea:** Record short voice memos using the ES7210 ADC mic input (GPIO 6). Store as raw PCM or WAV files in flash. Playback through speaker.
- **Effort:** Medium
- **Status:** ⬜

### 9.03 Game Assets & Level Packs
- **Prereq:** Section 0.1
- **Idea:** Store maze level definitions, sprites, sound effects as files. Save game state, high scores, user progress. Add more games with pre-built level packs.
- **Effort:** Medium
- **Status:** ⬜

### 9.04 Offline Weather / Data Cache
- **Prereq:** Section 0.1
- **Idea:** Cache weather data, calendar events, reminders, and notes for offline viewing when WiFi is unavailable.
- **Effort:** Small
- **Status:** ⬜

### 9.05 Alarm / Notification Sounds
- **Prereq:** Section 0.1
- **Idea:** Store custom alarm tones, notification sounds, and boot sounds as audio files in flash instead of hardcoded arrays.
- **Effort:** Small
- **Status:** ⬜

---

## 10. NEW FEATURE IDEAS

### 10.1 Notifications (BLE)
- **Issue:** No phone notification mirroring.
- **Idea:** BLE ANCS (Apple) or BLE notification forwarding from Android companion app. Show caller name, message preview on watch.
- **Effort:** Very Large
- **Status:** 🟡

### 10.2 Stopwatch / Timer
- **Issue:** Clock screen has time only. No stopwatch or countdown timer.
- **Fix:** Add as sub-mode of clock screen or new app in menu.
- **Effort:** Medium
- **Status:** ⬜

### 10.3 Pomodoro Timer
- **Idea:** Work/break timer (25/5 min) with sound alert. Useful for productivity.
- **Effort:** Small-Medium
- **Status:** ⬜

### 10.4 Step Counter / Pedometer
- **Issue:** IMU available but not used for health tracking.
- **Fix:** Simple step counter using accelerometer peak detection. Display daily count on clock face.
- **Effort:** Medium
- **Status:** ⬜

### 10.5 Alarm Clock
- **Issue:** No alarm functionality despite having RTC + speaker.
- **Fix:** Set alarm via settings UI (hour:min), trigger melody when RTC matches. Could also wake from deep sleep.
- **Effort:** Medium
- **Status:** ⬜

### 10.6 OTA Firmware Update
- **Issue:** Current partition table has no OTA partition. Needs restructuring if OTA staging area is desired.
- **Fix:** Add HTTP OTA update screen — download firmware from GitHub releases via WiFi. Add `ota_0` + `ota_1` partitions.
- **Effort:** Medium-Large
- **Status:** ⬜

### 10.7 QR Code Display
- **Idea:** Generate QR codes on screen (WiFi sharing, URL, contact info). Useful for quick setup sharing.
- **Effort:** Small (many QR libraries for ESP32)
- **Status:** 🟡

---

## 11. CODE QUALITY & MAINTENANCE

### 11.1 Screen Menu Size
- **File:** `main/screens/screen_menu.c`
- **Issue:** Very large file (~2000+ LOC) handling 7 app overlays.
- **Fix:** Extract overlay UIs into separate files (music_overlay.c, macropad_overlay.c, etc.).
- **Effort:** Medium
- **Status:** 🟡

### 11.2 Magic Numbers
- **Issue:** Constants like `ACCEL_SCALE = 3.5`, `BALL_R = 6`, buffer sizes scattered throughout code without explanation.
- **Fix:** Document non-obvious constants with brief comments where needed.
- **Effort:** Small
- **Status:** ⬜

### 11.3 README Update
- **File:** `README.md`
- **Issue:** Likely outdated given all recent feature additions.
- **Fix:** Update with current feature list, build instructions, hardware setup, screenshots.
- **Effort:** Medium
- **Status:** ⬜

---

## PRIORITY GUIDE

| Priority | Items | Why |
|----------|-------|-----|
| **P0 — Do now** | **0.1 (expand SPIFFS to 7M)**, 0.3 (PSRAM WiFi buffers), 1.1 (brightness NVS) | Unlock storage features, free internal RAM, quick fix |
| **P1 — Next sprint** | 0.2 (LittleFS eval), 1.2 (error logging), 5.4 (assistant error recovery), 4.4 (text scroll), 4.1 (transitions) | Quality + storage foundation |
| **P2 — Build on storage** | 9.0 (offline music cache), 9.01 (watch faces), 9.02 (voice memos), 9.04 (offline data), 9.05 (custom sounds) | Storage-enabled features |
| **P3 — Feature expansion** | 2.2 (IMU cal), 2.3 (gestures), 3.1 (sleep mode), 6.2 (favorites), 7.1 (animations), 8.1-8.3 (macropad), 10.2-10.5 (timer/steps/alarm) | Feature expansion |
| **P4 — Someday** | 5.2 (wake word), 5.3 (streaming TTS), 10.1 (notifications), 10.6 (OTA), 7.2 (pet learning) | Complex, research-heavy |

---

## HOW TO USE THIS FILE

1. **Discuss** — Review items marked 🟡 and decide go/no-go
2. **Pick** — Choose items by priority (P0 first)
3. **Elaborate** — Before coding, we'll detail the approach for each item
4. **Code & Test** — Implement, flash, verify on device
5. **Mark done** — Change ⬜ to ✅ when completed
