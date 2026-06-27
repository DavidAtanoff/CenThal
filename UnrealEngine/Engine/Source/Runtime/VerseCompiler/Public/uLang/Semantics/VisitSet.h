// Copyright Epic Games, Inc. All Rights Reserved.
// uLang Compiler Public API

#pragma once

#include "uLang/Common/Common.h"
#include "uLang/Common/Containers/Set.h"

namespace uLang
{
class CVisitKey {};

class CVisitSet
{
public:
    bool TryVisit(const CVisitKey& Key)
    {
        if (!Set.Find(&Key))
        {
            Set.Insert(&Key);
            return true;
        }
        return false;
    }
private:
    TSet<const CVisitKey*> Set;
};
}