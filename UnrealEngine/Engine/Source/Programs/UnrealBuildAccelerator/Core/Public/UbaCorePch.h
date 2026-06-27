// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if UBA_USE_MIMALLOC
#include <mimalloc.h>
#include <mimalloc-override.h>
#if PLATFORM_LINUX
namespace std { inline void* mi_realloc(void* p, size_t newsize) { return ::mi_realloc(p, newsize); } }
#endif
#endif

#include "UbaPlatform.h"
