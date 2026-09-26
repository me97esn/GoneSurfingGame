// The ride score counter. See specs/ride-score-counter.md.
//
// Two gaps in the gradual control handoff, closed by one number:
//
//   - Nothing told the player whether they were surfing WELL. The assist stepped down and they had
//     no idea what earned it. A number on the level-down card does not fix that: it explains the
//     threshold, not the behaviour, and it arrives too late to act on.
//   - Nothing asked the player to keep riding. You try the game once and there is no second wave.
//
// The feedback signal was already being computed and thrown away. SurfAssist accrues credit at 1.0x
// while the steering guard is silent and 0.25x while it is correcting - a per-tick verdict on "is
// this player holding the line unaided" that disappeared into a float nobody saw. This overlay is
// that float, made visible, ticking visibly faster when the player has it and crawling when they do
// not.
//
// THE COUNTER IS THE CREDIT (FR1). Not a score beside it. A parallel arcade score re-creates the
// exact problem the feature exists to fix - two numbers that disagree about why the board changed,
// and a step-down that is unexplained again by a different route.
//
// The player sees POINTS; the code keeps credit (D4). Seconds appear nowhere player-facing.
//
// Placement is top-RIGHT. It was top-centre while the old Restart/Replay bar lived along the bottom;
// when the bar moved to the top the counter was pushed down to clear it and landed on the surfer,
// who the camera keeps dead centre. A corner is where the board never is. It held the top-left
// until the ride's Back button took that corner (specs/two-screen-navigation.md FR2).
#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace RideScore
{
	/** Show (installing on first call) the counter. Idempotent - safe to call every tick.
	 *
	 *  @param Score        points for THIS ride (FR3 - the number with a stake, reset by a fall)
	 *  @param bEarningFast true while the steering guard is silent. Drives a colour change, because
	 *                      a change of tick RATE alone is too subtle to read in peripheral vision
	 *                      while the player is watching a wave (FR2).
	 *  @param BestScore    best single ride so far, points. Shown only when bRideOver.
	 *  @param bRideOver    the ride has ended: stop implying the number is still climbing, and
	 *                      reveal the best to beat. This is FR4's "after a fall" surface - the
	 *                      counter is already on screen, so it costs no new widget and no modal. */
	void Show(UWorld* World, int32 Score, bool bEarningFast, int32 BestScore, bool bRideOver);

	/** A scored turn: the number snaps and settles. A BIG one adds the burst. No words - see below.
	 *
	 *  This is the assist's old level-up celebration, reclaimed. Its trigger died with graduation
	 *  (a board's assist level is fixed now, so nothing ever steps down) and RideScore::PlayLevelUp
	 *  went with it in 74fac206c - but the animation itself was deliberately left standing, and it
	 *  carries four hard-won lessons worth keeping: a fast snap with a long settle rather than a
	 *  symmetric swell, more than one element moving, a pivot below centre so the peak does not crop
	 *  off the top of the screen, and a burst painted as radial shards rather than a scaled
	 *  rectangle.
	 *
	 *  It took a caption once ("BIG TURN", "AGAIN!", "LINKED!") and the counter wore a x2 / x3 chain
	 *  badge. Both were cut on the first device session, and the reason is the design rule for this
	 *  whole surface: text takes the eyes off the wave and needs decoding, and a player who has just
	 *  made a good turn already knows it. What they need is the number visibly reacting - that the
	 *  game noticed - not to be told by how much or why. So: every scored turn moves the number;
	 *  only a big one bursts, because the burst covers the wave for a moment and would be wallpaper
	 *  on every turn. */
	void PlayCelebration(UWorld* World, bool bBigTurn);

	/** Hide (start screen, assist disabled, every automated run). Does not install. */
	void Hide(UWorld* World);

	/** Remove entirely. Safe when not installed. */
	void Uninstall(UWorld* World);
}
