// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class GoneSurfingEditorTarget : TargetRules
{
	public GoneSurfingEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("GoneSurfing");

		// Workaround for MSVC 14.42 warnings in engine headers (C4668, C4067)
		bWarningsAsErrors = false;
		bUseUnityBuild = false;
	}
}
