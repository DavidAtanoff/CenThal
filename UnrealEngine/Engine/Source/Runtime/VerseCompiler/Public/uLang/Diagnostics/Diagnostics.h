// Copyright Epic Games, Inc. All Rights Reserved.
// uLang Parser Public API

#pragma once

#include "uLang/Common/Algo/Cases.h"
#include "uLang/Common/Containers/SharedPointerArray.h"
#include "uLang/Common/Misc/Event.h"
#include "uLang/Diagnostics/Glitch.h"

namespace uLang
{
enum class EBuildPhase : uint8_t
{
    Parse,
    SemanticAnalysis,
    Localization,
    IrGeneration,
    CodeGen,
    Link,
};

enum class EBuildEvent
{
    PersistentWeakMapDefinition,
    EntitlementSubclassDefinition,
    FunctionDefinition,
    ClassDefinition,
    TopLevelDefinition,
    UseOfExperimentalDefinition,
    MissingEntitlement,
    PhaseStarted,
    PhaseCompleted,
};

struct SBuildEventInfo
{
    SBuildEventInfo(EBuildEvent Type, uint32_t Int32)
        : Type{Type}
        , Int32{Int32}
    {
    }

    SBuildEventInfo(EBuildEvent Type, CUTF8String String)
        : Type{Type}
        , String{Move(String)}
    {
    }

    SBuildEventInfo() = delete;

    SBuildEventInfo(const SBuildEventInfo&) = delete;

    SBuildEventInfo(SBuildEventInfo&&) = delete;

    SBuildEventInfo& operator=(const SBuildEventInfo&) = delete;

    SBuildEventInfo& operator=(SBuildEventInfo&&) = delete;

    ~SBuildEventInfo()
    {
        if (Type == Cases<EBuildEvent::UseOfExperimentalDefinition, EBuildEvent::MissingEntitlement>)
        {
            String.~CUTF8String();
        }
        else
        {
            Int32.~uint32_t();
        }
    }

    EBuildEvent Type;
    union
    {
        uint32_t Int32;
        CUTF8String String;
    };
};

/// Various statistics for a given build that can be used in analytics.
struct SBuildStatistics
{
    uint32_t NumPersistentWeakMaps = 0;
    uint32_t NumEntitlementSubclasses = 0;
    uint32_t NumFunctions = 0;
    uint32_t NumClasses = 0;
    uint32_t NumTopLevelDefinitions = 0;
    TArray<CUTF8String> UsesOfExperimentalDefinitions;
    TArray<CUTF8String> MissingEntitlements;
};

// Accumulated issues for full set of compilation passes
class CDiagnostics : public CSharedMix
{
public:

    // Just warnings (no info or errors)
    bool                            HasWarnings() const { return _Glitches.ContainsByPredicate([](const SGlitch* Glitch) -> bool { return Glitch->_Result.IsWarning(); }); }
    VERSECOMPILER_API int32_t                         GetWarningNum() const;

    // Just errors (no info or warnings)
    bool                            HasErrors() const { return _Glitches.ContainsByPredicate([](const SGlitch* Glitch) -> bool { return Glitch->_Result.IsError(); }); }
    VERSECOMPILER_API int32_t                         GetErrorNum() const;

    // Any type of glitch including info, warnings and errors
    bool                            HasGlitches() const { return _Glitches.IsFilled(); }
    int32_t                         GetGlitchNum() const { return _Glitches.Num(); }
    const TSRefArray<SGlitch>&      GetGlitches() const { return _Glitches; }
    bool                            IsGlitchWithId(uintptr_t VstIdentifier) const { return _Glitches.ContainsByPredicate([VstIdentifier](const SGlitch* Glitch) -> bool { return Glitch->_Locus._VstIdentifier == VstIdentifier; }); }

    bool HasUseOfExperimentalDefinition() const { return !_Statistics.UsesOfExperimentalDefinitions.IsEmpty(); }
    const SBuildStatistics& GetStatistics() const
    {
        return _Statistics;
    }

    void Reset()
    {
        _Glitches.Empty();
    }

    void AppendGlitch(const TSRef<SGlitch>& Glitch)
    {
        _Glitches.Add(Glitch);
        _OnGlitchEvent.Broadcast(Glitch);
    }

    void AppendGlitch(const TSPtr<SGlitch>& Glitch)
    {
        AppendGlitch(Glitch.AsRef());
    }

    void AppendGlitch(SGlitchResult&& Result, SGlitchLocus&& Locus)
    {
        AppendGlitch(TSRef<SGlitch>::New(Move(Result), Move(Locus)));
    }

    void AppendGlitch(SGlitchResult&& Result)
    {
        AppendGlitch(TSRef<SGlitch>::New(Move(Result), SGlitchLocus()));
    }

    void AppendGlitches(TSRefArray<SGlitch>& Glitches)
    {
        for (const TSRef<SGlitch>& NewGlitch : Glitches)
        {
            _OnGlitchEvent.Broadcast(NewGlitch);
        }
        _Glitches.Append(Glitches);
    }

    void Append(CDiagnostics&& Other)
    {
        for (const TSRef<SGlitch>& NewGlitch : Other._Glitches)
        {
            _OnGlitchEvent.Broadcast(NewGlitch);
        }
        _Glitches.Append(Move(Other._Glitches));

        for (uint32_t I = Other._Statistics.NumPersistentWeakMaps; I != 0; --I)
        {
            _OnBuildStatisticEvent.Broadcast({EBuildEvent::PersistentWeakMapDefinition, 1});
        }
        _Statistics.NumPersistentWeakMaps += Other._Statistics.NumPersistentWeakMaps;

        _OnBuildStatisticEvent.Broadcast({EBuildEvent::EntitlementSubclassDefinition, Other._Statistics.NumEntitlementSubclasses});
        _Statistics.NumEntitlementSubclasses += Other._Statistics.NumEntitlementSubclasses;

        _Statistics.NumClasses += Other._Statistics.NumClasses;

        _Statistics.NumFunctions += Other._Statistics.NumFunctions;

        _Statistics.NumTopLevelDefinitions += Other._Statistics.NumTopLevelDefinitions;

        for (CUTF8String UseOfExperimentalDefinition : Other._Statistics.UsesOfExperimentalDefinitions)
        {
            _OnBuildStatisticEvent.Broadcast({EBuildEvent::UseOfExperimentalDefinition, Move(UseOfExperimentalDefinition)});
        }
        _Statistics.UsesOfExperimentalDefinitions.Append(Other._Statistics.UsesOfExperimentalDefinitions);

        for (const CUTF8String& MissingEntitlement : Other._Statistics.MissingEntitlements)
        {
            _OnBuildStatisticEvent.Broadcast({EBuildEvent::MissingEntitlement, MissingEntitlement});
        }
        _Statistics.MissingEntitlements.Append(Other._Statistics.MissingEntitlements);
    }

    void AppendPersistentWeakMap()
    {
        ++_Statistics.NumPersistentWeakMaps;
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::PersistentWeakMapDefinition, 1});
    }

    void AppendEntitlementSubclasses(uint32_t Count)
    {
        _Statistics.NumEntitlementSubclasses += Count;
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::EntitlementSubclassDefinition, Count});
    }

    void AppendFunctionDefinition(const uint32_t Count)
    {
        _Statistics.NumFunctions += Count;
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::FunctionDefinition, 1});
    }

    void AppendClassDefinition(const uint32_t Count)
    {
        _Statistics.NumClasses += Count;
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::ClassDefinition, 1});
    }

    void AppendTopLevelDefinition(const uint32_t Count)
    {
        _Statistics.NumTopLevelDefinitions += Count;
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::TopLevelDefinition, 1});
    }

    void AppendUseOfExperimentalDefinition(CUTF8String UseOfExperimentalDefinition)
    {
        _Statistics.UsesOfExperimentalDefinitions.Emplace(UseOfExperimentalDefinition);
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::UseOfExperimentalDefinition, Move(UseOfExperimentalDefinition)});
    }

    void AppendMissingEntitlement(CUTF8String MissingEntitlement)
    {
        _Statistics.MissingEntitlements.Emplace(MissingEntitlement);
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::MissingEntitlement, Move(MissingEntitlement)});
    }

    void BeginPhase(EBuildPhase Phase)
    {
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::PhaseStarted, static_cast<uint32_t>(Phase)});
    }

    void EndPhase(EBuildPhase Phase)
    {
        _OnBuildStatisticEvent.Broadcast({EBuildEvent::PhaseCompleted, static_cast<uint32_t>(Phase)});
    }

    using COnGlitchEvent = TEvent<const TSRef<SGlitch>&>;
    COnGlitchEvent::Registrar& OnGlitchEvent() { return _OnGlitchEvent; }

    using COnBuildStatisticEvent = TEvent<const SBuildEventInfo&>;
    COnBuildStatisticEvent::Registrar& OnBuildStatisticEvent() { return _OnBuildStatisticEvent; }

protected:
    // All the issues encountered across all the phases (Parser and SemanticAnalyzer)
    TSRefArray<SGlitch> _Glitches;
 
    SBuildStatistics _Statistics;

    COnGlitchEvent _OnGlitchEvent;
    COnBuildStatisticEvent _OnBuildStatisticEvent;
};
}
