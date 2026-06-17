//
//  ShieldManager.swift
//  FocusPager
//
//  Phase 2 stub for the app-blocking shield.
//
//  Phase 3 replaces the bodies of `activateShield()` / `deactivateShield()`
//  with FamilyControls + ManagedSettings (raise/clear a ManagedSettingsStore
//  shield over a FamilyActivitySelection). The public surface here is designed
//  so that drop-in happens without touching callers.
//

import Foundation
import os

@MainActor
final class ShieldManager: ObservableObject {

    /// Whether the app-blocking shield is currently raised.
    ///
    /// In Phase 3 this becomes the ground truth for the *phone* (the pager's
    /// `BrickState` is ground truth for the *pager UI*). Never fail open: if the
    /// BLE link drops while shielded, the shield must persist.
    @Published private(set) var isShieldActive = false

    private let log = Logger(subsystem: "com.focuspager.app", category: "Shield")

    /// Raise the shield (block configured apps).
    ///
    /// - Phase 3: `store.shield.applications = selection.applicationTokens` etc.
    func activateShield() {
        guard !isShieldActive else { return }
        isShieldActive = true
        log.info("Shield activated (Phase 2 stub — no apps actually blocked yet)")
    }

    /// Clear the shield (unblock apps).
    ///
    /// - Phase 3: `store.shield.applications = nil` (and related categories).
    func deactivateShield() {
        guard isShieldActive else { return }
        isShieldActive = false
        log.info("Shield deactivated (Phase 2 stub)")
    }
}
