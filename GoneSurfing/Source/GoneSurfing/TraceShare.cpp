// See TraceShare.h and specs/share-trace-from-phone.md.

#include "TraceShare.h"
#include "SurfLog.h"
#include "SurfboardPawn.h"

#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

// The destination is a config value, not a tunable: it never changes mid-ride and the tuning stack
// is floats. A string CVar so the same value can be set from DefaultEngine.ini for the device build
// and overridden from -ExecCmds on the desktop.
static TAutoConsoleVariable<FString> CVarTraceShareUrl(
	TEXT("surf.traceshare.url"), TEXT(""),
	TEXT("Where the SEND panel posts ride traces, e.g. http://100.64.0.2:8765/trace - the PC's Tailscale\n")
	TEXT("address and the port Tools/TraceShareServer.py listens on. Empty = sending disabled."),
	ECVF_Default);

namespace
{
	// TraceShare_ prefix: duplicate anonymous-namespace names across .cpp files break the Android
	// unity build, which desktop builds never catch.
	const FLinearColor TraceShare_Ink  (1.00f, 1.00f, 1.00f, 0.95f);
	const FLinearColor TraceShare_Dim  (1.00f, 1.00f, 1.00f, 0.62f);
	const FLinearColor TraceShare_Faint(1.00f, 1.00f, 1.00f, 0.35f);
	const FLinearColor TraceShare_Lit  (0.42f, 0.78f, 1.00f, 0.98f);
	const FLinearColor TraceShare_Good (0.45f, 0.90f, 0.55f, 0.98f);
	const FLinearColor TraceShare_Bad  (1.00f, 0.45f, 0.40f, 0.98f);

	FSlateFontInfo TraceShare_Bold(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo TraceShare_Regular(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	/** One row's send state. Owned by a TSharedPtr the button lambda and the HTTP callback both hold,
	 *  so a reply arriving after the panel was rebuilt has somewhere harmless to land. */
	struct FTraceShareRow
	{
		FString Path;
		FString Name;        // clean filename, the name the server saves under
		int64 Bytes = 0;
		bool bLive = false;  // still being recorded when the panel opened
		bool bInFlight = false;
		FString Status;      // "", "SENDING...", "SENT", "FAILED ..."
		FLinearColor StatusColor = TraceShare_Faint;
	};

	/** Newest first, capped: this is "the last few rides", not a file browser. */
	TArray<TSharedPtr<FTraceShareRow>> TraceShare_ListTraces(UWorld* World, int32 MaxRows)
	{
		FString LivePath;
		if (ASurfboardPawn* Pawn = Cast<ASurfboardPawn>(UGameplayStatics::GetPlayerPawn(World, 0)))
		{
			// The live ride's rows sit in a 10 s buffer; send what the ride is now, not what it was.
			Pawn->FlushInputTraceForSharing();
			LivePath = Pawn->ActiveInputTracePath();
		}

		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InputTraces"));
		IFileManager& Fm = IFileManager::Get();
		TArray<FString> Files;
		Fm.FindFiles(Files, *(Dir / TEXT("*.csv")), /*Files*/true, /*Directories*/false);

		struct FStamped { FString Path; FDateTime Modified; };
		TArray<FStamped> Stamped;
		Stamped.Reserve(Files.Num());
		for (const FString& File : Files)
		{
			const FString Full = Dir / File;
			Stamped.Add({ Full, Fm.GetTimeStamp(*Full) });
		}
		Stamped.Sort([](const FStamped& A, const FStamped& B) { return A.Modified > B.Modified; });

		TArray<TSharedPtr<FTraceShareRow>> Rows;
		for (int32 i = 0; i < Stamped.Num() && i < MaxRows; ++i)
		{
			TSharedPtr<FTraceShareRow> Row = MakeShared<FTraceShareRow>();
			Row->Path  = Stamped[i].Path;
			Row->Name  = FPaths::GetCleanFilename(Row->Path);
			Row->Bytes = Fm.FileSize(*Row->Path);
			Row->bLive = !LivePath.IsEmpty() && FPaths::IsSamePath(Row->Path, LivePath);
			Rows.Add(Row);
		}
		return Rows;
	}

	/** "phone-2026-09-15-12-53-40.csv" -> "09-15 12:53:40". The prefix is the same on every row and
	 *  the year is not in doubt; what tells rides apart is the time. */
	FString TraceShare_ShortName(const FString& Name)
	{
		const FString Base = FPaths::GetBaseFilename(Name);
		int32 Dash = INDEX_NONE;
		if (Base.FindChar(TEXT('-'), Dash))
		{
			const FString Stamp = Base.Mid(Dash + 1);   // 2026-09-15-12-53-40
			if (Stamp.Len() == 19)
			{
				return FString::Printf(TEXT("%s %s:%s:%s"),
					*Stamp.Mid(5, 5), *Stamp.Mid(11, 2), *Stamp.Mid(14, 2), *Stamp.Mid(17, 2));
			}
		}
		return Base;
	}

	FString TraceShare_Size(int64 Bytes)
	{
		return Bytes >= 1024 * 1024
			? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0))
			: FString::Printf(TEXT("%lld KB"), Bytes / 1024);
	}

	void TraceShare_Send(TSharedPtr<FTraceShareRow> Row)
	{
		if (!Row.IsValid() || Row->bInFlight)
		{
			return;
		}
		const FString Url = TraceShare::DestinationUrl();
		if (Url.IsEmpty())
		{
			Row->Status = TEXT("NO URL");
			Row->StatusColor = TraceShare_Bad;
			return;
		}

		TArray<uint8> Body;
		if (!FFileHelper::LoadFileToArray(Body, *Row->Path))
		{
			Row->Status = TEXT("READ FAILED");
			Row->StatusColor = TraceShare_Bad;
			UE_LOG(LogSurf, Warning, TEXT("TraceShare: could not read %s"), *Row->Path);
			return;
		}

		Row->bInFlight = true;
		Row->Status = TEXT("SENDING...");
		Row->StatusColor = TraceShare_Lit;
		UE_LOG(LogSurf, Display, TEXT("TraceShare: POST %s (%lld bytes) -> %s"), *Row->Name, (int64)Body.Num(), *Url);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("text/csv"));
		// The server saves under this name, so the file lands on the PC as it was on the phone and
		// the two never need reconciling. Device tag so PC captures and phone captures stay apart.
		Request->SetHeader(TEXT("X-Trace-Name"), Row->Name);
		Request->SetHeader(TEXT("X-Trace-Device"), FPlatformMisc::GetDeviceMakeAndModel());
		Request->SetContent(MoveTemp(Body));
		Request->SetTimeout(60.0f);   // a 1.5 MB trace over mobile data can take a while
		Request->OnProcessRequestComplete().BindLambda(
			[Row](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				Row->bInFlight = false;
				const int32 Code = (bConnected && Response.IsValid()) ? Response->GetResponseCode() : 0;
				if (Code >= 200 && Code < 300)
				{
					Row->Status = TEXT("SENT");
					Row->StatusColor = TraceShare_Good;
					UE_LOG(LogSurf, Display, TEXT("TraceShare: %s sent (%d)"), *Row->Name, Code);
				}
				else
				{
					Row->Status = Code == 0 ? FString(TEXT("FAILED: no connection")) : FString::Printf(TEXT("FAILED: HTTP %d"), Code);
					Row->StatusColor = TraceShare_Bad;
					UE_LOG(LogSurf, Warning, TEXT("TraceShare: %s NOT sent - %s"), *Row->Name, *Row->Status);
				}
			});
		Request->ProcessRequest();
	}

	TSharedRef<SWidget> TraceShare_MakeRow(TSharedPtr<FTraceShareRow> Row)
	{
		const FString Label = TraceShare_ShortName(Row->Name) + (Row->bLive ? TEXT("   ·   RIDING NOW") : TEXT(""));
		return SNew(SBox).HeightOverride(44.0f).Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(TraceShare_Bold(12))
					.ColorAndOpacity(FSlateColor(Row->bLive ? TraceShare_Lit : TraceShare_Ink))
					.Text(FText::FromString(Label))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(TraceShare_Regular(9))
					.ColorAndOpacity(FSlateColor(TraceShare_Faint))
					.Text(FText::FromString(TraceShare_Size(Row->Bytes)))
				]
			]

			// Status is a live attribute: the button lambda and the HTTP reply write the row, the
			// text re-reads it every paint. Nothing to rebuild.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(TraceShare_Bold(10))
				.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([Row]() { return FSlateColor(Row->StatusColor); }))
				.Text(TAttribute<FText>::CreateLambda([Row]() { return FText::FromString(Row->Status); }))
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(72.0f).HeightOverride(34.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(4.0f))
					.IsEnabled(TAttribute<bool>::CreateLambda([Row]() { return !Row->bInFlight; }))
					.OnClicked_Lambda([Row]() { TraceShare_Send(Row); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Justification(ETextJustify::Center)
						.Font(TraceShare_Bold(11))
						.Text(FText::FromString(TEXT("SEND")))
					]
				]
			]
		];
	}
}

// The panel's SEND without the panel: newest trace by default, or the first of the newest ten whose
// name contains the argument. For the desktop (-ExecCmds at launch, or the ~ console) and for
// proving the route end to end without a finger on a button; the outcome is in the log.
static FAutoConsoleCommandWithWorldAndArgs GTraceShareSendCmd(
	TEXT("surf.traceshare.send"),
	TEXT("Post a ride trace to surf.traceshare.url. No argument = the newest in Saved/InputTraces;\n")
	TEXT("otherwise the newest whose filename contains the argument."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const FString Want = Args.Num() > 0 ? Args[0] : FString();
		for (const TSharedPtr<FTraceShareRow>& Row : TraceShare_ListTraces(World, /*MaxRows*/10))
		{
			if (Want.IsEmpty() || Row->Name.Contains(Want))
			{
				TraceShare_Send(Row);
				return;
			}
		}
		UE_LOG(LogSurf, Warning, TEXT("TraceShare: no trace%s%s to send"),
			Want.IsEmpty() ? TEXT("") : TEXT(" matching "), *Want);
	}));

namespace TraceShare
{
	FString DestinationUrl()
	{
		return CVarTraceShareUrl.GetValueOnGameThread().TrimStartAndEnd();
	}

	TSharedRef<SWidget> BuildPanel(UWorld* World)
	{
		const FString Url = DestinationUrl();
		const TArray<TSharedPtr<FTraceShareRow>> Rows = TraceShare_ListTraces(World, /*MaxRows*/10);

		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		if (Rows.Num() == 0)
		{
			List->AddSlot().AutoHeight()
			[
				SNew(STextBlock).Font(TraceShare_Regular(11)).ColorAndOpacity(FSlateColor(TraceShare_Dim))
				.Text(FText::FromString(TEXT("No traces in Saved/InputTraces yet.")))
			];
		}
		for (const TSharedPtr<FTraceShareRow>& Row : Rows)
		{
			List->AddSlot().AutoHeight() [ TraceShare_MakeRow(Row) ];
		}

		// Same box as the tuning panel: 560 wide, height capped so the Android SDPIScaler's 2x
		// still fits a landscape phone.
		return SNew(SBox).WidthOverride(560.0f).MaxDesiredHeight(540.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.05f, 0.92f))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(TraceShare_Bold(13)).ColorAndOpacity(FSlateColor(TraceShare_Ink))
					.Text(FText::FromString(TEXT("SEND A RIDE TO THE PC")))
				]

				// The destination on screen, because a wrong or empty URL is the likeliest failure
				// and one the phone can otherwise only report as "no connection".
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock).Font(TraceShare_Regular(9))
					.ColorAndOpacity(FSlateColor(Url.IsEmpty() ? TraceShare_Bad : TraceShare_Faint))
					.Text(FText::FromString(Url.IsEmpty()
						? FString(TEXT("surf.traceshare.url is not set - see specs/share-trace-from-phone.md"))
						: Url))
				]

				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SScrollBox) + SScrollBox::Slot() [ List ]
				]
			]
		];
	}
}
