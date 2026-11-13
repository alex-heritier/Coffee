import AppKit

@MainActor
final class CoffeeApp: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()
    private let sessionItem = NSMenuItem(title: "Coffee is off", action: nil, keyEquivalent: "")
    private let stopItem = NSMenuItem(title: "Stop", action: #selector(stop), keyEquivalent: "")
    private var process: Process?
    private var endDate: Date?
    private var timer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        menu.delegate = self
        menu.autoenablesItems = false
        sessionItem.isEnabled = false
        menu.addItem(sessionItem)
        menu.addItem(.separator())

        for minutes in [15, 30, 60, 120] {
            let title = minutes < 60 ? "\(minutes) minutes" : "\(minutes / 60) hour\(minutes == 60 ? "" : "s")"
            let item = NSMenuItem(title: title, action: #selector(startPreset(_:)), keyEquivalent: "")
            item.tag = minutes
            item.target = self
            menu.addItem(item)
        }

        let customItem = NSMenuItem(title: "Custom duration…", action: #selector(startCustom), keyEquivalent: "")
        customItem.target = self
        menu.addItem(customItem)
        menu.addItem(.separator())
        stopItem.target = self
        menu.addItem(stopItem)
        menu.addItem(.separator())
        let quitItem = NSMenuItem(title: "Quit Coffee", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
        statusItem.menu = menu
        updateStatus()

        timer = Timer.scheduledTimer(timeInterval: 1, target: self, selector: #selector(updateStatus), userInfo: nil, repeats: true)
        RunLoop.main.add(timer!, forMode: .common)
    }

    func menuWillOpen(_ menu: NSMenu) {
        updateStatus()
    }

    @objc private func startPreset(_ sender: NSMenuItem) {
        start(minutes: sender.tag)
    }

    @objc private func startCustom() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Keep your Mac awake"
        alert.informativeText = "Enter a duration in minutes."
        alert.addButton(withTitle: "Start")
        alert.addButton(withTitle: "Cancel")
        let input = NSTextField(string: "60")
        input.frame = NSRect(x: 0, y: 0, width: 240, height: 24)
        input.setAccessibilityLabel("Duration in minutes")
        alert.accessoryView = input
        alert.window.initialFirstResponder = input
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        guard let minutes = Int(input.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)),
              minutes > 0, minutes <= Int(Int32.max) / 60 else {
            showError("Enter a positive whole number of minutes (up to \(Int(Int32.max) / 60)).")
            return
        }
        start(minutes: minutes)
    }

    private func start(minutes: Int) {
        let nextProcess = Process()
        nextProcess.executableURL = URL(fileURLWithPath: "/usr/bin/caffeinate")
        // Watching Coffee's PID also releases assertions if Coffee exits unexpectedly.
        nextProcess.arguments = ["-dimsu", "-t", String(minutes * 60), "-w", String(ProcessInfo.processInfo.processIdentifier)]
        do {
            try nextProcess.run()
            stop()
            process = nextProcess
            endDate = Date().addingTimeInterval(TimeInterval(minutes * 60))
            updateStatus()
        } catch {
            showError("Could not start caffeinate: \(error.localizedDescription)")
        }
    }

    @objc private func stop() {
        if let process, process.isRunning {
            process.terminate()
        }
        process = nil
        endDate = nil
        updateStatus()
    }

    @objc private func updateStatus() {
        if let endDate, endDate <= Date() {
            stop()
            return
        }
        if let process, !process.isRunning {
            self.process = nil
            endDate = nil
        }
        let active = process != nil
        let symbol = active ? "cup.and.saucer.fill" : "cup.and.saucer"
        statusItem.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "Coffee")
        if let endDate {
            let remaining = max(0, Int(ceil(endDate.timeIntervalSinceNow)))
            let hours = remaining / 3600
            let minutes = (remaining % 3600) / 60
            let seconds = remaining % 60
            let time = hours > 0
                ? String(format: "%d:%02d:%02d", hours, minutes, seconds)
                : String(format: "%d:%02d", minutes, seconds)
            sessionItem.title = "Awake for \(time) more"
        } else {
            sessionItem.title = "Coffee is off"
        }
        statusItem.button?.toolTip = sessionItem.title
        stopItem.isEnabled = active
    }

    private func showError(_ message: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Coffee"
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    func applicationWillTerminate(_ notification: Notification) {
        timer?.invalidate()
        stop()
    }
}

@main
struct Coffee {
    @MainActor static func main() {
        let app = NSApplication.shared
        let delegate = CoffeeApp()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        withExtendedLifetime(delegate) {
            app.run()
        }
    }
}
