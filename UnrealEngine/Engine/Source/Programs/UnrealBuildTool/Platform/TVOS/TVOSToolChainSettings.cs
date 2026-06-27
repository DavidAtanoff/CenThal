// Copyright Epic Games, Inc. All Rights Reserved.

using Microsoft.Extensions.Logging;

namespace UnrealBuildTool
{
	class TVOSToolChainSettings : IOSToolChainSettings
	{
		public TVOSToolChainSettings(IOSProjectSettings ProjectSettings, ILogger Logger) 
			: base("AppleTVOS", "AppleTVSimulator", "tvos", ProjectSettings, Logger)
		{
		}
	}
}
