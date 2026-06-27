//
//  Symbols.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/21/24.
//

import SwiftUI

enum Symbols: String {
    case gear
    case wandAndStarsInverse = "wand.and.stars.inverse"

    var image: Image {
        Image(systemName: self.rawValue)
    }

    var name: String {
        self.rawValue
    }

    func nsImage(accessibilityDescription: String? = nil) -> NSImage? {
        return NSImage(systemSymbolName: self.rawValue, accessibilityDescription: accessibilityDescription)
    }

    func callAsFunction() -> String {
        return self.rawValue
    }
}
