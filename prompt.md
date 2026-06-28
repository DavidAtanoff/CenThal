You are an expert systems programmer, compiler architect, and Unreal Engine engine developer. 

I have cloned a repository containing specific extracted subsystems of the Unreal Engine source code. 
Repository: https://github.com/DavidAtanoff/CenThal
Inside, you will find `links.txt` pointing to the exact upstream source, alongside local source directories for:
Unreal Build Tool
Unreal Header Tool
UObject
Related stuff to compiling/vm/building.

### MY GOAL
I am designing and developing a custom systems programming language. 
- It will compile down to machine code via an LLVM or Cranelift backend.
- It may use a Garbage Collector (GC) or a hybrid memory management model.
- Crucially, I want this language to have first-class, deep integration with Unreal Engine 5 (UE5) and be architecturally forward-compatible with the future Unreal Engine 6 (UE6).

### YOUR TASK
Scout, analyze, and map out the provided codebase, focusing entirely on how a custom language can hook into Unreal's architecture. Provide a comprehensive architectural blueprint covering the following areas:

1. The Reflection Bridge (UHT & UObject)
   - How does UHT parse C++ headers to generate the necessary `.generated.h` metadata?
   - How should my custom language compiler or an automated binding generator emit or hook into this reflection data so that my language's classes/structs are recognized by the `UClass`, `UFunction`, and `UProperty` systems?
   - If my language uses a GC, how can I safely bridge or synchronize its memory management with Unreal’s own `UObject` Garbage Collector to avoid dual-GC conflicts or dangling references?

2. The Build Pipeline Integration (UBT)
   - How does UBT orchestrate the build process? 
   - What is the cleanest way to inject my language's compiler (LLVM/Cranelift-based) into UBT? Should it be treated as a custom ThirdParty library toolchain, a Module rules extension, or a specialized compiler toolchain subclass?

3. Interoperability & FFI (Foreign Function Interface)
   - Unreal’s API relies heavily on C++ features (v-tables, complex templates, inline functions, specific calling conventions). 
   - Based on the source code provided, what are the primary friction points for a compiled language interfacing directly with C++? Is it better to target a pure C-API intermediate layer, or should my compiler emit C++ compatible ABI layout directly?

4. Designing for UE5 & Future-Proofing for UE6
   - Considering trends in modern engine architecture (like massive multi-threading, ECS/Mass Entity, and data-oriented design), what architectural paradigms should I build into my language now so it integrates flawlessly with UE5 and transitions smoothly into UE6?
   - Would I need to somehow follow certain language rules to avoid complicating any work done with UObject stuff when developing my language.

Please be highly technical, specific, and reference code patterns or structures you find in the provided repository to justify your architectural recommendations. If you require another folder from Unreal Engine 5 repository I will provide it to you. Long story short, you have to explain and suggest how we can properly make a language that will integrate with UE and provide all the features and flexibilities of a high-level language that UE5 would require for programming/scripting/modding/etc. This should be "native" to UE5, it shouldn't feel like a bolted on VM plugin for UE5 like the other plugins for example Lua and Angelscript, and it shouldn't transpile to C++ to compile for the sake of avoiding complexity of integrating with UE5. Preferably, I would also not want to recompile UE5 everytime to add our language support, if there would be a way like via a plugin or patch tool, it would be nice.