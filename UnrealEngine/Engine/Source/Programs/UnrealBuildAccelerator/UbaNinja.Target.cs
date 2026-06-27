// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

[SupportedPlatforms(UnrealPlatformClass.Desktop)]
public class UbaNinjaTarget : TargetRules
{
	public UbaNinjaTarget(TargetInfo Target) : base(Target)
	{
		LaunchModuleName = "UbaNinja";
		UbaAgentTarget.CommonUbaSettings(this, Target);
	}
}
