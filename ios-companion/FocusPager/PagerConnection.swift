//
//  PagerConnection.swift
//  FocusPager
//
//  BLE central: owns the CBCentralManager + CBPeripheral, discovers the Brick
//  Control service, subscribes to notifications, and drives the brick/unbrick
//  flows. Also implements `PeripheralIO` so `AuthService` can write/read the
//  Auth characteristic via async/await.
//
//  Threading: the central manager runs on the main queue, so all delegate
//  callbacks and @Published mutations happen on the main thread.
//

import CoreBluetooth
import Foundation
import os

@MainActor
final class PagerConnection: NSObject, ObservableObject {

    // MARK: Published UI state

    @Published private(set) var connectionState: ConnectionState = .disconnected
    @Published private(set) var pagerState: PagerState = .unbricked
    @Published private(set) var events: [LogEvent] = []

    struct LogEvent: Identifiable {
        let id = UUID()
        let timestamp = Date()
        let message: String
    }

    // MARK: Dependencies

    private let shield: ShieldManager
    private let auth = AuthService()

    // MARK: CoreBluetooth

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?

    private var brickStateChar: CBCharacteristic?
    private var unbrickEventChar: CBCharacteristic?
    private var authChar: CBCharacteristic?
    private var commandChar: CBCharacteristic?

    private static let restoreIdentifier = "com.focuspager.central"

    // MARK: async continuation bookkeeping (for PeripheralIO)

    private var writeContinuations: [CBUUID: CheckedContinuation<Void, Error>] = [:]
    private var readContinuations: [CBUUID: CheckedContinuation<Data, Error>] = [:]

    enum IOError: Error { case notConnected, disconnected }

    // MARK: scan fallback

    private var scanFallbackTask: Task<Void, Never>?
    private var usingNameFallback = false

    // MARK: unbrick flow guard

    private var unbrickInFlight = false
    private var lastUnbrickCounter: UInt8?

    private let log = Logger(subsystem: "com.focuspager.app", category: "BLE")

    // MARK: Init

    init(shield: ShieldManager) {
        self.shield = shield
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier]
        )
    }

    // MARK: Public commands

    /// App-initiated brick: write Command = forceBrick, raise the shield, and
    /// optimistically reflect bricked state (the BrickState notify confirms it).
    func forceBrick() {
        guard let peripheral, let commandChar else {
            logEvent("Cannot brick — not connected")
            return
        }
        shield.activateShield()
        peripheral.writeValue(Data([BrickCommand.forceBrick.rawValue]), for: commandChar, type: .withResponse)
        setPagerState(.bricked, reason: "Bricked (command sent)")
    }

    /// Manually run the unbrick handshake (also what an UnbrickEvent triggers).
    func triggerUnbrickFlow() {
        startUnbrickFlow(triggeredBy: "manual")
    }

    // MARK: Scanning / connection lifecycle

    private func startScan() {
        guard central.state == .poweredOn else { return }
        guard connectionState != .connected, peripheral == nil else { return }

        usingNameFallback = false
        connectionState = .scanning
        logEvent("Scanning for Brick Control service…")
        central.scanForPeripherals(withServices: [BrickGATT.service], options: nil)

        // Fallback: the ESP32 advertises an ANCS solicitation UUID, so it may
        // not surface on a service-UUID scan. After 5s with no hit, scan for
        // everything and filter by advertised name.
        scanFallbackTask?.cancel()
        scanFallbackTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 5 * 1_000_000_000)
            guard !Task.isCancelled else { return }
            self?.beginNameFallbackScanIfNeeded()
        }
    }

    private func beginNameFallbackScanIfNeeded() {
        guard central.state == .poweredOn, connectionState == .scanning, peripheral == nil else { return }
        usingNameFallback = true
        logEvent("No service match — falling back to name scan")
        central.stopScan()
        central.scanForPeripherals(withServices: nil, options: nil)
    }

    private func stopScanning() {
        scanFallbackTask?.cancel()
        scanFallbackTask = nil
        if central.state == .poweredOn { central.stopScan() }
    }

    private func connect(to peripheral: CBPeripheral) {
        stopScanning()
        self.peripheral = peripheral
        peripheral.delegate = self
        connectionState = .connecting
        logEvent("Connecting to \(peripheral.name ?? "pager")…")
        central.connect(peripheral, options: nil)
    }

    private func teardownAndReconnect(reason: String) {
        logEvent(reason)
        // Fail any in-flight async I/O so awaiting tasks don't hang.
        failPendingIO(IOError.disconnected)
        brickStateChar = nil
        unbrickEventChar = nil
        authChar = nil
        commandChar = nil
        peripheral = nil
        lastUnbrickCounter = nil
        unbrickInFlight = false
        connectionState = .disconnected
        startScan()
    }

    // MARK: Brick/unbrick internals

    private func setPagerState(_ state: PagerState, reason: String) {
        pagerState = state
        logEvent(reason)
    }

    private func startUnbrickFlow(triggeredBy source: String) {
        guard !unbrickInFlight else {
            log.debug("Unbrick already in flight — ignoring \(source) trigger")
            return
        }
        guard let authChar else {
            logEvent("Cannot unbrick — Auth characteristic unavailable")
            return
        }
        unbrickInFlight = true
        Task { @MainActor in
            defer { unbrickInFlight = false }
            do {
                let passed = try await auth.verifyHandshake(authChar: authChar, io: self)
                if passed {
                    logEvent("Auth passed")
                    shield.deactivateShield()
                    // Confirm the unbrick to the pager via Command = sync.
                    if let peripheral, let commandChar {
                        peripheral.writeValue(Data([BrickCommand.sync.rawValue]), for: commandChar, type: .withResponse)
                    }
                    setPagerState(.unbricked, reason: "Unbricked")
                } else {
                    // Mismatch → ignore, stay bricked. Never fail open.
                    logEvent("Auth FAILED — staying bricked")
                }
            } catch {
                logEvent("Unbrick handshake error: \(error.localizedDescription)")
            }
        }
    }

    private func handleUnbrickEvent(_ value: Data) {
        let counter = value.first ?? 0
        logEvent("UnbrickEvent #\(counter) received")
        // A notify on this characteristic *is* the button-hold event; act on it.
        // Track the counter only to de-dupe a redelivery of the same value.
        if lastUnbrickCounter == counter {
            log.debug("Duplicate UnbrickEvent counter \(counter) — ignoring")
            return
        }
        lastUnbrickCounter = counter
        startUnbrickFlow(triggeredBy: "UnbrickEvent #\(counter)")
    }

    // MARK: Logging

    private func logEvent(_ message: String) {
        log.info("\(message, privacy: .public)")
        events.append(LogEvent(message: message))
        if events.count > 20 { events.removeFirst(events.count - 20) }
    }

    // MARK: async I/O failure helper

    private func failPendingIO(_ error: Error) {
        let writes = writeContinuations; writeContinuations.removeAll()
        let reads = readContinuations; readContinuations.removeAll()
        writes.values.forEach { $0.resume(throwing: error) }
        reads.values.forEach { $0.resume(throwing: error) }
    }
}

// MARK: - PeripheralIO

extension PagerConnection: PeripheralIO {

    func write(_ data: Data, to characteristic: CBCharacteristic) async throws {
        guard let peripheral else { throw IOError.notConnected }
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            writeContinuations[characteristic.uuid] = cont
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
        }
    }

    func read(_ characteristic: CBCharacteristic) async throws -> Data {
        guard let peripheral else { throw IOError.notConnected }
        return try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Data, Error>) in
            readContinuations[characteristic.uuid] = cont
            peripheral.readValue(for: characteristic)
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension PagerConnection: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            logEvent("Bluetooth ready")
            // If state restoration handed us a peripheral, reconnect it.
            if let peripheral, connectionState != .connected {
                connect(to: peripheral)
            } else {
                startScan()
            }
        case .poweredOff:
            teardownAndReconnect(reason: "Bluetooth powered off")
            connectionState = .disconnected
        case .unauthorized:
            logEvent("Bluetooth permission denied")
            connectionState = .disconnected
        case .unsupported:
            logEvent("Bluetooth unsupported on this device")
            connectionState = .disconnected
        default:
            connectionState = .disconnected
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral],
           let first = restored.first {
            peripheral = first
            first.delegate = self
            logEvent("Restored peripheral from background")
        }
    }

    func centralManager(_ central: CBCentralManager,
                         didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any],
                         rssi RSSI: NSNumber) {
        if usingNameFallback {
            let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
            let name = advName ?? peripheral.name
            guard name == PagerIdentity.advertisedName else { return }
        }
        connect(to: peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        // Stay in `.connecting` until characteristics are resolved — otherwise
        // the UI would show "Connected" while brick/unbrick are still unusable.
        logEvent("Link up — discovering services")
        peripheral.discoverServices([BrickGATT.service])
    }

    func centralManager(_ central: CBCentralManager,
                         didFailToConnect peripheral: CBPeripheral,
                         error: Error?) {
        teardownAndReconnect(reason: "Failed to connect: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(_ central: CBCentralManager,
                         didDisconnectPeripheral peripheral: CBPeripheral,
                         error: Error?) {
        let why = error?.localizedDescription ?? "clean"
        teardownAndReconnect(reason: "Disconnected (\(why)) — rescanning")
    }
}

// MARK: - CBPeripheralDelegate

extension PagerConnection: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { logEvent("Service discovery error: \(error.localizedDescription)"); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == BrickGATT.service }) else {
            logEvent("Brick Control service not found")
            return
        }
        // Discover all characteristics and filter locally. Naming specific
        // UUIDs can fail with CBError code 8 ("UUID not allowed") when iOS is
        // serving a stale GATT cache whose layout no longer matches.
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error { logEvent("Characteristic discovery error: \(error.localizedDescription)"); return }
        for char in service.characteristics ?? [] {
            switch char.uuid {
            case BrickGATT.brickState:
                brickStateChar = char
                peripheral.setNotifyValue(true, for: char)
                peripheral.readValue(for: char)   // read current state on connect
            case BrickGATT.unbrickEvent:
                unbrickEventChar = char
                peripheral.setNotifyValue(true, for: char)
            case BrickGATT.auth:
                authChar = char
            case BrickGATT.command:
                commandChar = char
            default:
                break
            }
        }

        // Only now is the pager actually usable.
        let missing = [
            ("BrickState", brickStateChar), ("UnbrickEvent", unbrickEventChar),
            ("Auth", authChar), ("Command", commandChar),
        ].filter { $0.1 == nil }.map(\.0)

        if missing.isEmpty {
            connectionState = .connected
            logEvent("Characteristics ready — pager usable")
        } else {
            logEvent("Missing characteristics: \(missing.joined(separator: ", "))")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        // Resume an outstanding async read (used by the Auth handshake) first.
        if let cont = readContinuations.removeValue(forKey: characteristic.uuid) {
            if let error { cont.resume(throwing: error) }
            else { cont.resume(returning: characteristic.value ?? Data()) }
            return
        }

        if let error { logEvent("Read/notify error: \(error.localizedDescription)"); return }
        guard let value = characteristic.value else { return }

        switch characteristic.uuid {
        case BrickGATT.brickState:
            let state = PagerState(byte: value.first ?? 0)
            setPagerState(state, reason: "BrickState = \(state == .bricked ? "BRICKED" : "UNBRICKED")")
        case BrickGATT.unbrickEvent:
            handleUnbrickEvent(value)
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let cont = writeContinuations.removeValue(forKey: characteristic.uuid) {
            if let error { cont.resume(throwing: error) }
            else { cont.resume(returning: ()) }
        } else if let error {
            logEvent("Write error: \(error.localizedDescription)")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            logEvent("Subscribe error: \(error.localizedDescription)")
        }
    }
}
