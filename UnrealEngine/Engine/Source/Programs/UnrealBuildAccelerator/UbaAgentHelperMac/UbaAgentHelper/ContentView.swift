//
//  ContentView.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/10/24.
//

import SwiftUI

// Implement this support: https://developer.apple.com/forums/thread/735862

struct ContentView: View {
    
    @AppStorage(StorageKeys.XCodePath.id)
    private var xcodePath: String = StorageKeys.XCodePath.default
    
    @AppStorage(StorageKeys.UbaAgentPath.id)
    private var ubaAgentPath: String = StorageKeys.UbaAgentPath.default
    
    @State var activeTask : Process? = nil
    @Binding var status: Bool
    //    @State var pathToAgent : String = ""
    @State var ipAddress = ""
    @State var log : String = ""
    
    var body: some View {
        VStack {
            GroupBox(label: Text("Uba Agent Configuration"), content: {
                HStack {
                    Text("UbaHost IP:")
                    //                        .fontWeight(.bold)
                    TextField(text: $ipAddress, prompt: Text("e.g.: 127.0.0.1"), label: {})
                }
            })
            
            GroupBox(label: Text("UbaAgent Output"), content: {
                HStack {
                    Spacer()
                }
                ScrollView {
                    TextEditor(text: $log)
                }
                .defaultScrollAnchor(.bottom)
            })
            Divider()
            HStack {
                if let activeTask = activeTask, activeTask.isRunning {
                    Button(action: killTask, label: {
                        Text("Kill")
                    })
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                }
                else {
                    Button(action: {
                        runUbaAgent()
                    }, label: {
                        Text("Start Agent")
                            .bold()
                    })
                    .disabled(canRun())
                }
                
                Spacer()
                SettingsLink()
                Button(action: {
                    NSApplication.shared.terminate(nil)
                }, label: {
                    Text("Quit")
                })
                .buttonStyle(.borderedProminent)
                .tint(.red)
            }
        }
        .padding()
        .background(Color.black.opacity(0.4))
    }

    
    func canRun() -> Bool {
        return ipAddress.isEmpty || xcodePath.isEmpty || ubaAgentPath.isEmpty
    }
    
    func runUbaAgent() {
        // This should be replaced with the recs from here:
        // https://developer.apple.com/forums/thread/690310
        if ((activeTask?.isRunning) != nil) {
            killTask()
        }
        
        log = ""
        
        let task = Process()
        task.executableURL = URL(fileURLWithPath: ubaAgentPath)
        task.arguments = ["-host=\(ipAddress)", "-log", "-populatecas=\(xcodePath)"]
        let outputPipe = Pipe()
        task.standardOutput = outputPipe
        let outputHandle = outputPipe.fileHandleForReading
        
        outputHandle.readabilityHandler = { pipe in
            if let ouput = String(data: pipe.availableData, encoding: .utf8) {
                if !ouput.isEmpty {
                    log += " " + ouput
                }
            } else {
                print("Error decoding data: \(pipe.availableData)")
            }
        }
        activeTask = task
        task.launch()
        
        if task.isRunning {
            status = true
        }
        //todo: Swap to this later
        //        Task {
        //            try! task.run()
        //            task.waitUntilExit()
        //        }
    }
    
    func killTask() {
        if let activeTask = activeTask, activeTask.isRunning {
            activeTask.terminate()
            activeTask.waitUntilExit()
            log += "\n\nKILLED\n\n"
            status = false
        }
    }
}

#Preview {
    ContentView(status: .constant(true))
}
