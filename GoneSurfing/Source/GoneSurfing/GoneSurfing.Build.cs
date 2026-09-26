// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GoneSurfing : ModuleRules
{
	public GoneSurfing(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Workaround for MSVC 14.42 compatibility warnings in engine headers
		bEnableUndefinedIdentifierWarnings = false;

		// Disable specific MSVC warnings that appear with 14.42 toolchain
		// C4668: '__has_feature' is not defined as a preprocessor macro
		// C4067: unexpected tokens following preprocessor directive
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			bEnableExceptions = false;  // Disable exceptions if not needed
		}

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Niagara", "NiagaraCore", "Json", "JsonUtilities", "Slate", "SlateCore", "UMG", "HTTP" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// Editor-only dependencies
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Android: apply the UPL that repoints the app icon at the adaptive mipmap icons
		// (see GoneSurfing_UPL.xml + Build/Android/res). Only affects Android packaging.
		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			AdditionalPropertiesForReceipt.Add("AndroidPlugin",
				System.IO.Path.Combine(ModuleDirectory, "GoneSurfing_UPL.xml"));
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
