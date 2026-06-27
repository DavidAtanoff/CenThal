//
//  Color.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/21/24.
//

import Foundation
import SwiftUI

extension Color: RawRepresentable {
    public init?(rawValue: String) {
        guard let data = Data(base64Encoded: rawValue) else {
            self = .black
            return
        }

        do {
            let color = try NSKeyedUnarchiver.unarchivedObject(ofClass: NSColor.self, from: data) ?? .labelColor
            self = Color(color)
        } catch {
            self = .black
        }
    }

    public var rawValue: String {
        do {
            let data = try NSKeyedArchiver.archivedData(
                withRootObject: NSColor(self),
                requiringSecureCoding: false
            ) as Data
            return data.base64EncodedString()
        } catch {
            return ""
        }
    }
}
