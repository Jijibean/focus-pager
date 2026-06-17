//
//  FocusPagerApp.swift
//  FocusPager
//

import SwiftUI

@main
struct FocusPagerApp: App {
    @StateObject private var shield = ShieldManager()
    @StateObject private var connection: PagerConnection

    init() {
        let shield = ShieldManager()
        _shield = StateObject(wrappedValue: shield)
        _connection = StateObject(wrappedValue: PagerConnection(shield: shield))
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(connection)
                .environmentObject(shield)
        }
    }
}
