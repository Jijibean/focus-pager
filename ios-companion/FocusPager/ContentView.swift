//
//  ContentView.swift
//  FocusPager
//

import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var connection: PagerConnection
    @EnvironmentObject private var shield: ShieldManager

    // MARK: Display settings persistence

    @AppStorage("todo_0_text") private var todo0Text = ""
    @AppStorage("todo_0_checked") private var todo0Checked = false
    @AppStorage("todo_1_text") private var todo1Text = ""
    @AppStorage("todo_1_checked") private var todo1Checked = false
    @AppStorage("todo_2_text") private var todo2Text = ""
    @AppStorage("todo_2_checked") private var todo2Checked = false
    @AppStorage("todo_3_text") private var todo3Text = ""
    @AppStorage("todo_3_checked") private var todo3Checked = false
    @AppStorage("customMessage") private var customMessage = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 24) {
                    connectionBanner
                    stateIndicator
                    controls
                    displaySection
                    eventLog
                    Spacer(minLength: 0)
                }
                .padding()
            }
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

    // MARK: Display settings

    private var displaySection: some View {
        let connected = connection.connectionState == .connected
        return VStack(alignment: .leading, spacing: 16) {
            Text("Display")
                .font(.headline)

            // Time sync
            Button {
                connection.syncTime()
            } label: {
                Label("Sync Time", systemImage: "clock")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .disabled(!connected)

            // To-do list
            VStack(alignment: .leading, spacing: 8) {
                Text("To-Do List")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.secondary)

                todoRow(index: 0, text: $todo0Text, checked: $todo0Checked)
                todoRow(index: 1, text: $todo1Text, checked: $todo1Checked)
                todoRow(index: 2, text: $todo2Text, checked: $todo2Checked)
                todoRow(index: 3, text: $todo3Text, checked: $todo3Checked)

                HStack(spacing: 12) {
                    Button("Send All") {
                        sendAllTodos()
                    }
                    .buttonStyle(.bordered)
                    .disabled(!connected)

                    Button("Clear All") {
                        clearAllTodos()
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    .disabled(!connected)
                }
            }

            // Custom message
            VStack(alignment: .leading, spacing: 8) {
                Text("Custom Message")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.secondary)

                HStack {
                    TextField("Message for pager", text: $customMessage)
                        .textFieldStyle(.roundedBorder)

                    Button {
                        if customMessage.isEmpty {
                            connection.clearMessage()
                        } else {
                            connection.sendMessage(customMessage)
                        }
                    } label: {
                        Image(systemName: "paperplane.fill")
                    }
                    .buttonStyle(.bordered)
                    .disabled(!connected)

                    Button {
                        customMessage = ""
                        connection.clearMessage()
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    .disabled(!connected)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(Color(.secondarySystemBackground))
        )
    }

    private func todoRow(index: Int, text: Binding<String>, checked: Binding<Bool>) -> some View {
        HStack(spacing: 8) {
            Button {
                checked.wrappedValue.toggle()
                if !text.wrappedValue.isEmpty {
                    connection.sendTodo(
                        index: UInt8(index),
                        checked: checked.wrappedValue,
                        text: text.wrappedValue
                    )
                }
            } label: {
                Image(systemName: checked.wrappedValue ? "checkmark.square.fill" : "square")
                    .foregroundStyle(checked.wrappedValue ? .green : .secondary)
            }
            .buttonStyle(.plain)

            TextField("Item \(index + 1)", text: text)
                .textFieldStyle(.roundedBorder)
        }
    }

    private func sendAllTodos() {
        let items: [(String, Bool)] = [
            (todo0Text, todo0Checked),
            (todo1Text, todo1Checked),
            (todo2Text, todo2Checked),
            (todo3Text, todo3Checked),
        ]
        connection.clearTodos()
        for (i, item) in items.enumerated() {
            guard !item.0.isEmpty else { continue }
            connection.sendTodo(index: UInt8(i), checked: item.1, text: item.0)
        }
        if !customMessage.isEmpty {
            connection.sendMessage(customMessage)
        }
    }

    private func clearAllTodos() {
        todo0Text = ""; todo0Checked = false
        todo1Text = ""; todo1Checked = false
        todo2Text = ""; todo2Checked = false
        todo3Text = ""; todo3Checked = false
        connection.clearTodos()
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
