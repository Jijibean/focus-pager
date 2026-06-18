//
//  AuthService.swift
//  FocusPager
//
//  Stateless challenge-response verification for the unbrick handshake.
//
//  Implements steps 2–5 of the unbrick sequence:
//    2. generate a 4-byte random challenge
//    3. write it to the Auth characteristic
//    4. read back the pager's 4-byte HMAC response
//    5. independently compute HMAC-SHA256(PSK, challenge)[0..3] and compare
//
//  The CoreBluetooth write/read are bridged to async/await via `PeripheralIO`,
//  which `PagerConnection` implements with CheckedContinuations over the
//  delegate callbacks. Routing I/O through the single peripheral delegate (vs.
//  swapping `peripheral.delegate` here) avoids dropping BrickState/UnbrickEvent
//  notifications that may arrive mid-handshake.
//

import CoreBluetooth
import CryptoKit
import Foundation

/// Async wrapper over the CoreBluetooth write/read delegate dance.
protocol PeripheralIO: AnyObject {
    /// Write `data` with response, completing when `didWriteValueFor` fires.
    func write(_ data: Data, to characteristic: CBCharacteristic) async throws
    /// Issue a read, completing with the value from `didUpdateValueFor`.
    func read(_ characteristic: CBCharacteristic) async throws -> Data
}

struct AuthService {

    enum AuthError: Error {
        case malformedResponse
    }

    /// 16-byte pre-shared key.
    var psk: [UInt8] = PagerSecrets.psk

    /// Runs the challenge-response handshake against the pager.
    ///
    /// - Returns: The 4-byte challenge on success (needed by `signCommand` to
    ///   authenticate follow-up writes), or `nil` if the pager failed auth.
    func verifyHandshake(authChar: CBCharacteristic, io: PeripheralIO) async throws -> Data? {
        // 2. Generate a fresh 4-byte random challenge.
        var challenge = Data(count: 4)
        challenge.withUnsafeMutableBytes { raw in
            _ = SecRandomCopyBytes(kSecRandomDefault, raw.count, raw.baseAddress!)
        }

        // 3. Write the challenge, waiting for the write to be acknowledged
        //    before issuing the read (CoreBluetooth does not order these for us).
        try await io.write(challenge, to: authChar)

        // 4. Read back the pager's response.
        let response = try await io.read(authChar)
        guard response.count == 4 else { throw AuthError.malformedResponse }

        // 5. Independently compute the expected HMAC and compare.
        let expected = Self.hmac4(psk: psk, challenge: challenge)
        if Self.constantTimeEqual(response, expected) {
            return challenge
        }
        return nil
    }

    /// Build a signed command: `[cmd, HMAC(PSK, challenge || cmd)[0:4]]`.
    /// The firmware verifies this before executing the command.
    func signCommand(challenge: Data, command: UInt8) -> Data {
        let msg = challenge + Data([command])
        let key = SymmetricKey(data: Data(psk))
        let mac = HMAC<SHA256>.authenticationCode(for: msg, using: key)
        return Data([command]) + Data(mac.prefix(4))
    }

    /// HMAC-SHA256(PSK, challenge) truncated to the first 4 bytes — matches the
    /// firmware's `compute_hmac4()`.
    static func hmac4(psk: [UInt8], challenge: Data) -> Data {
        let key = SymmetricKey(data: Data(psk))
        let mac = HMAC<SHA256>.authenticationCode(for: challenge, using: key)
        return Data(mac.prefix(4))
    }

    /// Length-checked, constant-time comparison to avoid leaking timing info.
    private static func constantTimeEqual(_ a: Data, _ b: Data) -> Bool {
        guard a.count == b.count else { return false }
        var diff: UInt8 = 0
        for i in 0..<a.count { diff |= a[i] ^ b[i] }
        return diff == 0
    }
}
