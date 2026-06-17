# Focus Pager — iOS Companion (Phase 2)

SwiftUI app that acts as the **BLE central** for the ESP32 pager. It maintains
the connection, drives the brick/unbrick flows, and runs the anti-spoof auth
handshake. App blocking (FamilyControls / ManagedSettings) is stubbed for
Phase 3.

- **Bundle ID:** `com.focuspager.app`
- **Min iOS:** 16.0
- **Frameworks:** CoreBluetooth + CryptoKit (system only, no dependencies)

## Open & run

```bash
open ios-companion/FocusPager.xcodeproj
```

Select a physical iPhone (BLE doesn't work in the Simulator), set your signing
team, and run. The app scans on launch, connects to the pager, and shows the
current `BrickState`.

> The project uses Xcode's file-system-synchronized groups — every file under
> `FocusPager/` is automatically part of the target, so just add/remove files in
> that folder; no `.pbxproj` edits needed.

## Architecture

| File | Role |
|------|------|
| `BrickProtocol.swift` | UUIDs, enums, and the all-zeros dev PSK. Mirror of `/protocol/brick-control.md`. |
| `PagerConnection.swift` | `ObservableObject` BLE central. Scans, connects, subscribes to `BrickState`/`UnbrickEvent`, auto-reconnects, supports state restoration. Exposes `forceBrick()` and `triggerUnbrickFlow()`. |
| `AuthService.swift` | Stateless challenge-response: random 4-byte challenge → write `Auth` → read response → compare against `HMAC-SHA256(PSK, challenge)[0..3]`. |
| `ShieldManager.swift` | Phase 3 stub — `isShieldActive` + `activate/deactivateShield()`. |
| `ContentView.swift` | Single screen: connection status, BRICKED/UNBRICKED indicator, Force Brick / Unbrick Now buttons, recent-events log. |

### Unbrick flow

1. Pager button hold → `UnbrickEvent` notify.
2. App generates a 4-byte challenge, writes it to `Auth`, waits for the write
   ack, then reads the 4-byte HMAC response back.
3. App computes the expected HMAC locally and compares (constant-time).
4. **Match** → clear shield, send `Command = 0x00` (sync) to confirm, set
   `UNBRICKED`. **Mismatch** → ignore, stay bricked (never fail open).

### Note on `AuthService` API

The build prompt sketched `verifyAndUnbrick(peripheral:authChar:)`. The shipped
API is `verifyHandshake(authChar:io:)`, where `io` is a `PeripheralIO` that
`PagerConnection` implements. Routing the write/read through the single
peripheral delegate (instead of temporarily swapping `peripheral.delegate`
inside `AuthService`) avoids dropping `BrickState`/`UnbrickEvent` notifications
that can arrive mid-handshake. The crypto and sequencing are otherwise exactly
as specified.

## Background BLE

`UIBackgroundModes = [bluetooth-central]` and the central is created with
`CBCentralManagerOptionRestoreIdentifierKey = "com.focuspager.central"`.
`willRestoreState` re-adopts the peripheral after relaunch.
