// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "interfaces/ToolMode.h"

namespace BuildPatchTool
{
	class FMockToolMode
		: public IToolMode
	{
	public:

		virtual ~FMockToolMode() {}

		virtual EReturnCode Execute() override
		{
			return EReturnCode::OK;
		}
	};
}
