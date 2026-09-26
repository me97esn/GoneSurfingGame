// See AboutPanel.h and specs/about-screen.md.

#include "AboutPanel.h"

#include "BackPill.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"

namespace
{
	// About_ prefix: duplicate anonymous-namespace names across .cpp files break the Android
	// unity build, which desktop builds never catch.
	//
	// The rack's palette, on purpose: this opens from the same hub over the same paused wave.
	const FLinearColor About_Scrim   (0.02f, 0.05f, 0.09f, 0.90f);
	const FLinearColor About_Ink     (1.00f, 1.00f, 1.00f, 0.95f);
	const FLinearColor About_Dim     (1.00f, 1.00f, 1.00f, 0.62f);
	const FLinearColor About_Faint   (1.00f, 1.00f, 1.00f, 0.40f);
	const FLinearColor About_Gold    (1.00f, 0.78f, 0.34f, 1.00f);
	const FLinearColor About_CardFill(0.055f, 0.085f, 0.130f, 0.94f);
	const FLinearColor About_CardEdge(1.000f, 1.000f, 1.000f, 0.20f);
	const FLinearColor About_Rule    (1.000f, 1.000f, 1.000f, 0.12f);

	// The block is designed at ONE size and scaled to fit, exactly as the rack is: desktop and phone
	// differ by ~2x in layout units and nothing on screen says which one you are on. These are the
	// mockup's numbers at the phone's 1200x540 logical size.
	constexpr float About_CardHeight = 452.0f;
	constexpr float About_Card1Width = 330.0f;
	constexpr float About_Card2Width = 350.0f;
	constexpr float About_Card3Width = 372.0f;
	constexpr float About_CardGap    = 14.0f;
	constexpr float About_PageWidth  = About_Card1Width + About_Card2Width + About_Card3Width + 2.0f * About_CardGap;

	FSlateFontInfo About_Bold(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo About_Regular(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	const FSlateBrush* About_CardBrush()
	{
		static const FSlateRoundedBoxBrush B(About_CardFill, 8.0f, About_CardEdge, 1.0f);
		return &B;
	}

	/** The rack's quiet button: keyline pill, white label. Both of this screen's buttons open a
	 *  text page, neither moves the player forward, so neither wears the accent. */
	const FButtonStyle& About_QuietStyle()
	{
		static const FButtonStyle Style = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.07f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.34f), 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.14f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.50f), 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.20f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.70f), 1.0f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
		return Style;
	}

	// ---- the words -------------------------------------------------------------------------

	/** Content/Legal/<Name>. Plain files, staged as UFS (DefaultGame.ini) so they reach the pak;
	 *  the board profiles and the intro traces travel the same way. */
	FString About_LoadLegalText(const TCHAR* Name)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Legal"), Name);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogSurf, Warning, TEXT("AboutPanel: could not read %s"), *Path);
			return FString::Printf(TEXT("(%s is missing from this build)"), Name);
		}
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		return Text;
	}

	struct FAboutRow     { FString Name; FString Detail; };
	struct FAboutSection { FString Title; TArray<FAboutRow> Rows; };
	struct FAboutCredits
	{
		FString Author;
		FString Contact;
		FString Blurb;
		TArray<FAboutSection> Sections;
	};

	/** Credits.txt: `key: value` lines for the game card, `[SECTION]` headers and `Name | Detail`
	 *  rows for the credits card. `\n` inside a detail is a line break - a music licence is four
	 *  lines by the artist's own request. `#` starts a comment. Nothing here is a format anyone
	 *  else reads, so it stays as small as the file it parses. */
	FAboutCredits About_ParseCredits(const FString& Text)
	{
		FAboutCredits Out;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))) { continue; }

			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				FAboutSection& S = Out.Sections.AddDefaulted_GetRef();
				S.Title = Line.Mid(1, Line.Len() - 2).TrimStartAndEnd();
				continue;
			}

			FString Name, Detail;
			if (Out.Sections.Num() > 0 && Line.Split(TEXT("|"), &Name, &Detail))
			{
				Detail.ReplaceInline(TEXT("\\n"), TEXT("\n"));
				Out.Sections.Last().Rows.Add({ Name.TrimStartAndEnd(), Detail.TrimStartAndEnd() });
				continue;
			}

			FString Key, Value;
			if (Line.Split(TEXT(":"), &Key, &Value))
			{
				Key.TrimStartAndEndInline(); Value.TrimStartAndEndInline();
				if      (Key == TEXT("author"))  { Out.Author  = Value; }
				else if (Key == TEXT("contact")) { Out.Contact = Value; }
				else if (Key == TEXT("blurb"))   { Out.Blurb   = Value; }
			}
		}
		return Out;
	}

	/** "1.0 · build 2026-09-17". The number is ProjectVersion in DefaultGame.ini; the date is the
	 *  day this file was last compiled, which a full package build makes the day of the build. */
	FString About_VersionLine()
	{
		FString Version;
		GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Version, GGameIni);
		if (Version.IsEmpty()) { Version = TEXT("dev"); }

		// __DATE__ is "Sep 17 2026"; the screen wants 2026-09-17.
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
										 TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		const FString Raw(TEXT(__DATE__));
		FString Iso = Raw;
		if (Raw.Len() == 11)
		{
			const FString Mon = Raw.Mid(0, 3);
			int32 M = 0;
			for (int32 i = 0; i < 12; ++i) { if (Mon == Months[i]) { M = i + 1; break; } }
			const FString Day = Raw.Mid(4, 2).Replace(TEXT(" "), TEXT("0"));
			Iso = FString::Printf(TEXT("%s-%02d-%s"), *Raw.Mid(7, 4), M, *Day);
		}
		return FString::Printf(TEXT("Version %s  ·  build %s"), *Version, *Iso);
	}

	/** The two lines the Unreal Engine EULA (section 7a) requires in a product's credits, verbatim
	 *  but for the year, which is the build year so it never has to be remembered. */
	FString About_EpicYear()
	{
		const FString Raw(TEXT(__DATE__));
		return Raw.Len() == 11 ? Raw.Mid(7, 4) : TEXT("2026");
	}
	const TCHAR* const About_EpicTrademarkLine =
		TEXT("Gone Surfing uses Unreal® Engine. Unreal® is a trademark or registered trademark of Epic Games, Inc. in the United States of America and elsewhere.");
	FString About_EpicCopyrightLine()
	{
		return FString::Printf(TEXT("Unreal® Engine, Copyright 1998 – %s, Epic Games, Inc. All rights reserved."), *About_EpicYear());
	}

	// ---- small builders ----------------------------------------------------------------------

	TSharedRef<SWidget> About_Label(const FString& Text)
	{
		return SNew(STextBlock).Text(FText::FromString(Text)).Font(About_Bold(11)).ColorAndOpacity(About_Faint);
	}

	TSharedRef<SWidget> About_Body(const FString& Text, int32 Size = 13, const FLinearColor& Color = About_Ink)
	{
		return SNew(STextBlock).Text(FText::FromString(Text)).Font(About_Regular(Size)).ColorAndOpacity(Color).AutoWrapText(true);
	}

	/** One credit row: the thing, then who and under what beneath it. Stacked, not two columns -
	 *  the music attribution is four lines by the artist's request, and right-aligned against a
	 *  name column it wrapped mid-word. A hairline above every row but the first, so a section
	 *  reads as a list and not a paragraph. */
	TSharedRef<SWidget> About_Row(const FAboutRow& Row, bool bFirst)
	{
		TSharedRef<SVerticalBox> V = SNew(SVerticalBox);
		if (!bFirst)
		{
			V->AddSlot().AutoHeight().Padding(0.0f, 6.0f)
			[
				SNew(SBox).HeightOverride(1.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(About_Rule)
				]
			];
		}
		V->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(FText::FromString(Row.Name)).Font(About_Bold(13)).ColorAndOpacity(About_Ink)
		];
		V->AddSlot().AutoHeight().Padding(0.0f, 1.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).Text(FText::FromString(Row.Detail)).Font(About_Regular(12)).ColorAndOpacity(About_Dim).AutoWrapText(true)
		];
		return V;
	}

	TSharedRef<SWidget> About_Card(float Width, TSharedRef<SWidget> Content)
	{
		return SNew(SBox).WidthOverride(Width).HeightOverride(About_CardHeight)
			[
				SNew(SBorder).BorderImage(About_CardBrush()).Padding(FMargin(16.0f, 14.0f))
				[
					Content
				]
			];
	}

	TSharedRef<SWidget> About_QuietButton(const FText& Label, FOnClicked OnClicked)
	{
		return SNew(SBox).HeightOverride(34.0f)
			[
				SNew(SButton)
				.ButtonStyle(&About_QuietStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center)
				.ContentPadding(FMargin(16.0f, 0.0f))
				.OnClicked(OnClicked)
				[
					SNew(STextBlock).Text(Label).Font(About_Bold(12)).ColorAndOpacity(About_Ink)
				]
			];
	}
}

class SAboutPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAboutPanel) {}
		SLATE_ARGUMENT(AboutPanel::FHooks, Hooks)
		SLATE_EVENT(FSimpleDelegate, OnClosed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Hooks    = InArgs._Hooks;
		OnClosed = InArgs._OnClosed;

		const FAboutCredits Credits = About_ParseCredits(About_LoadLegalText(TEXT("Credits.txt")));

		ChildSlot
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(About_Scrim)
			]

			+ SOverlay::Slot()
			.Padding(FMargin(20.0f, 12.0f))
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::Both)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						SAssignNew(TitleText, STextBlock)
						.Font(About_Bold(15))
						.ColorAndOpacity(About_Dim)
						.Text(NSLOCTEXT("GoneSurfing", "AboutTitle", "ABOUT"))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(Pages, SWidgetSwitcher)
						+ SWidgetSwitcher::Slot() [ BuildAboutPage(Credits) ]   // 0
						+ SWidgetSwitcher::Slot() [ BuildTextPage() ]           // 1
					]
				]
			]

			// The exit, in the one corner it lives in on every screen. Last slot, over the scrim.
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				BackPill::MakeAnchored([this]() { OnBackPressed(); })
			]
		];
	}

private:
	AboutPanel::FHooks Hooks;
	FSimpleDelegate OnClosed;

	TSharedPtr<STextBlock>      TitleText;
	TSharedPtr<SWidgetSwitcher> Pages;
	TSharedPtr<STextBlock>      PageText;
	TSharedPtr<SScrollBox>      PageScroll;

	// ---- page 0: the three cards ------------------------------------------------------------

	TSharedRef<SWidget> BuildAboutPage(const FAboutCredits& Credits)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				About_Card(About_Card1Width, BuildGameCard(Credits))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(About_CardGap, 0.0f)
			[
				About_Card(About_Card2Width, BuildCreditsCard(Credits))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				About_Card(About_Card3Width, BuildLegalCard())
			];
	}

	/** The game: wordmark, version, who, where to write - and the doors to the two text pages,
	 *  pinned to the card's foot so they sit in the same place whatever the blurb's length. */
	TSharedRef<SWidget> BuildGameCard(const FAboutCredits& Credits)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth() [ SNew(STextBlock).Text(FText::FromString(TEXT("GONE "))).Font(About_Bold(26)).ColorAndOpacity(About_Ink) ]
				+ SHorizontalBox::Slot().AutoWidth() [ SNew(STextBlock).Text(FText::FromString(TEXT("SURFING"))).Font(About_Bold(26)).ColorAndOpacity(About_Gold) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				About_Body(About_VersionLine(), 12, About_Dim)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 18.0f, 0.0f, 0.0f)
			[
				About_Body(Credits.Blurb)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				About_Body(Credits.Author.IsEmpty() ? FString() : FString::Printf(TEXT("Made in Sweden by %s."), *Credits.Author))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				About_Body(Credits.Contact.IsEmpty() ? FString() : FString::Printf(TEXT("Feedback and bug reports:\n%s"), *Credits.Contact), 12, About_Dim)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f) [ SNew(SBox) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					About_QuietButton(NSLOCTEXT("GoneSurfing", "AboutLicences", "THIRD-PARTY LICENCES"),
						FOnClicked::CreateSP(this, &SAboutPanel::OnOpenText, FString(TEXT("ThirdPartyNotices.txt")),
							NSLOCTEXT("GoneSurfing", "AboutLicencesTitle", "THIRD-PARTY LICENCES")))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					About_QuietButton(NSLOCTEXT("GoneSurfing", "AboutPrivacy", "PRIVACY"),
						FOnClicked::CreateSP(this, &SAboutPanel::OnOpenText, FString(TEXT("PrivacyAndTerms.txt")),
							NSLOCTEXT("GoneSurfing", "AboutPrivacyTitle", "PRIVACY & TERMS")))
				]
			];
	}

	/** The credits, section by section, straight from the file. */
	TSharedRef<SWidget> BuildCreditsCard(const FAboutCredits& Credits)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		bool bFirstSection = true;
		for (const FAboutSection& Section : Credits.Sections)
		{
			Box->AddSlot().AutoHeight().Padding(0.0f, bFirstSection ? 0.0f : 22.0f, 0.0f, 10.0f)
			[
				About_Label(Section.Title)
			];
			bFirstSection = false;
			bool bFirstRow = true;
			for (const FAboutRow& Row : Section.Rows)
			{
				Box->AddSlot().AutoHeight() [ About_Row(Row, bFirstRow) ];
				bFirstRow = false;
			}
		}
		Box->AddSlot().FillHeight(1.0f) [ SNew(SBox) ];
		Box->AddSlot().AutoHeight()
		[
			About_Body(TEXT("Something missing here? Write to us and we'll fix the credit."), 11, About_Faint)
		];
		return Box;
	}

	/** The legal card is the one that should never need touching again: Epic's two lines as the
	 *  EULA fixes them, and the data statement the store listing repeats. */
	TSharedRef<SWidget> BuildLegalCard()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f) [ About_Label(TEXT("BUILT WITH")) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Unreal® Engine"))).Font(About_Bold(15)).ColorAndOpacity(About_Ink)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f) [ About_Body(About_EpicTrademarkLine, 11, About_Dim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)  [ About_Body(About_EpicCopyrightLine(), 11, About_Dim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				About_Body(TEXT("Runs on a modified build of Unreal Engine 5.4, tuned for water physics."), 11, About_Dim)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 22.0f, 0.0f, 10.0f) [ About_Label(TEXT("YOUR DATA")) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				About_Body(TEXT("Gone Surfing runs entirely on your phone. It stores your best rides locally and sends nothing anywhere."), 11, About_Dim)
			];
	}

	// ---- page 1: a scrolling text ----------------------------------------------------------

	TSharedRef<SWidget> BuildTextPage()
	{
		return SNew(SBox).WidthOverride(About_PageWidth).HeightOverride(About_CardHeight)
			[
				SNew(SBorder).BorderImage(About_CardBrush()).Padding(FMargin(26.0f, 18.0f))
				[
					SAssignNew(PageScroll, SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(PageText, STextBlock)
						.Font(About_Regular(11))
						.ColorAndOpacity(About_Ink)
						.AutoWrapText(true)
					]
				]
			];
	}

	FReply OnOpenText(FString FileName, FText Title)
	{
		PageText->SetText(FText::FromString(About_LoadLegalText(*FileName)));
		PageScroll->ScrollToStart();
		TitleText->SetText(Title);
		Pages->SetActiveWidgetIndex(1);
		return FReply::Handled();
	}

	/** BACK steps out one level: a text page returns to the cards, the cards close the screen.
	 *  Two levels, one corner - the pill never moves (BackPill.h). */
	void OnBackPressed()
	{
		if (Pages->GetActiveWidgetIndex() == 1)
		{
			// Drop the big text so a 70 KB licence list is not laid out again behind the cards.
			PageText->SetText(FText::GetEmpty());
			TitleText->SetText(NSLOCTEXT("GoneSurfing", "AboutTitle", "ABOUT"));
			Pages->SetActiveWidgetIndex(0);
			return;
		}
		if (Hooks.OnClosed) { Hooks.OnClosed(); }
		OnClosed.ExecuteIfBound();
	}
};

// ---- install / open / close --------------------------------------------------------------------
namespace
{
	struct FAboutState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SAboutPanel> Widget;
		bool bWePaused = false;
	};
	TMap<UWorld*, TSharedPtr<FAboutState>> GAboutStates;

	void About_Destroy(UWorld* World)
	{
		TSharedPtr<FAboutState> State;
		if (!GAboutStates.RemoveAndCopyValue(World, State) || !State.IsValid()) { return; }
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
		// Only undo a pause we caused. The hub pauses too, and this opens from it.
		if (State->bWePaused && World)
		{
			UGameplayStatics::SetGamePaused(World, false);
		}
	}
}

namespace AboutPanel
{
	void Open(UWorld* World, const FHooks& Hooks)
	{
		if (!World) { return; }

		// A stale entry can survive a PIE session (the map is keyed on a raw UWorld* and PIE reuses
		// those pointers); left alone, IsOpen() reads true for ever. Same guard as the rack.
		if (TSharedPtr<FAboutState>* Existing = GAboutStates.Find(World))
		{
			if (Existing->IsValid() && (*Existing)->Widget.IsValid())
			{
				return;
			}
			GAboutStates.Remove(World);
		}

		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			UE_LOG(LogSurf, Warning, TEXT("AboutPanel: no game viewport; cannot open"));
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FAboutState> State = MakeShared<FAboutState>();
		TSharedPtr<SAboutPanel> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SAboutPanel)
				.Hooks(Hooks)
				.OnClosed(FSimpleDelegate::CreateLambda([World]() { About_Destroy(World); }))
			];
		State->Root = Root;
		State->Widget = Widget;
		State->bWePaused = !UGameplayStatics::IsGamePaused(World);
		if (State->bWePaused)
		{
			UGameplayStatics::SetGamePaused(World, true);
		}

		// ZOrder 260: the modal tier, with the rack and the ride list. Never up together.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 260);
		GAboutStates.Add(World, State);
		UE_LOG(LogSurf, Display, TEXT("AboutPanel: opened"));
	}

	bool IsOpen(UWorld* World)
	{
		if (!World) { return false; }
		const TSharedPtr<FAboutState>* Found = GAboutStates.Find(World);
		return Found && Found->IsValid() && (*Found)->Widget.IsValid();
	}

	void Close(UWorld* World)     { About_Destroy(World); }
	void Uninstall(UWorld* World) { About_Destroy(World); }
}
