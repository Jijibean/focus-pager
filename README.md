# Focus Pager

A cheap BLE companion device that keeps you reachable for calls and important
messages while your iPhone is bricked, with the physical device doubling as the
only way to un-brick.

**Competes with:** Brick (adds reachability) — not the Apple Watch (which adds
enforced focus at ~3× the price).

---

## Hardware (v1 dev bring-up)

| Part | Purpose |
|------|---------|
| ESP32-WROOM-32 dev board | SoC — dual BLE + Classic BT |
| MAX98357A I2S amp | Speaker output for HFP calls |
| INMP441 I2S MEMS mic | Microphone for HFP + Siri |
| SSD1306 128×64 OLED (I2C) | Status display |
| Tactile button (GPIO0 or any) | Physical un-brick trigger |
| TP4056 + LiPo | Battery & charging |

**Why ESP32?** It is the only affordable SoC that runs ANCS over BLE *and* HFP
over Classic Bluetooth simultaneously. nRF52 is BLE-only and cannot do HFP call
audio.

---

## Repo layout

```
focus-pager/
├── README.md                         ← you are here
├── protocol/brick-control.md         ← shared BLE protocol contract
├── firmware/                         ← ESP-IDF project (C)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── main.c
│       ├── ancs_client.{c,h}         ← Phase 1: ANCS read path
│       ├── brick_service.{c,h}       ← Phase 2 (TODO)
│       ├── hfp_audio.{c,h}           ← Phase 4 (TODO)
│       ├── pager_state.{c,h}         ← Phase 3 (TODO)
│       └── ui.{c,h}                  ← Phase 1+ (TODO)
└── ios-companion/                    ← Swift/SwiftUI app (TODO Phase 2+)
```

---

## Build phases

| Phase | What | Acceptance test |
|-------|------|-----------------|
| **0** | Toolchain setup | LED blinks, blank SwiftUI app on device |
| **1** | ANCS read path *(current)* | Text notification appears over UART |
| **2** | Brick Control GATT service | Button hold → app logs verified UnbrickEvent |
| **3** | FamilyControls shield | Instagram blocked/unblocked by button hold |
| **4** | HFP calls + Siri voice | Real call through pager mic/speaker |
| **5** | UX + hardening | Schedules, reconnect, low-power, NVS PSK |

---

## Phase 0 — Environment setup

### Prerequisites

- macOS with Xcode CLT installed
- Python 3.8+
- USB cable for ESP32

### Install ESP-IDF v5.x

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.2.1          # or latest stable v5.x
./install.sh esp32
source export.sh             # add to ~/.zshrc for convenience
```

### Build & flash Phase 1 firmware

```bash
cd focus-pager/firmware
idf.py set-target esp32      # reads sdkconfig.defaults
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

> **Tip:** Replace `/dev/cu.usbserial-*` with your actual port.
> Run `ls /dev/cu.*` to find it after plugging in the ESP32.
> Press Ctrl+] to exit the monitor.

---

## Phase 1 — ANCS read path

### How it works

1. ESP32 advertises as `FocusPager` with the ANCS service UUID in the BLE
   solicitation field.
2. iPhone sees the solicitation, connects, and after bonding exposes its ANCS
   GATT service.
3. ESP32 subscribes to **Notification Source** (notify) and **Data Source** (notify).
4. On each incoming notification, ESP32 writes a `GetNotificationAttributes`
   command to the **Control Point** requesting Title, Subtitle, Message, and App ID.
5. Response streams in over Data Source; ESP32 reassembles packets and logs the
   result over UART.

### Test procedure (iPhone)

1. Flash firmware and open the serial monitor (`idf.py -p ... monitor`).
2. On iPhone: **Settings → Bluetooth → pair with "FocusPager"**.
   - Accept the pairing request on the iPhone. No passkey needed (Just Works).
3. Wait for `ANCS fully subscribed — waiting for notifications` in UART output.
4. Send yourself an iMessage from another device, or trigger any notification
   (email, WhatsApp, etc.).
5. Within 1–2 seconds you should see:

```
I (1234) ANCS: --- Notification ---
I (1234) ANCS:   UID     : 42
I (1234) ANCS:   Category: Social
I (1234) ANCS:   App     : com.apple.MobileSMS
I (1234) ANCS:   Title   : Alice
I (1234) ANCS:   Message : hey, you free tonight?
I (1234) ANCS: --------------------
```

6. Test an incoming call — you'll see `Category: IncomingCall`.

### Troubleshooting

| Symptom | Fix |
|---------|-----|
| `ANCS service not found` after bonding | Re-pair: forget device on iPhone, erase flash (`idf.py erase-flash`), re-pair |
| No notifications after `subscribed` | Confirm iPhone Notifications are enabled for the app you're testing |
| `Auth failed` | Usually means bonding info is stale — erase NVS partition and re-pair |
| Connection drops immediately | Check `sdkconfig` has `BT_BLE_SMP_ENABLE=y` and rebuild |

---

## Key constraints (read before modifying)

1. **SoC = ESP32 only.** nRF52 is BLE-only and cannot do HFP call audio.
2. **No outbound SMS/iMessage from iOS.** Siri voice via `AT+BVRA=1` is the
   ceiling. Don't write code that tries to inject messages.
3. **ANCS is read-only.** Subscribe; don't build a custom notification pipe.
4. **Un-brick is physically gated.** Software-only unbrick defeats the product.
5. **v1 = same-room BT only.** No SIM. Prove the loop first.

See `protocol/brick-control.md` for the full BLE contract.

---

## License

MIT
