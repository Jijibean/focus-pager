//
//  BrickProtocol.swift
//  FocusPager
//
//  Canonical BLE contract for the Brick Control service.
//  Keep in sync with /protocol/brick-control.md and the ESP32 firmware
//  (firmware/main/brick_service.c).
//

import CoreBluetooth
import Foundation

/// Brick Control GATT service definition.
///
/// Base UUID: `B41C00xx-9E5A-4C7B-9D2F-0A1B2C3D4E5F` — the `xx` byte selects
/// the service (`01`) or characteristic (`02`–`05`).
enum BrickGATT {
    static let service     = CBUUID(string: "B41C0001-9E5A-4C7B-9D2F-0A1B2C3D4E5F")
    static let brickState  = CBUUID(string: "B41C0002-9E5A-4C7B-9D2F-0A1B2C3D4E5F")
    static let unbrickEvent = CBUUID(string: "B41C0003-9E5A-4C7B-9D2F-0A1B2C3D4E5F")
    static let auth        = CBUUID(string: "B41C0004-9E5A-4C7B-9D2F-0A1B2C3D4E5F")
    static let command     = CBUUID(string: "B41C0005-9E5A-4C7B-9D2F-0A1B2C3D4E5F")
    static let displayData = CBUUID(string: "B41C0006-9E5A-4C7B-9D2F-0A1B2C3D4E5F")

    /// Characteristics the app subscribes to (CCCD notify) on connect.
    static let notifyCharacteristics: [CBUUID] = [brickState, unbrickEvent]
}

/// `Command` characteristic opcodes (`B41C0005`).
enum BrickCommand: UInt8 {
    /// Ask the pager to re-sync / confirm its state. Used to confirm an unbrick.
    case sync = 0x00
    /// Force the pager into the bricked state.
    case forceBrick = 0x01
}

/// `BrickState` characteristic values (`B41C0002`).
enum PagerState: UInt8 {
    case unbricked = 0x00
    case bricked   = 0x01

    init(byte: UInt8) {
        self = (byte == PagerState.bricked.rawValue) ? .bricked : .unbricked
    }
}

/// High-level BLE link state, surfaced to the UI.
enum ConnectionState: Equatable {
    case disconnected
    case scanning
    case connecting
    case connected
}

/// `DisplayData` characteristic opcodes (`B41C0006`).
enum DisplayCommand: UInt8 {
    case timeSync    = 0x01
    case todoSet     = 0x02
    case todoClear   = 0x03
    case messageSet  = 0x04
    case messageClear = 0x05
}

/// Names the pager advertises under (used by the name-based scan fallback).
enum PagerIdentity {
    static let advertisedName = "FocusPager"
}

/// Pre-shared key shared between app and pager.
///
/// Phase 5: random 16-byte key matching the firmware's compiled-in default.
/// Must stay in sync with `s_psk` in brick_service.c.
enum PagerSecrets {
    static let psk: [UInt8] = [
        0x2E, 0x8F, 0x41, 0xA6, 0xF4, 0xE3, 0x67, 0x69,
        0xA0, 0x3E, 0x79, 0xC9, 0xF6, 0x6B, 0x0E, 0xB7,
    ]
}
