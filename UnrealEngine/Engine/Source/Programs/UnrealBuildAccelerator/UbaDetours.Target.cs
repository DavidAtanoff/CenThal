// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

[SupportedPlatforms(UnrealPlatformClass.Desktop)]
public class UbaDetoursTarget : TargetRules
{
	public UbaDetoursTarget(TargetInfo Target) : base(Target)
	{
		LaunchModuleName = "UbaDetours";
		bShouldCompileAsDLL = true;
		UbaAgentTarget.CommonUbaSettings(this, Target);
		GlobalDefinitions.Add("_CRT_SECURE_CPP_OVERLOAD_SECURE_NAMES=0");

		// For some reason xcode 26.2.0+ produce a bad dylib that fails to load when ltcg is enabled
		if (Target.Platform.IsInGroup(UnrealPlatformGroup.Apple))
		{
			bAllowLTCG = false;
		}

	}
}
