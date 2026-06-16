# Brick Control Protocol

**Revision:** v1 (Phase 1–3 scope)
**Status:** Canonical — keep firmware and iOS app in sync with this file.

---

## Service

Base UUID: `B41C00xx-9E5A-4C7B-9D2F-0A1B2C3D4E5F`

Transported over the same BLE link as ANCS (one physical connection from iPhone
to ESP32). ANCS is handled by iOS at the system level; Brick Control is handled
by the companion app via CoreBluetooth.

---

## Characteristics

| Name           | UUID (`xx`) | Properties       | Length  | Meaning |
|----------------|-------------|------------------|---------|---------|
| `BrickState`   | `0002`      | Read, Notify     | 1 byte  | `0x00` = unbricked, `0x01` = bricked |
| `UnbrickEvent` | `0003`      | Notify           | 1 byte  | Monotonic counter, incremented on each qualifying button hold |
| `Auth`         | `0004`      | Write, Read      | 4 bytes | Challenge (app→pager) / Response (pager→app) |
| `Command`      | `0005`      | Write            | 1 byte  | `0x00` = sync state, `0x01` = force brick |

---

## Authentication (anti-spoof, v1)

Goal: prevent a spoofed BLE central from triggering an unbrick.

### PSK provisioning (once, at pairing)
1. iOS app generates 16 random bytes as the PSK.
2. App writes PSK to ESP32 NVS via a separate provisioning characteristic
   (not listed above — only present during the initial pairing flow).
3. App stores PSK in iOS Keychain (kSecAttrAccessibleWhenUnlockedThisDeviceOnly).

### Per-session handshake (on each reconnect)
1. App writes a 4-byte random challenge to `Auth`.
2. Pager computes `response = HMAC-SHA256(psk, challenge)[0..3]`.
3. Pager writes 4-byte response into the `Auth` read value.
4. App reads `Auth` back and verifies. If mismatch → ignore all `UnbrickEvent`
   notifications until a new handshake succeeds.

> **v2 upgrade path:** Sign every `UnbrickEvent` counter value individually and
> rotate the PSK after each use.

---

## Un-brick flow

```
User holds button ≥ 2 s
  └─ pager: ++UnbrickEvent, notify
      └─ app: verify auth handshake is valid
          └─ app: clear ManagedSettings shield
              └─ app: write BrickState = 0x00
                  └─ pager: update UI (unbricked)
```

Fail-safe: if the BLE link is dropped while bricked, `ManagedSettings` persists.
The shield stays up. Never fail open.

---

## Brick flow

```
User taps "Start Focus" in app
  └─ app: calls ManagedSettings to raise shield
      └─ app: writes Command = 0x01 (force brick)
          └─ pager: sets BrickState = 0x01, notifies
              └─ pager: UI → bricked state
```

---

## Notes

- `BrickState` is the ground truth for the *pager UI*. The iOS shield state is
  the ground truth for the *phone*. They are kept in sync but the shield is
  authoritative for app blocking.
- The pager must persist `BrickState` in NVS so it survives power cycling while
  the phone is not reachable.
- Emergency unbrick policy (Phase 5): mirror Brick's limited-count emergency
  button in the app UI (bypasses the physical hold, counted and rate-limited).
