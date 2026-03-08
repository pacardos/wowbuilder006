#include "WoWMapImporterActor.h"
// Open files
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
// create dynamic meshes
#include "DynamicMeshActor.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "EngineUtils.h"
#include "IndexTypes.h"
// gui
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Framework/Application/SlateApplication.h"
// create water plane
#include "Engine/StaticMeshActor.h"
// json support to liquid parsing
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"



// maximum capacity for reference
constexpr int32 MaxTileVertices = 37120;    // 145 × 16 × 16 vertices -> no holes
constexpr int32 MaxTileTriangles = 70000;   // safe triangle upper bound

using namespace UE::Geometry;



AWoWMapImporterActor::AWoWMapImporterActor()
{
    PrimaryActorTick.bCanEverTick = false;
}



// Helper to handle the Bit-Shifting for Tags
FString AWoWMapImporterActor::GetTagString(uint32 Tag)
{
    ANSICHAR TagChars[5];
    TagChars[0] = (ANSICHAR)(Tag & 0xFF);
    TagChars[1] = (ANSICHAR)((Tag >> 8) & 0xFF);
    TagChars[2] = (ANSICHAR)((Tag >> 16) & 0xFF);
    TagChars[3] = (ANSICHAR)((Tag >> 24) & 0xFF);
    TagChars[4] = '\0';
    return ANSI_TO_TCHAR(TagChars);
}



#if WITH_EDITOR

void AWoWMapImporterActor::LoadWDT()
{
    FString FullPath = WDTFile.FilePath;

    if (FullPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("No WDT selected."));
        return;
    }

    MapName = FPaths::GetBaseFilename(FullPath);
    WorkingDirectory = FPaths::GetPath(FullPath);

    UE_LOG(LogTemp, Warning, TEXT("Loading WDT: %s"), *FullPath);

    TArray<uint8> Data;

    if (!FFileHelper::LoadFileToArray(Data, *FullPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load WDT."));
        return;
    }

    ParseWDT(Data);
}

#endif

void AWoWMapImporterActor::ParseWDT(const TArray<uint8>& Data)
{
    FMemoryReader Reader(Data);

    AvailableADTs.Empty();

    while (!Reader.AtEnd())
    {
        uint32 ChunkID;
        uint32 ChunkSize;

        Reader << ChunkID;
        Reader << ChunkSize;

        FName TagStr = FName(*GetTagString(ChunkID))    ;

        int64 ChunkStart = Reader.Tell();

        if (TagStr == "NIAM") // MAIN
        {
            // Only loop if the size matches expectations
            int32 NumEntries = ChunkSize / 8;
            for (int32 i = 0; i < NumEntries; i++)
            {
                uint32 Flags;
                uint32 Unused;
                Reader << Flags;
                Reader << Unused;

                // In WDT, bit 0 is the "Has ADT" flag
                if (Flags & 1)
                {
                    int32 Y = i / 64; // Row
                    int32 X = i % 64; // Column

                    UE_LOG(LogTemp, Log, TEXT("Tile found at: Row %d | Col %d"), Y, X);

                    // You might want to store these coordinates in a TArray to process later
                    // to parse all adts that wdt know about
                    AvailableADTs.Add(FIntPoint(X, Y));
                }
            }
        }

        Reader.Seek(ChunkStart + ChunkSize);
    }

    if (AvailableADTs.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Total ADTs detected: %d"), AvailableADTs.Num());
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("No ADTs detected. Terminating..."));
        return;
    }

    ShowADTGrid();
}



void AWoWMapImporterActor::ShowADTGrid()
{
    SelectedADTs.Empty();

    TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel);

    for (int Y = 0; Y < 64; Y++)
    {
        for (int X = 0; X < 64; X++)
        {
            FIntPoint Tile(X, Y);
            bool bExists = AvailableADTs.Contains(Tile);

            Grid->AddSlot(X, Y)
                [
                    SNew(SBox)
                        .WidthOverride(8)
                        .HeightOverride(6)
                        [
                            SNew(SButton)
                                .ButtonStyle(FAppStyle::Get(), "NoBorder")
                                .ToolTipText_Lambda([X, Y, bExists]()
                                    {
                                        if (!bExists)
                                            return FText::GetEmpty();

                                        return FText::FromString(
                                            FString::Printf(TEXT("Tile: %u %u"), X, Y)
                                        );
                                    })
                                .IsEnabled(bExists)
                                .OnClicked_Lambda([this, Tile]()
                                    {
                                        if (SelectedADTs.Contains(Tile))
                                            SelectedADTs.Remove(Tile);
                                        else
                                            SelectedADTs.Add(Tile);

                                        return FReply::Handled();
                                    })
                                [
                                    SNew(SBorder)
                                        .BorderImage(FAppStyle::Get().GetBrush("WhiteBrush")) // <- important
                                        .BorderBackgroundColor_Lambda([this, Tile, bExists]()
                                            {
                                                if (!bExists)
                                                    return FLinearColor(0.018f, 0.018f, 0.018f);

                                                if (SelectedADTs.Contains(Tile))
                                                    return FLinearColor::Red;

                                                return FLinearColor::Blue;
                                            })
                                        
                                ]
                        ]
                ];
        }
    }

    TSharedPtr<SWindow> Window;
    TWeakPtr<SWindow> WeakWindow;

    Window = SNew(SWindow)
        .Title(FText::FromString("ADT Selection"))
        .ClientSize(FVector2D(1100, 900));

    WeakWindow = Window;

    Window->SetContent(

        SNew(SHorizontalBox)

        + SHorizontalBox::Slot()
        .FillWidth(1.0)
        [
            Grid
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(10)
        [
            SNew(SVerticalBox)

                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                        .Text(FText::FromString("Options"))
                ]

                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SCheckBox)
                        .IsChecked_Lambda([this]()
                            {
                                return bGenerateDynamicMesh ?
                                    ECheckBoxState::Checked :
                                    ECheckBoxState::Unchecked;
                            })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                            {
                                bGenerateDynamicMesh = (State == ECheckBoxState::Checked);
                            })
                        [
                            SNew(STextBlock).Text(FText::FromString("Generate Dynamic Mesh"))
                        ]
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SCheckBox)
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                            {
                                bParseWater = (State == ECheckBoxState::Checked);
                            })
                        [
                            SNew(STextBlock).Text(FText::FromString("Parse Water"))
                        ]
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SCheckBox)
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                            {
                                bParseTextures = (State == ECheckBoxState::Checked);
                            })
                        [
                            SNew(STextBlock).Text(FText::FromString("Parse Textures"))
                        ]
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SCheckBox)
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                            {
                                bParseM2 = (State == ECheckBoxState::Checked);
                            })
                        [
                            SNew(STextBlock).Text(FText::FromString("Parse M2 Doodads"))
                        ]
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SCheckBox)
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                            {
                                bParseWMO = (State == ECheckBoxState::Checked);
                            })
                        [
                            SNew(STextBlock).Text(FText::FromString("Parse WMOs"))
                        ]
                ]

            + SVerticalBox::Slot().AutoHeight().Padding(10)
                [
                    SNew(SButton)
                        .Text(FText::FromString("Select All"))
                        .OnClicked_Lambda([this]()
                            {
                                SelectedADTs = AvailableADTs;
                                return FReply::Handled();
                            })
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SButton)
                        .Text(FText::FromString("Deselect"))
                        .OnClicked_Lambda([this]()
                            {
                                SelectedADTs.Empty();
                                return FReply::Handled();
                            })
                ]

            + SVerticalBox::Slot().AutoHeight().Padding(10)
                [
                    SNew(SButton)
                        .Text(FText::FromString("Parse"))
                        .OnClicked_Lambda([this]()
                            {
                                ParseSelectedADTs();
                                return FReply::Handled();
                            })
                ]

            + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SButton)
                        .Text(FText::FromString("Cancel"))
                        .OnClicked_Lambda([WeakWindow]()
                            {
                                if (WeakWindow.IsValid())
                                {
                                    WeakWindow.Pin()->RequestDestroyWindow();
                                }

                                return FReply::Handled();
                            })
                ]
        ]
    );

    FSlateApplication::Get().AddWindow(Window.ToSharedRef());
}

void AWoWMapImporterActor::ParseSelectedADTs()
{
    UE_LOG(LogTemp, Warning, TEXT("Selected ADTs: %d"), SelectedADTs.Num());

    if (bGenerateDynamicMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== TERRAIN PHASE ==="));

        for (const FIntPoint& Tile : SelectedADTs)
        {
            ImportTerrainOBJ(Tile);
        }
    }

    if (bParseWater)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== WATER PHASE ==="));

        for (const FIntPoint& Tile : SelectedADTs)
        {
            ImportWater(Tile);
        }
    }

    if (bParseTextures)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== TEXTURE PHASE ==="));

        for (const FIntPoint& Tile : SelectedADTs)
        {
            ImportTextures(Tile);
        }
    }

    if (bParseM2)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== M2 DOODADS PHASE ==="));

        for (const FIntPoint& Tile : SelectedADTs)
        {
            ImportM2Doodads(Tile);
        }
    }

    if (bParseWMO)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== WMO PHASE ==="));

        for (const FIntPoint& Tile : SelectedADTs)
        {
            ImportWMOs(Tile);
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("Parsing finished."));
}



void AWoWMapImporterActor::ImportTerrainOBJ(const FIntPoint& Tile)
{
    FString OBJName = FString::Printf(
        TEXT("adt_%02d_%02d.obj"),
        Tile.X,
        Tile.Y
    );

    FString OBJPath = FPaths::Combine(WorkingDirectory, OBJName);

    UE_LOG(LogTemp, Warning, TEXT("Loading OBJ: %s"), *OBJPath);

    LoadOBJTile(OBJPath, Tile);
}

void AWoWMapImporterActor::LoadOBJTile(const FString& OBJPath, const FIntPoint& Tile)
{
    TArray<FString> Lines;

    if (!FFileHelper::LoadFileToStringArray(Lines, *OBJPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load OBJ."));
        return;
    }

    UE::Geometry::FDynamicMesh3 Mesh;

    for (const FString& Line : Lines)
    {
        if (Line.StartsWith("v "))
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT(" "), true);

            float X = FCString::Atof(*Parts[1]);
            float Y = FCString::Atof(*Parts[2]);
            float Z = FCString::Atof(*Parts[3]);

            FVector UEVertex(
                -Z * 100.f,
                X * 100.f,
                Y * 100.f
            );

            Mesh.AppendVertex(FVector3d(UEVertex));
        }
    }

    for (const FString& Line : Lines)
    {
        if (Line.StartsWith("f "))
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT(" "), true);

            int v0 = FCString::Atoi(*Parts[1]) - 1;
            int v1 = FCString::Atoi(*Parts[2]) - 1;
            int v2 = FCString::Atoi(*Parts[3]) - 1;

            Mesh.AppendTriangle(v0, v1, v2);
        }
    }

    FString ActorName = FString::Printf(
        TEXT("%s_%02d_%02d"),
        *MapName,
        Tile.X,
        Tile.Y
    );

    ADynamicMeshActor* MeshActor = nullptr;

    /* ---------------------------------- */
    /* Look for an existing terrain actor */
    /* ---------------------------------- */

    for (TActorIterator<ADynamicMeshActor> It(GetWorld()); It; ++It)
    {
        if (It->GetActorLabel() == ActorName)
        {
            MeshActor = *It;
            break;
        }
    }

    /* ---------------------------------- */
    /* Create new actor if needed         */
    /* ---------------------------------- */

    if (!MeshActor)
    {
        MeshActor = GetWorld()->SpawnActor<ADynamicMeshActor>();

        if (!MeshActor)
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to spawn mesh actor."));
            return;
        }

        MeshActor->SetActorLabel(ActorName);
        MeshActor->SetFolderPath(TEXT("Landscape"));

        UE_LOG(LogTemp, Warning, TEXT("Created new terrain actor: %s"), *ActorName);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Updating terrain actor: %s"), *ActorName);
    }

    /* ---------------------------------- */
    /* Update mesh geometry               */
    /* ---------------------------------- */

    UDynamicMeshComponent* MeshComponent = MeshActor->GetDynamicMeshComponent();

    MeshComponent->GetDynamicMesh()->SetMesh(MoveTemp(Mesh));
    MeshComponent->NotifyMeshUpdated();

    UE_LOG(LogTemp, Warning, TEXT("Terrain mesh generated for %s"), *ActorName);

    /* ---------------------------------- */
    /* Enable Collision                   */
    /* ---------------------------------- */

    MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    MeshComponent->SetCollisionProfileName(TEXT("BlockAll"));
    MeshComponent->SetGenerateOverlapEvents(false);

    MeshComponent->UpdateCollision();
}



void AWoWMapImporterActor::ImportWater(const FIntPoint& Tile)
{
    /* ------------------------------------------------------------ */
    /* 1 — Build JSON path                                          */
    /* ------------------------------------------------------------ */

    FString FileName = FString::Printf(TEXT("liquid_%d_%d.json"), Tile.X, Tile.Y);
    FString JSONPath = FPaths::Combine(WorkingDirectory, FileName);

    if (!FPaths::FileExists(JSONPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("No liquid file for tile %d_%d"), Tile.X, Tile.Y);
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("Loading liquid JSON: %s"), *JSONPath);


    /* ------------------------------------------------------------ */
    /* 2 — Load JSON file                                           */
    /* ------------------------------------------------------------ */

    FString JSONString;

    if (!FFileHelper::LoadFileToString(JSONString, *JSONPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to read liquid JSON"));
        return;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JSONString);

    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid liquid JSON"));
        return;
    }


    /* ------------------------------------------------------------ */
    /* 3 — Get liquidChunks array                                   */
    /* ------------------------------------------------------------ */

    const TArray<TSharedPtr<FJsonValue>>* LiquidChunks;

    if (!Root->TryGetArrayField(TEXT("liquidChunks"), LiquidChunks))
    {
        UE_LOG(LogTemp, Warning, TEXT("No liquidChunks found"));
        return;
    }


    /* ------------------------------------------------------------ */
    /* 4 — Iterate chunk slots (max 256)                            */
    /* ------------------------------------------------------------ */

    for (int32 ChunkIndex = 0; ChunkIndex < LiquidChunks->Num(); ChunkIndex++)
    {
        TSharedPtr<FJsonObject> ChunkObj = (*LiquidChunks)[ChunkIndex]->AsObject();

        if (!ChunkObj.IsValid())
            continue;


        const TArray<TSharedPtr<FJsonValue>>* Instances;

        if (!ChunkObj->TryGetArrayField(TEXT("instances"), Instances))
            continue;


        /* -------------------------------------------------------- */
        /* 5 — Iterate liquid instances in chunk                    */
        /* -------------------------------------------------------- */

        for (auto& InstanceValue : *Instances)
        {
            TSharedPtr<FJsonObject> Instance = InstanceValue->AsObject();

            if (!Instance.IsValid())
                continue;


            /* ---------------------------------------------------- */
            /* 6 — Read grid size                                   */
            /* ---------------------------------------------------- */

            int32 Width = Instance->GetIntegerField(TEXT("width"));
            int32 Height = Instance->GetIntegerField(TEXT("height"));
            int32 LiquidType = Instance->GetIntegerField(TEXT("liquidType"));

            // get the liquid name from ID
            FString LiquidName;

            switch (LiquidType)
            {
            case 1: LiquidName = TEXT("Water"); break;
            case 2: LiquidName = TEXT("Ocean"); break;
            case 3: LiquidName = TEXT("Slime"); break;
            case 4: LiquidName = TEXT("River"); break;
            case 6: LiquidName = TEXT("Lava"); break;
            default: LiquidName = TEXT("Unknown"); break;
            }

            UE_LOG(LogTemp, Warning,
                TEXT("Water instance: %d x %d type %d"),
                Width, Height, LiquidType);


            /* ---------------------------------------------------- */
            /* 7 — Read world position                              */
            /* ---------------------------------------------------- */

            const TArray<TSharedPtr<FJsonValue>>* WorldPos;

            if (!Instance->TryGetArrayField(TEXT("worldPosition"), WorldPos))
                continue;

            double WX = (*WorldPos)[0]->AsNumber();
            double WY = (*WorldPos)[1]->AsNumber();
            double WZ = (*WorldPos)[2]->AsNumber();


            /* ---------------------------------------------------- */
            /* 8 — Read vertex height map                           */
            /* ---------------------------------------------------- */

            const TSharedPtr<FJsonObject>* VertexData;

            if (!Instance->TryGetObjectField(TEXT("vertexData"), VertexData))
                continue;

            const TArray<TSharedPtr<FJsonValue>>* HeightArray;

            if (!(*VertexData)->TryGetArrayField(TEXT("height"), HeightArray))
                continue;


            /* ---------------------------------------------------- */
            /* 9 — Build vertices                                   */
            /* ---------------------------------------------------- */

            TArray<FVector> Vertices;

            const double Step = 4.166666 * 100.0; // WoW liquid grid spacing in cm

            for (int y = 0; y <= Height; y++)
            {
                for (int x = 0; x <= Width; x++)
                {
                    int Index = y * (Width + 1) + x;

                    double HeightValue = (*HeightArray)[Index]->AsNumber();

                    double OffsetX = (x - Width / 2.0) * Step;
                    double OffsetY = (y - Height / 2.0) * Step;

                    double VX = WX * 100.0 - OffsetX;
                    double VY = -(WZ * 100.0 - OffsetY);
                    double VZ = HeightValue * 100.0;

                    Vertices.Add(FVector(VX, VY, VZ));
                }
            }


            /* ---------------------------------------------------- */
            /* 10 — Build triangles                                 */
            /* ---------------------------------------------------- */

            TArray<FIndex3i> Triangles;

            for (int y = 0; y < Height; y++)
            {
                for (int x = 0; x < Width; x++)
                {
                    int v0 = y * (Width + 1) + x;
                    int v1 = v0 + 1;
                    int v2 = v0 + (Width + 1);
                    int v3 = v2 + 1;

                    Triangles.Add(FIndex3i(v0, v2, v1));
                    Triangles.Add(FIndex3i(v1, v2, v3));
                }
            }


            /* ---------------------------------------------------- */
            /* 11 — Build dynamic mesh                              */
            /* ---------------------------------------------------- */

            UE::Geometry::FDynamicMesh3 Mesh;

            for (const FVector& V : Vertices)
            {
                Mesh.AppendVertex((FVector3d)V);
            }

            for (const FIndex3i& T : Triangles)
            {
                Mesh.AppendTriangle(T);
            }


            /* ---------------------------------------------------- */
            /* 12 — Spawn water actor                               */
            /* ---------------------------------------------------- */

            ADynamicMeshActor* WaterActor =
                GetWorld()->SpawnActor<ADynamicMeshActor>();

            if (!WaterActor)
                continue;

            WaterActor->GetDynamicMeshComponent()->SetMesh(MoveTemp(Mesh));

            WaterActor->SetActorRotation(FRotator(0.f, 90.f, 180.f));

            WaterActor->SetActorLabel(
                FString::Printf(TEXT("Liquid_%s_%d_%d"), *LiquidName, Tile.X, Tile.Y)
            );

            WaterActor->SetFolderPath(TEXT("LiquidBody"));
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("Water import finished."));
}





void AWoWMapImporterActor::ImportTextures(const FIntPoint& Tile) {}
void AWoWMapImporterActor::ImportM2Doodads(const FIntPoint& Tile) {}
void AWoWMapImporterActor::ImportWMOs(const FIntPoint& Tile) {}