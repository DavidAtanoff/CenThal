//
//  LaunchItem.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/21/24.
//

import OSLog
import ServiceManagement
import SwiftUI

public enum LoginLaunch {
    private static let logger = Logger(subsystem: Constants.BundleIds.main, category: "login")
    fileprivate static let observable = Observable()

    public static var isEnabled: Bool {
        get { SMAppService.mainApp.status == .enabled }
        set {
            observable.objectWillChange.send()

            do {
                if newValue {
                    if SMAppService.mainApp.status == .enabled {
                        try? SMAppService.mainApp.unregister()
                    }

                    try SMAppService.mainApp.register()
                } else {
                    try SMAppService.mainApp.unregister()
                }
            } catch {
                logger.error("Failed to \(newValue ? "enable" : "disable"): \(error.localizedDescription)")
            }
        }
    }

    public static var wasLaunchedAtLogin: Bool {
        let event = NSAppleEventManager.shared().currentAppleEvent
        return event?.eventID == kAEOpenApplication
            && event?.paramDescriptor(forKeyword: keyAEPropData)?.enumCodeValue == keyAELaunchedAsLogInItem
    }
}

extension LoginLaunch {
    final class Observable: ObservableObject {
        var isEnabled: Bool {
            get { LoginLaunch.isEnabled }
            set {
                LoginLaunch.isEnabled = newValue
            }
        }
    }
}

extension LoginLaunch {
    public struct Toggle<Label: View>: View {
        @ObservedObject private var launchAtLogin = LoginLaunch.observable
        private let label: Label

        public init(@ViewBuilder label: () -> Label) {
            self.label = label()
        }

        public var body: some View {
            SwiftUI.Toggle(isOn: $launchAtLogin.isEnabled) { label }
        }
    }
}

extension LoginLaunch.Toggle<Text> {
    public init(_ titleKey: LocalizedStringKey) {
        label = Text(titleKey)
    }

    public init(_ title: some StringProtocol) {
        label = Text(title)
    }

    public init() {
        self.init("Launch At Login")
    }
}
