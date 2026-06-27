//
//  Preferences.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/21/24.
//

import Foundation

enum StorageKeys {
    
    struct Key<Value: Any> {
        let id: String
        let `default`: Value
    }
    
    static let XCodePath = Key(id: "xcodePath", default: "")
    static let UbaAgentPath = Key(id: "ubaAgentPath", default: "")
    static let P4DepotRoot = Key(id: "p4DepotRoot", default: "")
    static let AgentFromRepo = Key(id: "agentFromRepo", default: false)
}
