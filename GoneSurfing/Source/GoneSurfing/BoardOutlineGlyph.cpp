// See BoardOutlineGlyph.h and specs/board-selection.md D6.

#include "BoardOutlineGlyph.h"
#include "SurfBoards.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"

// The artist's outlines, generated from images/"surfboards outline.svg" by Tools/svg2boards.py.
// Every board in here shares one scale, which is what makes the rack's size comparison true.
#include "BoardOutlineShapes.inc"

namespace
{
	// Glyph_ prefix: duplicate anonymous-namespace names across .cpp files break the Android unity
	// build, which desktop builds never catch.
	const FBoardShapeDef* Glyph_FindShape(const FString& ShapeId)
	{
		if (ShapeId.IsEmpty())
		{
			return nullptr;
		}
		for (const FBoardShapeDef& Def : GBoardShapes)
		{
			if (ShapeId.Equals(Def.Id, ESearchCase::IgnoreCase))
			{
				return &Def;
			}
		}
		return nullptr;
	}
}

namespace BoardOutline
{
	bool GetShape(const FString& ShapeId, TArray<TArray<FVector2f>>& OutStrokes,
		TArray<float>& OutWeights)
	{
		OutStrokes.Reset();
		OutWeights.Reset();
		const FBoardShapeDef* Def = Glyph_FindShape(ShapeId);
		if (!Def || !Def->Build)
		{
			return false;
		}
		Def->Build(OutStrokes, OutWeights);
		return true;
	}

	bool GetExtent(const FString& ShapeId, float& OutLength, float& OutWidth)
	{
		const FBoardShapeDef* Def = Glyph_FindShape(ShapeId);
		if (!Def)
		{
			OutLength = OutWidth = 0.0f;
			return false;
		}
		OutLength = Def->Length;
		OutWidth = Def->Width;
		return true;
	}

	void GetAllShapeIds(TArray<FString>& OutIds)
	{
		OutIds.Reset();
		for (const FBoardShapeDef& Def : GBoardShapes)
		{
			OutIds.Add(FString(Def.Id));
		}
	}
}

void SBoardOutlineGlyph::Construct(const FArguments& InArgs)
{
	ShapeId       = InArgs._ShapeId;
	LineColor     = InArgs._LineColor;
	Thickness     = InArgs._Thickness;
	PixelsPerUnit = InArgs._PixelsPerUnit;
	Facing        = InArgs._Facing;
	OnClicked     = InArgs._OnClicked;
	SetCanTick(false);

	// Only take hit-testing when there is something to click. Left hit-testable, the picker's large
	// decorative copies would swallow drags aimed at the panel behind them.
	SetVisibility(OnClicked.IsBound() ? EVisibility::Visible : EVisibility::HitTestInvisible);
}

FVector2D SBoardOutlineGlyph::ComputeDesiredSize(float) const
{
	float L = 0.0f, W = 0.0f;
	if (!BoardOutline::GetExtent(ShapeId.Get(FString()), L, W))
	{
		return FVector2D(40.0f, 100.0f);
	}
	const float PPU = PixelsPerUnit.Get(0.0f);
	const bool bUp = (Facing.Get(EBoardGlyphFacing::NoseUp) == EBoardGlyphFacing::NoseUp);
	if (PPU <= 0.0f)
	{
		// Fit-to-box: the parent decides, so ask only for a sane aspect at a nominal size.
		const float Nominal = 90.0f;
		return bUp
			? FVector2D(Nominal * (W / L), Nominal)
			: FVector2D(Nominal, Nominal * (W / L));
	}
	// Pinned scale: ask for exactly this board's size, so a row of glyphs laid out bottom-aligned
	// shows their true relative lengths with no further arithmetic. Laying one board down does not
	// change PPU, so a landscape glyph's WIDTH is still its true length on the shared scale.
	return bUp
		? FVector2D(W * PPU, L * PPU)
		: FVector2D(L * PPU, W * PPU);
}

int32 SBoardOutlineGlyph::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	TArray<TArray<FVector2f>> Strokes;
	TArray<float> Weights;
	if (!BoardOutline::GetShape(ShapeId.Get(FString()), Strokes, Weights) || Strokes.Num() == 0)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogTemp, Warning, TEXT("BoardOutlineGlyph: no shape for id '%s'"), *ShapeId.Get(FString()));
		}
		return LayerId;
	}

	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X <= 1.0 || Size.Y <= 1.0)
	{
		return LayerId;
	}

	float L = 0.0f, W = 0.0f;
	BoardOutline::GetExtent(ShapeId.Get(FString()), L, W);
	if (L <= KINDA_SMALL_NUMBER)
	{
		return LayerId;
	}

	const bool bUp = (Facing.Get(EBoardGlyphFacing::NoseUp) == EBoardGlyphFacing::NoseUp);
	float Scale = PixelsPerUnit.Get(0.0f);
	if (Scale <= 0.0f)
	{
		// Fit the box on both axes, so a button never clips its own nose.
		const float AlongPx  = (float)(bUp ? Size.Y : Size.X);
		const float AcrossPx = (float)(bUp ? Size.X : Size.Y);
		Scale = FMath::Min(AlongPx / L, AcrossPx / FMath::Max(W, KINDA_SMALL_NUMBER)) * 0.96f;
	}

	const FVector2f Centre((float)Size.X * 0.5f, (float)Size.Y * 0.5f);
	const FLinearColor Ink = LineColor.Get(FLinearColor::White);
	const float Stroke = FMath::Max(1.0f, Thickness.Get(1.5f));

	TArray<FVector2D> Pts;
	for (int32 si = 0; si < Strokes.Num(); ++si)
	{
		const TArray<FVector2f>& Stroke2D = Strokes[si];
		if (Stroke2D.Num() < 2)
		{
			continue;
		}
		// Per-stroke weight, straight from the drawing: the outline is 1.0 and the stringer about a
		// third, so it reads as an inlay rather than a second rail. Floored at 1px because Slate will
		// simply not draw a sub-pixel line, which would lose the stringers entirely on the button.
		const float StrokeWeight = Weights.IsValidIndex(si) ? Weights[si] : 1.0f;
		const float StrokePx = FMath::Max(1.0f, Stroke * StrokeWeight);

		Pts.Reset(Stroke2D.Num());
		for (const FVector2f& P : Stroke2D)
		{
			// Shape space is x along the board (+x = nose), y across it.
			const FVector2f S = bUp
				? FVector2f(Centre.X + P.Y * Scale, Centre.Y - P.X * Scale)   // nose up
				: FVector2f(Centre.X + P.X * Scale, Centre.Y + P.Y * Scale);  // nose right
			Pts.Add(FVector2D(S.X, S.Y));
		}
		// Weight 0 = the drawing FILLED this shape rather than stroking it (the foamie's fin screws
		// and leash plug). Outlining them left a heavy ring, because a 1px stroke - Slate's minimum -
		// around a 3px dot is mostly stroke. Slate's line renderer cannot fill, so fill it with a
		// fully-rounded box sized to the shape's own bounds, which for a small circle IS a dot.
		if (StrokeWeight <= 0.0f)
		{
			FVector2D Min(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
			FVector2D Max(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());
			for (const FVector2D& Q : Pts)
			{
				Min.X = FMath::Min(Min.X, Q.X); Min.Y = FMath::Min(Min.Y, Q.Y);
				Max.X = FMath::Max(Max.X, Q.X); Max.Y = FMath::Max(Max.Y, Q.Y);
			}
			const FVector2D DotSize(FMath::Max(1.0, Max.X - Min.X), FMath::Max(1.0, Max.Y - Min.Y));
			// Radius 2, not 8: a corner radius larger than the box draws NOTHING at all (the same way
			// a 999 radius silently erased the board button's pill). A fin screw is 3-4px across, so
			// 2 is already fully round and cannot exceed the shape at any size this is drawn.
			static const FSlateRoundedBoxBrush DotBrush(FLinearColor::White, 2.0f);
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(DotSize, FSlateLayoutTransform(Min)),
				&DotBrush, ESlateDrawEffect::None, Ink);
			continue;
		}

		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			Pts, ESlateDrawEffect::None, Ink, /*bAntialias*/ true, StrokePx);
	}

	return LayerId + 1;
}

FReply SBoardOutlineGlyph::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (!OnClicked.IsBound() || Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bPressed = true;
	// Capture, or the release lands on whatever the finger drifted over and the button stays stuck
	// down. Capturing also means a press that slides off and lets go is correctly not a click.
	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply SBoardOutlineGlyph::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (!bPressed || Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bPressed = false;

	// Fire only if the release happened over the glyph - the standard let-go-to-cancel gesture,
	// which matters more on a phone where a tap and a scroll start identically.
	const bool bInside = Geometry.IsUnderLocation(Event.GetScreenSpacePosition());
	FReply Reply = FReply::Handled().ReleaseMouseCapture();
	if (bInside)
	{
		OnClicked.ExecuteIfBound();
	}
	return Reply;
}

void SBoardOutlineGlyph::OnMouseEnter(const FGeometry& Geometry, const FPointerEvent& Event)
{
	SLeafWidget::OnMouseEnter(Geometry, Event);
	bHovered = true;
}

void SBoardOutlineGlyph::OnMouseLeave(const FPointerEvent& Event)
{
	SLeafWidget::OnMouseLeave(Event);
	bHovered = false;
	// Deliberately not clearing bPressed: with capture held, sliding off and back on should still
	// read as pressed, the same as every other button.
}

// ---- UMG wrapper -------------------------------------------------------------------------------

FString UBoardOutlineWidget::ResolveShapeId() const
{
	if (bShowActiveBoard)
	{
		if (const USurfBoardSubsystem* Boards = SurfBoards::Get(this))
		{
			const FString Id = Boards->GetOutlineShapeId(Boards->GetActiveIndex());
			if (!Id.IsEmpty())
			{
				return Id;
			}
		}
		else
		{
			// Expected in the UMG designer, which has no game instance. NOT expected in play: it
			// would mean this widget cannot reach the world, and the icon would sit on ShapeId's
			// default for ever while looking deliberate. Say so once rather than let it pass.
			static bool bWarnedNoSubsystem = false;
			if (!bWarnedNoSubsystem && GetWorld() && GetWorld()->IsGameWorld())
			{
				bWarnedNoSubsystem = true;
				UE_LOG(LogTemp, Warning,
					TEXT("BoardOutlineWidget: bShowActiveBoard is set but no board subsystem is reachable "
						 "from this widget - the icon is falling back to ShapeId '%s'."), *ShapeId);
			}
		}
		return ShapeId;   // designer preview: no game instance to ask
	}
	if (BoardIndex >= 0)
	{
		if (const USurfBoardSubsystem* Boards = SurfBoards::Get(this))
		{
			if (Boards->GetProfile(BoardIndex))
			{
				const FString Id = Boards->GetOutlineShapeId(BoardIndex);
				if (!Id.IsEmpty())
				{
					return Id;
				}
			}
		}
	}
	// No game instance (the UMG designer) or no such profile: the authored shape.
	return ShapeId;
}

bool UBoardOutlineWidget::ResolveSelected() const
{
	if (bFollowActiveBoard && BoardIndex >= 0)
	{
		if (const USurfBoardSubsystem* Boards = SurfBoards::Get(this))
		{
			return Boards->GetActiveIndex() == BoardIndex;
		}
	}
	return bSelected;
}

FLinearColor UBoardOutlineWidget::ResolveColor() const
{
	// Press beats hover beats selection. Pressed is the only state a phone can show, so it must
	// never be masked by the board happening to be the selected one.
	if (Glyph.IsValid() && Glyph->IsPressed())    { return PressedColor; }
	if (Glyph.IsValid() && Glyph->IsHoveredNow()) { return HoveredColor; }
	return ResolveSelected() ? SelectedColor : LineColor;
}

float UBoardOutlineWidget::ResolveThickness() const
{
	if (Glyph.IsValid() && Glyph->IsPressed()) { return Thickness * PressedThicknessScale; }
	return ResolveSelected() ? Thickness * SelectedThicknessScale : Thickness;
}

TSharedRef<SWidget> UBoardOutlineWidget::RebuildWidget()
{
	// Attributes, not values: the active board can change while the control is up, and the glyph has
	// to follow without anyone remembering to refresh it.
	//
	// Weak, not raw `this`: the Slate widget can outlive a UMG rebuild, and a stale captured pointer
	// would be a crash on the next paint rather than a missing outline.
	TWeakObjectPtr<UBoardOutlineWidget> WeakThis(this);

	Glyph = SNew(SBoardOutlineGlyph)
		.Facing(Facing)
		.ShapeId(TAttribute<FString>::CreateLambda([WeakThis]()
		{
			const UBoardOutlineWidget* W = WeakThis.Get();
			return W ? W->ResolveShapeId() : FString();
		}))
		.LineColor(TAttribute<FLinearColor>::CreateLambda([WeakThis]()
		{
			const UBoardOutlineWidget* W = WeakThis.Get();
			return W ? W->ResolveColor() : FLinearColor::White;
		}))
		.Thickness(TAttribute<float>::CreateLambda([WeakThis]()
		{
			const UBoardOutlineWidget* W = WeakThis.Get();
			return W ? W->ResolveThickness() : 1.5f;
		}))
		.OnClicked(OnClicked.IsBound()
			? FSimpleDelegate::CreateLambda([WeakThis]()
				{
					if (UBoardOutlineWidget* W = WeakThis.Get()) { W->OnClicked.Broadcast(); }
				})
			: FSimpleDelegate());

	return Glyph.ToSharedRef();
}

void UBoardOutlineWidget::SynchronizeProperties()
{
	Super::SynchronizeProperties();
	// Everything drawn is bound as an attribute, so there is nothing to push - but invalidate so a
	// designer-time property edit repaints.
	if (Glyph.IsValid())
	{
		Glyph->Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
	}
}

void UBoardOutlineWidget::SetBoardIndex(int32 InIndex)
{
	BoardIndex = InIndex;
	if (Glyph.IsValid())
	{
		Glyph->Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
	}
}

void UBoardOutlineWidget::SetSelected(bool bInSelected)
{
	bSelected = bInSelected;
	if (Glyph.IsValid())
	{
		Glyph->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void UBoardOutlineWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Glyph.Reset();
}

#if WITH_EDITOR
const FText UBoardOutlineWidget::GetPaletteCategory()
{
	return NSLOCTEXT("GoneSurfing", "SurfPalette", "Gone Surfing");
}
#endif
