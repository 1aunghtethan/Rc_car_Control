# RC Car — Phone Controller

A phone remote for an Arduino UNO RC car, with a rewritten firmware that gives the
phone **more useful controls** than the original sketch (live distance, servo
angle, variable speed, obstacle on/off, record/play, horn).

**Hardware:** Arduino UNO R3 · L298N motor driver · SG90 servo · HC-SR04 ultrasonic · HC-05 Bluetooth

There are two phone apps, because the radio you use decides which one works:

| App | File | Radio it needs |
|---|---|---|
| **Web app** | `app/rc-car-controller.html` | BLE module (HM-10 / AT-09 / ESP32) |
| **Android app** | `android/` | your existing **HC-05** |

Both speak the identical command protocol and use the same firmware.

```
Rc_car_Control/
├─ firmware/rc_car_app.ino          ← main Arduino sketch (v3 full-function)
├─ forCode/rc_car_sketch_fixed.ino  ← earlier sketch: adds line-buffered serial parsing
├─ app/rc-car-controller.html       ← phone app (web) — for BLE modules
├─ app/start-local.cmd              ← serves app/ on http://localhost:8000
└─ android/                         ← phone app (native, Kotlin) — for HC-05
```

### Which firmware should I flash?

Both use the **same pin map** and the same command protocol. Pick one:

| Sketch | Use it if |
|---|---|
| `firmware/rc_car_app.ino` | **Recommended.** Adds failsafe, smooth speed ramp, turn modes, adjustable obstacle distance, live telemetry, non-blocking obstacle scan. |
| `forCode/rc_car_sketch_fixed.ino` | You only want the serial-parsing fix (line-buffered `V:` / `G:` commands) on top of the simple sketch. |

---

## ⚠️ Read this first — HC-05 vs BLE

Your module is an **HC-05**. The HC-05 uses **Bluetooth Classic (SPP)**.

**A phone web page cannot talk to Bluetooth Classic.** The Web Bluetooth API
only speaks **Bluetooth Low Energy (BLE)**. This is a browser limitation, not a
bug in the app — and it is exactly why the blog you linked ships a *native
Android app* instead of a web page.

So pick the path that matches your hardware:

| Path | Radio you need | App to use | Cost / effort |
|------|----------------|------------|----------------|
| **A** | HM-10 / AT-09 / JDY-08 / ESP32 (**BLE**) | `app/rc-car-controller.html` | ~$3 module, **no code change** |
| **B** | **your existing HC-05** | `android/` app | build once in Android Studio |

Both paths use the **same Arduino firmware** and the **same command protocol**.

> If you don't want to buy anything and don't want to build an app, the fastest
> route is a **free "Bluetooth RC Car" app from the Play Store** — the firmware
> still accepts the original `F/B/L/R/S` single characters.

---

## 1. Wiring (Arduino UNO R3)

| Function | Arduino pin | Notes |
|---|---|---|
| ENA (left motor speed) | **D5** | PWM |
| ENB (right motor speed) | **D6** | PWM |
| IN1 / IN2 (left dir) | **D2 / D4** | |
| IN3 / IN4 (right dir) | **D7 / D8** | |
| Servo signal | **D3** | |
| HC-SR04 TRIG / ECHO | **D11 / D12** | |
| HC-05 / BLE TX→RX, RX→TX | **D0 / D1** | hardware serial, 9600 baud |

Optional: set `#define HORN_ENABLED 1` in the firmware if you add a buzzer on D13.

**Uploading:** unplug the HC-05 (or BLE module) from D0/D1 before flashing, then
reconnect it. Otherwise the upload fails — the radio shares the USB pins.

---

## 2. Flash the firmware

1. Open `firmware/rc_car_app.ino` in the Arduino IDE.
2. Board = **Arduino UNO**, correct COM port.
3. Upload (radio unplugged). Open Serial Monitor at **9600** → you should see `READY`.

---

## 3. Command protocol (what the app sends)

Every command is a text line ending in `\n`.

| Command | Meaning |
|---|---|
| `F` `B` `L` `R` `S` | forward / back / left / right / stop (low-latency, sent while held) |
| `V:<0-255>` | set motor speed (e.g. `V:200`) |
| `G:<0-180>` | set servo angle (e.g. `G:135`) |
| `?D` | ask distance → car replies `DIST:<cm>` |
| `?S` | ask status → `STAT:<mode>,<speed>,<angle>,<dist>` |
| `!R:1` / `!R:0` | start / stop recording a path |
| `!P:1` / `!P:0` | start / stop playing the recorded path |
| `!O:1` / `!O:0` | obstacle avoidance on / off |
| `!H:1` / `!H:0` | horn on / off (only if `HORN_ENABLED 1`) |
| `!T` | servo sweep self-test |
| `!A:<cm>` | obstacle distance threshold (e.g. `!A:20`) |
| `!M:<0\|1>` | turn mode — `0` pivot / `1` gentle arc |
| `!F:<ms>` | failsafe timeout in ms (`0` = off) |
| `!L:<0\|1>` | live telemetry push on / off |
| `!K` | clear the recorded path |

The car also sends `EVT:OBSTACLE`, `EVT:REC_START`, `EVT:PLAY_STOP`, `READY`, etc.

The old sketch used `1`–`9` for speed **and** `1`/`2`/`3` for the servo, which
collided. That's fixed here — speed is `V:` and the servo is `G:`.

---

## 4A. Use the web app (BLE module)

The app is one file: `app/rc-car-controller.html`.

- **Open it over `https://`** (or `localhost`). Web Bluetooth refuses to run on
  an insecure origin. Easiest: I can publish it for you to a shareable https
  link, or run `python -m http.server` on a PC and open `http://<pc-ip>:8000`.
- Browser: **Chrome on Android** (or **Bluefy** on iOS — Safari has no Web Bluetooth).
- Tap **Connect** → pick the BLE device → drive.
- If your module isn't found, open *Connection settings* → **List all BLE devices**.
- If the write characteristic isn't found, switch *BLE service* between
  **HM-10 (FFE0)** and **Nordic UART (ESP32)**.

Features: D-pad **or** analog joystick, speed + servo sliders, live distance bar,
record/play, obstacle toggle, horn, emergency stop, haptics, fullscreen.

### Run it locally (no internet needed)

The app is one self-contained file — no CDN, no build step, works offline.

**On the PC (easiest):**
- Double-click `app/start-local.cmd` → serves the folder and opens
  `http://localhost:8000/rc-car-controller.html`.
- Or simply double-click `app/rc-car-controller.html`.

**On the phone — read this before trying.** Web Bluetooth only runs in a
*secure context*, and that decides which addresses work:

| How you open it on the phone | Address | Web Bluetooth |
|---|---|---|
| Copy the HTML onto the phone, open in Chrome | `file:///.../rc-car-controller.html` | usually works — `file://` counts as secure |
| PC server over Wi-Fi | `http://192.168.x.x:8000/...` | **BLOCKED** — plain HTTP is not a secure context |
| PC server + Chrome USB port forwarding | `http://localhost:8000/...` | works — `localhost` is secure |
| The published share link | `https://...` | works |

The Wi-Fi address (`192.168.x.x`) is the one that *looks* right and then fails
on Connect. To drive from the phone locally, either copy the file onto the
phone, or forward the port:

1. PC: run `app/start-local.cmd`.
2. Phone: enable USB debugging, plug in the USB cable.
3. PC Chrome: `chrome://inspect` → **Port forwarding** → add `8000` → `localhost:8000` → Enable.
4. Phone Chrome: open `http://localhost:8000/rc-car-controller.html`.

## 4B. Build the Android app (your existing HC-05)

The `android/` folder is a complete Android Studio project (Kotlin).

1. Android Studio → **Open** → select the `android/` folder.
2. Let Gradle sync. It will fetch the wrapper and AGP on first run.
3. Run on your phone, or **Build → Build APK** to get an installable file.
4. **Pair** the HC-05 in Android Bluetooth settings first (PIN usually `1234` or `0000`).
   It will then show as **Saved**. **Do not tap it in settings to connect** — just pair.
5. Open the app. If your phone already has an HC-05 saved, the app finds it automatically
   and the button reads **"Connect to HC-05"** — one tap, no scanning. Otherwise tap
   **Devices** and pick it from the list of saved devices.
6. The app remembers the last device, so every launch after that is a single tap.

The Android app uses the standard SPP UUID `00001101-0000-1000-8000-00805F9B34FB`
and sends the exact same protocol as the web app.

### Command-line build

The project targets **AGP 9.4.1 / Gradle 9.6.0** and needs a **JDK 17+** (Android
Studio's bundled JBR works).

```bash
cd android
export JAVA_HOME="C:/Program Files/Android/Android Studio/jbr"
./gradlew assembleDebug
```

The APK lands in `android/app/build/outputs/apk/debug/app-debug.apk`.

**Two things that will waste your afternoon if you don't know them:**

- **`buildToolsVersion` is pinned to `36.0.0` on purpose.** Without that line AGP
  resolves a default (34.0.0), which may not be installed. The build then prints
  `BUILD SUCCESSFUL` **and produces no APK**. If `assembleDebug` "succeeds" but the
  `outputs/` folder is empty, this is why.
- **`local-maven/` is a deliberate offline mirror.** It holds four dependencies
  (three androidx jars plus `aapt2` with the **`-windows` classifier**) that Gradle
  could not fetch from `dl.google.com` on the machine this was built on, even though
  `curl` to the same URLs returned HTTP 200. `settings.gradle` lists it ahead of
  `google()`. It is only ~2.2 MB and lets the build run with `--offline`. If your
  network reaches Google's Maven normally you can delete the folder and drop the
  `maven { url = uri("${rootDir}/local-maven") }` line from `settings.gradle` — but
  there is no reason to.

### Why can't the HTML page do this?

Because it is a browser platform limit, not a missing feature. Three separate walls:

1. **Web Bluetooth speaks BLE (GATT) only.** The HC-05 is Bluetooth Classic (SPP).
2. **No API exposes the phone's saved/paired device list.** `requestDevice()` always
   opens a chooser — that is a deliberate privacy design, and no permission unlocks it.
3. **`getDevices()` only returns BLE devices the page already had permission for.**
   It never sees your bonded list.

So an HTML file can never connect to that saved HC-05, on any phone, with any settings.
Native Android can — which is exactly why `android/` exists.

---

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Phone says **"Couldn't connect"** when you tap HC-05 in Bluetooth settings | **This is expected — ignore it.** HC-05 is SPP. Pair it, then never touch it in settings again. Connect from an app. |
| HC-05 shows as "Saved" but no app can reach it | Turn off other BT devices (earbuds, watch). Only one SPP client at a time. |
| App connects but the car does nothing | **TX/RX are swapped.** Module TXD must go to Arduino **D0**, RXD to **D1**. |
| App connects, car still dead | **Baud mismatch.** The HC-05 must be set to **9600** to match `Serial.begin(9600)`. |
| Arduino IDE upload fails | Unplug HC-05/BLE from D0/D1, upload, replug |
| `Connect` button does nothing (web) | Not https, or browser has no Web Bluetooth → use Chrome/Bluefy |
| BLE device not in the list | Settings → *List all BLE devices* |
| Connects but car won't move | Check L298N power (motor supply separate from Arduino 5V) |
| Car jitters or stops by itself | Failsafe fired. Keep the app polling, or raise `!F:<ms>` / disable with `!F:0` |
| Distance always `--` | Check HC-SR04 on D11/D12 and its 5V/GND |

---

## 6. Why this is "more useful" than the original

- **Live telemetry** — distance readout with a proximity bar, plus mode/speed/servo state.
- **Real speed control** — a 0–255 slider (or joystick distance) instead of 9 steps.
- **Direct servo control** — full 0–180° slider plus quick presets.
- **Analog joystick** — one-thumb driving with speed proportional to drag distance.
- **Safety** — emergency stop, and obstacle avoidance you can toggle live.
- **Path memory** — record a route and replay it from the phone.
- **Non-blocking obstacle scan** — the original blocked the whole loop during a scan;
  this version only scans while driving forward, so the car stays responsive.

### Added in the full-function sketch (`firmware/rc_car_app.ino`, v3)

Pins are **unchanged** — nothing new to wire.

- **Failsafe** — if the phone disconnects, dies or goes out of range while the car is
  moving, the car stops by itself after 2 s (configurable, `!F:0` disables). No more
  runaway car when Bluetooth drops.
- **Smooth speed ramp** — PWM eases up and down over ~70 ms instead of jumping, which is
  much kinder to the gearbox and the battery.
- **Turn modes** — *pivot* (spin in place, the original behaviour) or *gentle arc*
  (inner wheel stops, so the car curves forward instead of spinning).
- **Adjustable obstacle distance** — set the auto-brake threshold from the app (`!A:<cm>`)
  instead of re-flashing to change `15`.
- **Live telemetry** — the car can push distance readings on its own (`!L:1`).
- **Clear path** — erase a recording without recording over it (`!K`).
- **Legacy compatibility** — a bare `1`–`9` line still sets speed, and bare `F/B/L/R/S`
  single characters still work, so old Bluetooth apps keep functioning.
