// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// Project-wide log category for GoneSurfing diagnostics.
//
// In Shipping builds the compile-time verbosity is capped at Error, so every
// UE_LOG(LogSurf, Warning/Log/Display/Verbose, ...) site compiles to nothing —
// a single off-switch that strips the project's dev logging from the shipped
// binary while leaving genuine errors intact. In non-Shipping builds all
// verbosities are compiled in and default to Log at runtime.
//
// See RELEASE_CHECKLIST.md ("Gate debug logging out of Shipping").
#if UE_BUILD_SHIPPING
DECLARE_LOG_CATEGORY_EXTERN(LogSurf, Error, Error);
#else
DECLARE_LOG_CATEGORY_EXTERN(LogSurf, Log, All);
#endif
