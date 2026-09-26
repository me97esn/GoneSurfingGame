// On-screen touch controls: a floating-origin virtual joystick for weight shift and a
// hold-to-pump button. Replaces phone tilt and the shake-to-pump gesture; both motion schemes
// failed for the same reason — the player cannot watch the wave and perform the gesture at the
// same time. Pure C++ Slate, consistent with the other overlays in this module.
//
// The overlay produces input only. Everything it collects is read by ASurfboardPawn::Tick and
// written to the same values the tilt and stick paths write, so no physics path knows the
// difference. See specs/pump-button-and-virtual-stick.md.
#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace TouchControls
{
	/** What the player's thumbs are doing this frame. */
	struct FState
	{
		/** Weight offset in [-1,+1] per axis: X = sideways, Y = fore/aft. Same units as the
		 *  gamepad stick path, so everything downstream is unchanged. */
		FVector2D WeightOffset = FVector2D::ZeroVector;

		/** True while a thumb is down inside the joystick zone. Suppresses auto-centring, exactly
		 *  as holding the gamepad stick does. */
		bool bStickHeld = false;

		/** True while the pump button is held. ORed with ASurfboardPawn::SetPumpHeld, so a UMG
		 *  button can coexist with this one during a transition. */
		bool bPumpHeld = false;
		/** No thumb down, but the last drag's command is being held (StickLatch). WeightOffset
		 *  carries it; the pawn must not auto-centre while this is set. */
		bool bStickLatched = false;
	};

	/** Tunables pushed in from USurfTuningSubsystem each tick, so the HUD's sliders are live. */
	struct FTuning
	{
		float FullDeflectionPx    = 90.0f;   /**< Travel from the origin for full deflection. */
		float DeadzoneFrac        = 0.08f;   /**< Fraction of full deflection ignored around origin. */
		float ForeAftReturnSecs   = 0.0f;    /**< Fore/aft self-centring time constant; 0 = hold trim (default). */
		bool  bMirror             = false;   /**< Swap the two sides for left-handed players. */
		float StickRadiusFrac     = 0.25f;   /**< Drawn stick radius, as a fraction of screen height. */
		float PumpRadiusFrac      = 0.25f;   /**< Drawn pump radius, same units. */
		float HomeYFrac           = 0.5f;    /**< Height of both centres, as a fraction down the screen. */
		bool  bLatch              = true;    /**< Keep the command (both axes) after release; a tap without a drag re-centres. */
		bool  bPumpGhost          = true;    /**< Pump button in the stick's ghost tier (default) instead of coral. */
		int32 Hint                = 2;       /**< Caption placement: 0 none; pump 1 in the disc, 2 below it, 3 over a faded glyph. The stick's caption shows for any non-zero. */
		float RestArt             = 0.22f;   /**< SetDimmed: opacity of the instrument (rings, knob, glyph) before the handoff. */
		float RestText            = 0.85f;   /**< SetDimmed: opacity of the captions before the handoff - kept high; they are the instruction. */
	};

	/** Add the touch controls to the world's game viewport. No-op if already installed. */
	GONESURFING_API void Install(UWorld* World);

	/** Remove them. Safe to call when not installed. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** True while the widget is up. */
	GONESURFING_API bool IsInstalled(UWorld* World);

	/** Push live tunables. Cheap; call per tick. */
	GONESURFING_API void SetTuning(UWorld* World, const FTuning& Tuning);

	/** Whether the controls are live this tick. Inactive means they draw nothing and the pawn
	 *  ignores them - but they stay INSTALLED and their pads keep tracking fingers.
	 *
	 *  That distinction matters: a touch that begins during the intro and is still held when control
	 *  is handed over must start steering at the handoff. Destroying the widget for the intro and
	 *  rebuilding it afterwards loses the in-flight touch entirely, because a finger that is already
	 *  down generates no new touch-started event - which is why the stick used to need releasing and
	 *  pressing again before it did anything. */
	GONESURFING_API void SetActive(UWorld* World, bool bActive);

	/** Ride over, or not yet begun: keep drawing the controls at rest - rings, glyph, captions -
	 *  but take no input and report none. The captions ("SIDEWAYS TO TURN", "PRESS & RELEASE TO
	 *  SPEED UP") are the one piece of instruction in the game, and nobody reads them while
	 *  surfing; the end card and the hub are the two moments the player has time to, and pulling
	 *  the controls the instant the ride ended (or only raising them at the handoff) took the
	 *  words away from both. Under the end card its scrim dims them; on the hub SetDimmed does.
	 *  While inert the pads are hit-test invisible, so a tap in their zones reaches the screen
	 *  beneath (the hub's REPLAY and ABOUT sit inside them). Clears any held or latched command
	 *  on entry. */
	GONESURFING_API void SetInert(UWorld* World, bool bInert);

	/** Draw the controls as "not yet": the INSTRUMENT (rings, knob, glyph) drops far enough to read
	 *  as a drawing of itself, while the CAPTIONS stay near full - on the hub the words are the
	 *  whole point of showing the controls at all. Used on the hub and through the intro until the
	 *  handoff, where the rings coming up solid is the signal that control has arrived (with the
	 *  "...and surf!" cue) and the words carry across the cut unchanged. Fading everything evenly
	 *  instead was tried first and was not a signal at all: a ghost control at half strength still
	 *  looks like a ghost control. Not used under the end card, whose scrim dims the lot. See
	 *  specs/two-screen-navigation.md FR1a. */
	GONESURFING_API void SetDimmed(UWorld* World, bool bDimmed);

	/** Read this frame's thumb state. Returns a zeroed state when not installed. */
	GONESURFING_API FState GetState(UWorld* World);

	/** Per game tick: advance the fore/aft self-centring, and reconcile the pads with the fingers
	 *  actually on the glass - a thumb that was already down when the pads appeared (resting on the
	 *  start screen, or landed during the first frame after Start) never sends a touch-started event,
	 *  so the pads adopt it here instead of waiting for it to be lifted and pressed again. Driven from
	 *  the pawn's Tick so it runs on the game's clock rather than Slate's, and stops dead when the
	 *  game is paused. */
	GONESURFING_API void Advance(UWorld* World, float DeltaTime);
}
