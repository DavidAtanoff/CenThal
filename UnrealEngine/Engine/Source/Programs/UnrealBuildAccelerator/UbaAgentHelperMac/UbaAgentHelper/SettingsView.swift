//
//  SettingsView.swift
//  UbaAgentHelper
//
//  Created by Zack Neyland on 1/21/24.
//

import SwiftUI

struct SettingsView: View {
    
    @AppStorage(StorageKeys.XCodePath.id)
    private var xcodePath: String = StorageKeys.XCodePath.default
    
    @AppStorage(StorageKeys.UbaAgentPath.id)
    private var ubaAgentPath: String = StorageKeys.UbaAgentPath.default
    
    @AppStorage(StorageKeys.P4DepotRoot.id)
    private var p4DepotRoot : String = StorageKeys.P4DepotRoot.default
    
    
    @AppStorage(StorageKeys.AgentFromRepo.id)
    private var agentFromP4 : Bool = StorageKeys.AgentFromRepo.default
    
    private enum Tabs: Hashable {
        case general
        case ubaAgent
    }
    
    @State private var isImporting: Bool = false
    @State private var p4BinaryPath : URL? = nil
    
    @State private var selection: Tabs = .general
    
    var body: some View {
        TabView(selection: $selection) {
            generalTab
            ubaAgentTab
        }
        .frame(width: 375, height: 350)
        .onAppear(perform: {
            findP4Binary()
        })
    }
    
    private var generalTab: some View {
        Form {
            Section("Paths") {
                VStack(alignment: .leading) {
                    Text("XCode Path")
                    Text("\(xcodePath.isEmpty ? "Please locate XCode" : xcodePath)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                HStack {
                    Spacer()
                    Button("Locate XCode") {
                        locateXCode()
                    }
                    Spacer()
                }
            }
            // TODO: Revisit launching at login at a later date
            Section {
                VStack(alignment: .leading) {
                    LoginLaunch.Toggle()
                }
            }
            Section("DEBUG") {
                VStack(alignment: .center)
                {
                    Button(action:
                            {
                        if let bundleID = Bundle.main.bundleIdentifier {
                            UserDefaults.standard.removePersistentDomain(forName: bundleID)
                        }
                    }, label: {
                        Text("Reset Settings to Default")
                    })
                }
            }
        }
        .formStyle(.grouped)
        .tabItem {
            Label("General", systemImage: Symbols.gear())
        }
        .tag(Tabs.general)
    }
    
    private var ubaAgentTab : some View {
        Form {
            Section("Uba Agent") {
                if let _ = p4BinaryPath {
                    Toggle(isOn: $agentFromP4, label: {
                        Text("Use Perforce")
                    })
                }
                
                if agentFromP4 {
                    HStack() {
                        VStack(alignment: .leading) {
                            Text("P4 Depot Root")
                            Text("e.g. //UE5/Main")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        TextField(text: $p4DepotRoot, prompt: Text("Click here"), axis: .vertical, label: {})
                    }
                    HStack {
                        Spacer()
                        Button("Install Agent") {
                            InstallUbaAgent()
                        }
                        .disabled(p4DepotRoot.isEmpty)
                        Spacer()
                    }
                    HStack {
                        VStack(alignment: .leading) {
                            Text("UbaAgentPath")
                            Text(ubaAgentPath.isEmpty ? "Please select an UbaAgent" : ubaAgentPath)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                }
                else {
                    HStack() {
                        VStack(alignment: .leading) {
                            Text("UbaAgentPath")
                            Text(ubaAgentPath.isEmpty ? "Please select an UbaAgent" : ubaAgentPath)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button(action: {
                            isImporting = true
                        }, label: {
                            Text("Locate Agent")
                        })
                        .fileImporter(isPresented: $isImporting,
                                      allowedContentTypes: [.executable],
                                      onCompletion: { result in
                            
                            switch result {
                            case .success(let url):
                                // url contains the URL of the chosen file.
                                ubaAgentPath = url.path()
                            case .failure(let error):
                                print(error)
                            }
                        })
                    }
                }
            }
        }
        .formStyle(.grouped)
        .tabItem {
            Label("UbaAgent Config", systemImage: Symbols.wandAndStarsInverse())
        }
        .tag(Tabs.ubaAgent)
    }
    
    func findP4Binary() {
        let p4Locations = [
            "/usr/local/bin/p4",
            "/usr/bin/p4",
            "/opt/homebrew/bin/p4"
        ]
        
        for loc in p4Locations {
            let url = URL(fileURLWithPath: loc)
            do {
                if try url.checkResourceIsReachable() {
                    p4BinaryPath = url
                    break
                }
            }
            catch {
                
            }
        }
    }
    
    func InstallUbaAgent() {
        let task = Process()
        
        let executableURL = p4BinaryPath
        task.executableURL = executableURL
        
        let pipe = Pipe()
        task.standardOutput = pipe
        
        if (p4DepotRoot.last == "/")
        {
            p4DepotRoot = String(p4DepotRoot.dropLast())
        }
        let args = ["print", "-q", (p4DepotRoot + "/Engine/Binaries/Mac/UnrealBuildAccelerator/UbaAgent")]
        task.arguments = args
        task.launch()
        
        //        try! task.run()
        //        task.waitUntilExit()
        
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        
        if !data.isEmpty {
            let ubaAgentUrl = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("UbaAgent/UbaAgent", isDirectory: false)
            
            FileManager.default.createFile(atPath: ubaAgentUrl.path, contents: data, attributes: [FileAttributeKey.posixPermissions: NSNumber(value: 0o775 as UInt16)])
            do {
                if try ubaAgentUrl.checkResourceIsReachable() {
                    ubaAgentPath = ubaAgentUrl.path
                }
            }
            catch {
            }
        }
        
    }
    
    func locateXCode() {
        let task = Process()
        
        let executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        task.executableURL = executableURL
        let pipe = Pipe()
        task.standardOutput = pipe
        
        let args = ["-p"]
        task.arguments = args
        
        try! task.run()
        task.waitUntilExit()
        
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        xcodePath = (String(data: data, encoding: String.Encoding.utf8) ?? "").trimmingCharacters(in: CharacterSet.newlines)
    }
    
    func writeToFile(data: Data, fileName: String){
        // get path of directory
        guard let directory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).last else {
            return
        }
        // create file url
        let fileurl =  directory.appendingPathComponent("\(fileName).txt")
        // if file exists then write data
        if FileManager.default.fileExists(atPath: fileurl.path) {
            if let fileHandle = FileHandle(forWritingAtPath: fileurl.path) {
                // seekToEndOfFile, writes data at the last of file(appends not override)
                fileHandle.seekToEndOfFile()
                fileHandle.write(data)
                fileHandle.closeFile()
            }
            else {
                print("Can't open file to write.")
            }
        }
        else {
            // if file does not exist write data for the first time
            do{
                try data.write(to: fileurl, options: .atomic)
            }catch {
                print("Unable to write in new file.")
            }
        }
    }
}

#Preview {
    SettingsView()
}
