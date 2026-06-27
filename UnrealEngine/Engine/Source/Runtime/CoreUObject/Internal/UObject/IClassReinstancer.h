// Copyright Epic Games, Inc. All Rights Reserved.

#include "Templates/UniquePtr.h"
#include "Containers/Map.h"

namespace UE::Private
{
	struct IClassReinstancer
	{
		virtual ~IClassReinstancer() = default;
		virtual void DoReinstance(const TMap<UClass*, UClass*>& OldClassToNewClass) = 0;
	};

	COREUOBJECT_API TUniquePtr<IClassReinstancer> CreateClassReinstancer(TArray<UClass*>& InOutClasses);
	
	typedef TUniquePtr<IClassReinstancer> (*FReinstanceClassesPrepassFPtr)(TArray<UClass*>&);
	extern COREUOBJECT_API FReinstanceClassesPrepassFPtr GCreateClassReinstancerFPtr;
}