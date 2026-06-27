// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/CoreReflectedTypeAccessors.h"

class UClass;
class UScriptStruct;

/*-----------------------------------------------------------------------------
C++ templated Static(Class/Struct) retrieval function prototypes.
-----------------------------------------------------------------------------*/

template<typename ClassType> inline UClass* StaticClass()
{
	return ClassType::StaticClass();
}

template<typename StructType> inline UScriptStruct* StaticStruct()
{
	return StructType::StaticStruct();
}
