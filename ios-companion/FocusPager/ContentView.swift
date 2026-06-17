//
//  ContentView.swift
//  FocusPager
//

import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var connection: PagerConnection
    @EnvironmentObject private var shield: ShieldManager

    var body: some View {
        NavigationStack {
            VStack(spacing: 24) {
                connectionBanner
                stateIndicator
                controls
                eventLog
                Spacer(minLength: 0)
            }
            .padding()
            .navigationTitle("Focus Pager")
        }
    }

    // MARK: Connection status

    private var connectionBanner: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(connectionColor)
                .frame(width: 10, height: 10)
            Text(connectionText)
                .font(.subheadline.weight(.medium))
                .foregroundStyle(.secondary)
            Spacer()
        }
    }

    private var connectionColor: Color {
        switch connection.connectionState {
        case .connected:   return .green
        case .connecting:  return .orange
        case .scanning:    return .yellow
        case .disconnected: return .red
        }
    }

    private var connectionText: String {
        switch connection.connectionState {
        case .connected:    return "Connected to \(PagerIdentity.advertisedName)"
        case .connecting:   return "Connecting…"
        case .scanning:     return "Scanning…"
        case .disconnected: return "Disconnected"
        }
    }

    // MARK: Brick state indicator

    private var stateIndicator: some View {
        let bricked = connection.pagerState == .bricked
        return VStack(spacing: 12) {
            Image(systemName: bricked ? "lock.fill" : "lock.open.fill")
                .font(.system(size: 56))
            Text(bricked ? "BRICKED" : "UNBRICKED")
                .font(.largeTitle.bold())
        }
        .foregroundStyle(bricked ? Color.red : Color.green)
        .frame(maxWidth: .infinity)
        .padding(.vertical, 28)
        .background(
            RoundedRectangle(cornerRadius: 20)
                .fill((bricked ? Color.red : Color.green).opacity(0.12))
        )
    }

    // MARK: Controls

    private var controls: some View {
        let connected = connection.connectionState == .connected
        let bricked = connection.pagerState == .bricked
        return VStack(spacing: 12) {
            Button {
                connection.forceBrick()
            } label: {
                Label("Force Brick", systemImage: "lock.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(.red)
            .disabled(!connected || bricked)

            Button {
                connection.triggerUnbrickFlow()
            } label: {
                Label("Unbrick Now", systemImage: "lock.open.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .disabled(!connected)
        }
    }

    // MARK: Event log

    private var eventLog: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Recent events")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 6) {
                    ForEach(connection.events.suffix(5).reversed()) { event in
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Text(event.timestamp, style: .time)
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.tertiary)
                            Text(event.message)
                                .font(.caption)
                            Spacer(minLength: 0)
                        }
                    }
                    if connection.events.isEmpty {
                        Text("No events yet")
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                }
            }
            .frame(maxHeight: 140)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(Color(.secondarySystemBackground))
        )
    }
}

#Preview {
    let shield = ShieldManager()
    return ContentView()
        .environmentObject(PagerConnection(shield: shield))
        .environmentObject(shield)
}
