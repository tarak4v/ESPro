# ESPro SmartWatch — Actionable Roadmap

> **Last updated:** Based on commit d4a8a0f  
> **Overall completion:** ~85%  
> **Legend:** ⬜ Not started · 🟡 Needs discussion · ✅ Done  

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

### 2.2 IMU Gesture Recognition
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
- **Status:** ⬜

### 4.2 Flip Clock — Full Animation
- **File:** `main/screens/screen_clock.c`
- **Issue:** Flip clock toggle exists in settings and digit frames are defined, but the actual flip animation (top-half / bottom-half split with rotation) may need polish.
- **Fix:** Verify the flip animation renders properly. Test switching between digital/flip modes.
- **Status:** 🟡

### 4.3 Light Theme Rendering
- **File:** Multiple screen files
- **Issue:** Theme toggle (dark/light) saves to NVS, but all screen UIs are hard-coded with dark colors. Light theme may look broken (white text on white background).
- **Fix:** Audit each screen's `_create()` function — use `g_theme_dark` to pick colors. Define a color palette struct.
- **Effort:** Medium-Large
- **Status:** ⬜

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
- **Status:** ⬜

### 5.4 Error Recovery & Retry
- **File:** `main/screens/screen_assistant.c`
- **Issue:** If Groq API times out or returns error, user gets stuck.
- **Fix:** Add timeout handling, retry once, then show error message and return to IDLE state.
- **Effort:** Small
- **Status:** ⬜

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

---

## 9. NEW FEATURE IDEAS

### 9.1 Notifications (BLE)
- **Issue:** No phone notification mirroring.
- **Idea:** BLE ANCS (Apple) or BLE notification forwarding from Android companion app. Show caller name, message preview on watch.
- **Effort:** Very Large
- **Status:** 🟡

### 9.2 Stopwatch / Timer
- **Issue:** Clock screen has time only. No stopwatch or countdown timer.
- **Fix:** Add as sub-mode of clock screen or new app in menu.
- **Effort:** Medium
- **Status:** ⬜

### 9.3 Pomodoro Timer
- **Idea:** Work/break timer (25/5 min) with sound alert. Useful for productivity.
- **Effort:** Small-Medium
- **Status:** ⬜

### 9.4 Step Counter / Pedometer
- **Issue:** IMU available but not used for health tracking.
- **Fix:** Simple step counter using accelerometer peak detection. Display daily count on clock face.
- **Effort:** Medium
- **Status:** ⬜

### 9.5 Alarm Clock
- **Issue:** No alarm functionality despite having RTC + speaker.
- **Fix:** Set alarm via settings UI (hour:min), trigger melody when RTC matches. Could also wake from deep sleep.
- **Effort:** Medium
- **Status:** ⬜

### 9.6 OTA Firmware Update
- **Issue:** Partition table is OTA-ready but no OTA implementation.
- **Fix:** Add HTTP OTA update screen — download firmware from GitHub releases via WiFi.
- **Effort:** Medium-Large
- **Status:** ⬜

### 9.7 QR Code Display
- **Idea:** Generate QR codes on screen (WiFi sharing, URL, contact info). Useful for quick setup sharing.
- **Effort:** Small (many QR libraries for ESP32)
- **Status:** 🟡

---

## 10. CODE QUALITY & MAINTENANCE

### 10.1 Screen Menu Size
- **File:** `main/screens/screen_menu.c`
- **Issue:** Very large file (~2000+ LOC) handling 7 app overlays.
- **Fix:** Extract overlay UIs into separate files (music_overlay.c, macropad_overlay.c, etc.).
- **Effort:** Medium
- **Status:** 🟡

### 10.2 Magic Numbers
- **Issue:** Constants like `ACCEL_SCALE = 3.5`, `BALL_R = 6`, buffer sizes scattered throughout code without explanation.
- **Fix:** Document non-obvious constants with brief comments where needed.
- **Effort:** Small
- **Status:** ⬜

### 10.3 README Update
- **File:** `README.md`
- **Issue:** Likely outdated given all recent feature additions.
- **Fix:** Update with current feature list, build instructions, hardware setup, screenshots.
- **Effort:** Medium
- **Status:** ⬜

---

## PRIORITY GUIDE

| Priority | Items | Why |
|----------|-------|-----|
| **P0 — Do now** | 1.1 (brightness NVS), 5.4 (assistant error recovery), 4.4 (text scroll) | Quick fixes, immediate UX improvement |
| **P1 — Next sprint** | 6.0 (JioSaavn streams), 1.2 (error logging), 4.1 (transitions), 4.3 (light theme), 5.1 (conversation history), 6.1 (now-playing) | Curated content + quality improvements |
| **P2 — Nice to have** | 2.2 (gestures), 3.1 (sleep mode), 6.2 (favorites), 7.1 (animations), 8.1-8.3 (macropad), 9.2-9.5 (timer/steps/alarm) | Feature expansion |
| **P3 — Someday** | 5.2 (wake word), 5.3 (streaming TTS), 9.1 (notifications), 9.6 (OTA), 7.2 (pet learning) | Complex, research-heavy |

---

## HOW TO USE THIS FILE

1. **Discuss** — Review items marked 🟡 and decide go/no-go
2. **Pick** — Choose items by priority (P0 first)
3. **Elaborate** — Before coding, we'll detail the approach for each item
4. **Code & Test** — Implement, flash, verify on device
5. **Mark done** — Change ⬜ to ✅ when completed
