//
//  UbaAgentHelperApp.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/10/24.
//

import SwiftUI

@main
struct UbaAgentHelperApp: App {
    
    @State var isRunning = false
    
    var body: some Scene {
        

        
        MenuBarExtra {
            
            ContentView(status: $isRunning)
                .frame(width: 500, height: 500)
//                .onAppear(perform: {
//                    if let bundleID = Bundle.main.bundleIdentifier {
//                        UserDefaults.standard.removePersistentDomain(forName: bundleID)
//                    }
//                })
                
        } label: {
            HStack {
                let configuration = NSImage.SymbolConfiguration(pointSize: 16, weight: .light)
                    .applying(.init(hierarchicalColor: isRunning ? .green : .red))
                
                let image = NSImage(systemSymbolName: "bolt.circle.fill", accessibilityDescription: nil)
                let updateImage = image?.withSymbolConfiguration(configuration)
                
                Image(nsImage: updateImage!) // This works.
                // TODO: If we want custom text in the menu bar this will cover it along with colors
                //                    let font = NSFont.systemFont(ofSize: 16, weight: .light)
                //                let color = NSColor.red // Attempting to make the text red.
                //                    let attributes: [NSAttributedString.Key: Any] = [
                //                        .font: font,
                //                        .foregroundColor: colorx
                //                    ]
                
                //                    let attributedString = NSAttributedString(string: "Hello, world!", attributes: attributes)
                
                //                    Text(AttributedString(attributedString)) // This doesn't work.
            }
        }
        .menuBarExtraStyle(.window)
        
        //        WindowGroup {
        //            ContentView(status: $isRunning)
        //                .navigationTitle("UbaAgent Helper")
        //        }
        Settings {
            SettingsView()
            //                .environmentObject(appDelegate.utility)
        }
    }
}
