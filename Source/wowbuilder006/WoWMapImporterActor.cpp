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

#include "Algo/MinElement.h"
#include "Algo/MaxElement.h"
// textures
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInstanceConstant.h"
#include "RenderingThread.h" // Required for FlushRenderingCommands
// progress bar
#include "Misc/ScopedSlowTask.h"
// parse adt m2
#include "Serialization/MemoryReader.h"
// table of missing meshes
#include "Kismet/GameplayStatics.h"



// maximum capacity for reference
constexpr int32 MaxTileVertices = 37120;    // 145 × 16 × 16 vertices -> no holes
constexpr int32 MaxTileTriangles = 70000;   // safe triangle upper bound

using namespace UE::Geometry;

// wow reference values
constexpr float WOW_TILE_SIZE = (1600.0f / 3.0f) * 100.0f;          // 53333.333
constexpr float WOW_CHUNK_SIZE = WOW_TILE_SIZE / 16.0f;             // 3333.333
constexpr float WOW_QUAD_SIZE = WOW_TILE_SIZE / (16.0f * 8.0f);     // 416.666



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

    ShowTileGrid();
}



void AWoWMapImporterActor::ShowTileGrid()
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

            + SVerticalBox::Slot().AutoHeight().Padding(10)
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
                                ParseSelectedTiles();
                                return FReply::Handled();
                            })
                ]

            + SVerticalBox::Slot().AutoHeight().Padding(10)
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

            + SVerticalBox::Slot().AutoHeight().Padding(10)
                [
                    SNew(SButton)
                        .Text(FText::FromString("M2 Update"))
                        .ToolTipText(FText::FromString("Checks the Missing M2 Table and swaps placeholders for assigned meshes."))
                        .OnClicked_Lambda([this]()
                            {
                                UpdateM2Doodads();
                                return FReply::Handled();
                            })
                ]
        ]
    );

    FSlateApplication::Get().AddWindow(Window.ToSharedRef());
}

void AWoWMapImporterActor::ParseSelectedTiles()
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
        TEXT("adt_%d_%d.obj"),
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
    if (!FFileHelper::LoadFileToStringArray(Lines, *OBJPath)) return;

    UE::Geometry::FDynamicMesh3 Mesh;
    Mesh.EnableAttributes();
    // enable uv attribute
    FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV();
    TArray<int32> UVHandles;
    // enable normals attribute
    FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
    if (!NormalOverlay)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to initialize Normal Overlay"));
        return;
    }
    TArray<int32> NormalHandles;

    // --- PASS 1: VERTICES ONLY ---
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith("v "))
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT(" "), true);
            // Your existing Vertex math
            FVector UEVertex(-FCString::Atof(*Parts[3]) * 100.f, FCString::Atof(*Parts[1]) * 100.f, FCString::Atof(*Parts[2]) * 100.f);
            Mesh.AppendVertex(FVector3d(UEVertex));
        }
    }

    // --- PASS 2: GENERATE UV HANDLES (The Missing Step) ---
    TArray<int32> GlobalUVHandles;
    FAxisAlignedBox3d Bounds = Mesh.GetBounds();
    FVector3d Min = Bounds.Min;
    FVector3d Max = Bounds.Max;
    FVector3d MeshSize = Max - Min;

    double DivX = (MeshSize.X > 0.0001) ? MeshSize.X : 1.0;
    double DivY = (MeshSize.Y > 0.0001) ? MeshSize.Y : 1.0;

    // Important: Use Mesh.MaxVertexID() to ensure we match the mesh exactly
    for (int32 i = 0; i < Mesh.MaxVertexID(); ++i)
    {
        FVector3d P = Mesh.GetVertex(i);

        // get the standard 0-1 normalized coordinates
        float NormX = (float)((P.X - Min.X) / DivX);
        float NormY = (float)((P.Y - Min.Y) / DivY);

        // Apply 90 degree CCW Rotation:
        // NewU = NormY
        // NewV = 1.0 - NormX
        float RotatedU = 1.0f - NormY;
        float RotatedV = 1.0f - NormX;

        // This fills the array so the next loop doesn't crash!
        GlobalUVHandles.Add(UVOverlay->AppendElement(FVector2f(RotatedU, RotatedV)));
    }

    // --- PASS NORMALS ---
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("vn ")))
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT(" "), true);

            if (Parts.Num() < 4) continue;

            // Swizzle to match your Vertex math: (-Z, X, Y)
            float NX = FCString::Atof(*Parts[1]);
            float NY = FCString::Atof(*Parts[2]);
            float NZ = FCString::Atof(*Parts[3]);

            // FVector3f is used for Normals in the Overlay
            FVector3f UENormal(-(float)NZ, (float)NX, (float)NY);
            NormalHandles.Add(NormalOverlay->AppendElement(UENormal));
        }
    }

    // --- PASS FACES ---
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("f ")))
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT(" "), true);
            if (Parts.Num() < 4) continue;

            // Helper to parse "v/vt/vn" and return all three indices
            auto ParseOBJIndices = [](const FString& InPart, int32& OutV, int32& OutVN) {
                TArray<FString> SubParts;
                InPart.ParseIntoArray(SubParts, TEXT("/"), false);

                OutV = FCString::Atoi(*SubParts[0]) - 1;

                // The normal index is the 3rd part (index 2)
                if (SubParts.Num() >= 3) {
                    OutVN = FCString::Atoi(*SubParts[2]) - 1;
                }
                else {
                    OutVN = OutV; // Fallback
                }
            };

            // NOW we declare them so the compiler can find them
            int32 v0, vn0, v1, vn1, v2, vn2;
            ParseOBJIndices(Parts[1], v0, vn0);
            ParseOBJIndices(Parts[2], v1, vn1);
            ParseOBJIndices(Parts[3], v2, vn2);

            int32 TriID = Mesh.AppendTriangle(v0, v1, v2);

            if (TriID != IndexConstants::InvalidID)
            {
                // Safety check against the GlobalUVHandles array we built in Pass 2
                if (GlobalUVHandles.IsValidIndex(v0) &&
                    GlobalUVHandles.IsValidIndex(v1) &&
                    GlobalUVHandles.IsValidIndex(v2))
                {
                    UVOverlay->SetTriangle(TriID, FIndex3i(
                        GlobalUVHandles[v0],
                        GlobalUVHandles[v1],
                        GlobalUVHandles[v2]
                    ));
                }

                // Set Normals from the file
                if (NormalHandles.IsValidIndex(vn0) &&
                    NormalHandles.IsValidIndex(vn1) &&
                    NormalHandles.IsValidIndex(vn2))
                {
                    NormalOverlay->SetTriangle(TriID, FIndex3i(
                        NormalHandles[vn0],
                        NormalHandles[vn1],
                        NormalHandles[vn2]
                    ));
                }
            }
        }
    }

    FString ActorName = FString::Printf(
        TEXT("%s_%d_%d"),
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
    /* check if we already                */
    /*             have the material made */
    /* ---------------------------------- */
    // 1. Try to find the specific Material Instance for this tile
    FString TileID = FString::Printf(TEXT("%s_%d_%d"), *MapName, Tile.X, Tile.Y);
    FString MIPath = FString::Printf(TEXT("/Game/maps/%s/materials/MI_%s.%s"), *MapName, *TileID, *TileID);

    // TRY to load the material
    UMaterialInterface* CurrentMat = LoadObject<UMaterialInstanceConstant>(nullptr, *MIPath);

    // IF NOT FOUND, use the Master Debug as a placeholder
    if (!CurrentMat)
    {
        CurrentMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/test-zone/M_WoWTerrain_Debug"));
        UE_LOG(LogTemp, Warning, TEXT("Mesh %s: Material Instance not found, using Debug Master."), *TileID);
    }
    
    if (CurrentMat && MeshComponent)
    {
        MeshComponent->SetMaterial(0, CurrentMat);
    }

    MeshComponent->SetNumMaterials(1);

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
    //---------------------------------------------------------
    // Build JSON path
    //---------------------------------------------------------

    FString JsonName = FString::Printf(TEXT("liquid_%d_%d.json"), Tile.X, Tile.Y);
    FString JsonPath = FPaths::Combine(WorkingDirectory, JsonName);

    if (!FPaths::FileExists(JsonPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("No liquid file for tile %d_%d"), Tile.X, Tile.Y);
        return;
    }

    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to read liquid json"));
        return;
    }

    //---------------------------------------------------------
    // Parse JSON
    //---------------------------------------------------------

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);

    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("Invalid liquid JSON"));
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* LiquidChunks;

    if (!Root->TryGetArrayField(TEXT("liquidChunks"), LiquidChunks))
        return;

    //---------------------------------------------------------
    // Build ONE mesh for the entire tile
    //---------------------------------------------------------

    UE::Geometry::FDynamicMesh3 Mesh;

    FVector TileOrigin;
    bool bTileOriginSet = false;

    FString LiquidName = TEXT("Liquid");

    //---------------------------------------------------------
    // Iterate chunks
    //---------------------------------------------------------

    for (const TSharedPtr<FJsonValue>& ChunkValue : *LiquidChunks)
    {
        const TSharedPtr<FJsonObject> Chunk = ChunkValue->AsObject();
        if (!Chunk.IsValid())
            continue;

        const TArray<TSharedPtr<FJsonValue>>* Instances;

        if (!Chunk->TryGetArrayField(TEXT("instances"), Instances))
            continue;

        //-----------------------------------------------------
        // Iterate liquid instances
        //-----------------------------------------------------

        for (const TSharedPtr<FJsonValue>& InstanceValue : *Instances)
        {
            const TSharedPtr<FJsonObject> Instance = InstanceValue->AsObject();
            if (!Instance.IsValid())
                continue;

            int32 Width = Instance->GetIntegerField(TEXT("width"));
            int32 Height = Instance->GetIntegerField(TEXT("height"));
            int32 LiquidType = Instance->GetIntegerField(TEXT("liquidType"));

            float MinHeight = (float)Instance->GetNumberField(TEXT("minHeightLevel"));
            float MaxHeight = (float)Instance->GetNumberField(TEXT("maxHeightLevel"));


            //-------------------------------------------------
            // Compute chunk position
            //-------------------------------------------------

            int32 ChunkIndex = Instance->GetIntegerField(TEXT("chunkIndex"));

            int32 ChunkX = ChunkIndex % 16;
            int32 ChunkY = ChunkIndex / 16;

            // float ChunkOffsetX = ChunkX * WOW_CHUNK_SIZE;
            float ChunkOffsetX = ChunkX * WOW_CHUNK_SIZE;
            float ChunkOffsetY = ChunkY * WOW_CHUNK_SIZE;

            //-------------------------------------------------
            // Liquid type name
            //-------------------------------------------------

            switch (LiquidType)
            {
            case 1: LiquidName = TEXT("Water"); break;
            case 2: LiquidName = TEXT("Ocean"); break;
            case 3: LiquidName = TEXT("Magma"); break;
            case 4: LiquidName = TEXT("Slime"); break;
            case 5: LiquidName = TEXT("River"); break;
            case 6: LiquidName = TEXT("Lava"); break;
            default: LiquidName = TEXT("Liquid"); break;
            }

            //-------------------------------------------------
            // World position
            //-------------------------------------------------

            const TArray<TSharedPtr<FJsonValue>>* WorldPos;

            if (!Instance->TryGetArrayField(TEXT("worldPosition"), WorldPos))
                continue;

            double WX = (*WorldPos)[0]->AsNumber();
            double WY = (*WorldPos)[1]->AsNumber();
            double WZ = (*WorldPos)[2]->AsNumber();

            FVector WorldLocation(
                -WZ * 100.f,
                WX * 100.f,
                WY * 100.f
            );

            if (!bTileOriginSet)
            {
                TileOrigin = WorldLocation;
                bTileOriginSet = true;
            }

            //-------------------------------------------------
            // Get bitmap
            //-------------------------------------------------

            const TArray<TSharedPtr<FJsonValue>>* Bitmap = nullptr;

            bool UseBitmap = Instance->TryGetArrayField(TEXT("bitmap"), Bitmap)
                && Bitmap
                && Bitmap->Num() > 0;

            int32 MaxBitmapBytes = (Width * Height + 7) / 8;

            //-------------------------------------------------
            // Height map
            //-------------------------------------------------

            TArray<float> HeightMap;

            const TSharedPtr<FJsonObject>* VertexData;

            if (Instance->TryGetObjectField(TEXT("vertexData"), VertexData))
            {
                const TArray<TSharedPtr<FJsonValue>>* HeightArray;

                if ((*VertexData)->TryGetArrayField(TEXT("height"), HeightArray))
                {
                    HeightMap.Reserve(HeightArray->Num());

                    for (const TSharedPtr<FJsonValue>& V : *HeightArray)
                        HeightMap.Add((float)V->AsNumber());
                }
            }

            //-------------------------------------------------
            // Vertex counts
            //-------------------------------------------------

            int32 VertCountX = Width + 1;
            int32 VertCountY = Height + 1;

            float QuadSize = WOW_QUAD_SIZE;

            int32 VertexOffset = Mesh.VertexCount();

            //-------------------------------------------------
            // Build vertices
            //-------------------------------------------------

            for (int32 y = 0; y < VertCountY; y++)
            {
                for (int32 x = 0; x < VertCountX; x++)
                {
                    float HeightWoW;

                    int32 VertIdx = y * VertCountX + x;

                    if (HeightMap.Num() > 0 && VertIdx < HeightMap.Num())
                    {
                        HeightWoW = HeightMap[VertIdx];
                    }
                    else
                    {
                        HeightWoW = WY;   // flat liquid surface
                    }

                    float Z = (HeightWoW - WY) * 100.f;

                    FVector Pos(
                        -(ChunkOffsetY + y * QuadSize),
                        (ChunkOffsetX + x * QuadSize),
                        Z
                    );

                    UE_LOG(LogTemp, Warning,
                        TEXT("Instance height: %f | vertex height: %f"),
                        WY,
                        HeightWoW
                    );

                    Mesh.AppendVertex((FVector3d)Pos);
                }
            }

            //-------------------------------------------------
            // Quad bitmap test
            //-------------------------------------------------

            auto QuadExists = [&](int32 X, int32 Y)
                {
                    if (!UseBitmap)
                        return true;

                    int32 Index = Y * Width + X;

                    int32 ByteIndex = Index >> 3;
                    int32 BitIndex = Index & 7;

                    if (ByteIndex >= MaxBitmapBytes)
                        return true;

                    uint8 Byte = (uint8)(*Bitmap)[ByteIndex]->AsNumber();

                    return ((Byte >> BitIndex) & 1) != 0;
                };

            //-------------------------------------------------
            // Build triangles
            //-------------------------------------------------

            for (int32 y = 0; y < Height; y++)
            {
                for (int32 x = 0; x < Width; x++)
                {
                    if (!QuadExists(x, y))
                        continue;

                    int v0 = VertexOffset + y * VertCountX + x;
                    int v1 = v0 + 1;
                    int v2 = v0 + VertCountX;
                    int v3 = v2 + 1;

                    Mesh.AppendTriangle(v0, v2, v1);
                    Mesh.AppendTriangle(v1, v2, v3);
                }
            }
        }
    }

    //---------------------------------------------------------
    // Spawn actor (ONE per tile)
    //---------------------------------------------------------

    if (Mesh.VertexCount() == 0)
        return;

    ADynamicMeshActor* WaterActor = GetWorld()->SpawnActor<ADynamicMeshActor>();

    if (!WaterActor)
        return;

    WaterActor->GetDynamicMeshComponent()->SetMesh(MoveTemp(Mesh));

    WaterActor->SetActorLocation(TileOrigin);

    WaterActor->SetActorRotation(FRotator(0.f, 90.f, 180.f));

    WaterActor->SetActorLabel(
        FString::Printf(TEXT("Liquid_%s_%d_%d"), *LiquidName, Tile.X, Tile.Y)
    );

    WaterActor->SetFolderPath(TEXT("LiquidBody"));
}



void AWoWMapImporterActor::ImportTextures(const FIntPoint& Tile)
{
    // 1. Setup Standardized Names
    FString TileID = FString::Printf(TEXT("%s_%d_%d"), *MapName, Tile.X, Tile.Y);
    FString MIFolder = FString::Printf(TEXT("/Game/maps/%s/materials"), *MapName);
    FString MIName = FString::Printf(TEXT("MI_%s"), *TileID);

    UE_LOG(LogTemp, Log, TEXT("Starting Texture Import for Tile: %s"), *TileID);

    // 2. Get the Texture List from your tool's file_list.txt
    TArray<FString> WoWPaths = GetTexturePathsFromList(Tile);

    // 3. Create or Load the Material Instance
    UMaterialInstanceConstant* MIC = CreateOrGetMaterialInstance(MIName, MIFolder);

    if (MIC)
    {
        // 4. Assign the 4 Diffuse Layers
        for (int32 i = 0; i < 4; i++)
        {
            FName ParamName = FName(*FString::Printf(TEXT("Layer_%d"), i));
            UTexture2D* LayerTex = nullptr;

            if (i < WoWPaths.Num())
            {
                FString UnrealPath = ConvertWoWPathToUnreal(WoWPaths[i]);
                LayerTex = LoadObject<UTexture2D>(nullptr, *UnrealPath);
            }

            // Pink Fallback if texture is missing or slot is empty
            if (!LayerTex)
            {
                LayerTex = LoadObject<UTexture2D>(nullptr, TEXT("/Game/_Textures/T_Error_White"));
            }

            MIC->SetTextureParameterValueEditorOnly(ParamName, LayerTex);
        }

        // 5. Link the Splatmap (The PNG from your tool)
        FString SplatPath = FString::Printf(TEXT("/Game/maps/%s/textures/Splat_%d_%d"), *MapName, Tile.X, Tile.Y);
        UTexture2D* SplatTex = LoadObject<UTexture2D>(nullptr, *SplatPath);

        if (SplatTex)
        {
            // 1. Tell Unreal to pause background tasks for this texture
            SplatTex->PreEditChange(nullptr);

            // 2. Apply your settings
            SplatTex->SRGB = false;
            SplatTex->CompressionSettings = TC_Masks;
            SplatTex->MipGenSettings = TMGS_NoMipmaps;

            // 3. Tell Unreal to re-register and compile the texture
            SplatTex->PostEditChange();

            // 4. Update the Material Instance parameter
            MIC->SetTextureParameterValueEditorOnly(FName("Splatmap"), SplatTex);
        }

        // 6. Finalize Asset and Apply to World Actor
        MIC->PostEditChange();
        ApplyMaterialToActor(TileID, MIC);
    }
    // This forces the GPU to finish all pending work before the loop continues
    FlushRenderingCommands();
}

UMaterialInstanceConstant* AWoWMapImporterActor::CreateOrGetMaterialInstance(const FString& Name, const FString& Folder)
{
    // 1. Build the "Full Reference" path: /Game/Path/Name.Name
    FString FullReference = FString::Printf(TEXT("%s/%s.%s"), *Folder, *Name, *Name);

    // 2. Try to load using the Full Reference
    UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *FullReference);

    // 3. Load the Master Material
    UMaterialInterface* MasterMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/maps/materials/M_WoW_Terrain_Master"));

    if (!MasterMat) {
        UE_LOG(LogTemp, Error, TEXT("CRITICAL: Master Material NOT found!"));
        return nullptr;
    }

    // 4. Create if it doesn't exist
    if (!MIC)
    {
        IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
        auto Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
        UObject* NewAsset = AssetTools.CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory);
        MIC = Cast<UMaterialInstanceConstant>(NewAsset);
    }

    // 5. Ensure Parent and Tiling are correct
    if (MIC)
    {
        if (MIC->Parent != MasterMat) MIC->SetParentEditorOnly(MasterMat);

        // Ensure tiling isn't 0 (which causes solid color)
        MIC->SetScalarParameterValueEditorOnly(FName("Tiling_Factor"), 20.0f);

        MIC->PostEditChange();
        MIC->MarkPackageDirty();
    }

    return MIC;
}

TArray<FString> AWoWMapImporterActor::GetTexturePathsFromList(const FIntPoint& Tile)
{
    TArray<FString> FoundTextures;
    FString FilePath = FString::Printf(TEXT("E:/WoW_Export/maps/%s/textures/file_list.txt"), *MapName);

    TArray<FString> Lines;
    if (FFileHelper::LoadFileToStringArray(Lines, *FilePath))
    {
        FString TargetHeader = FString::Printf(TEXT("--- %s_%d_%d ---"), *MapName, Tile.X, Tile.Y);
        bool bRecording = false;

        for (const FString& Line : Lines)
        {
            if (Line.Contains(TargetHeader)) { bRecording = true; continue; }
            if (bRecording)
            {
                if (Line.StartsWith(TEXT("---")) || Line.IsEmpty()) break;
                FoundTextures.Add(Line.TrimStartAndEnd());
            }
        }
    }
    return FoundTextures;
}

FString AWoWMapImporterActor::ConvertWoWPathToUnreal(FString WoWPath)
{
    // 1. Clean up slashes and remove spaces from the entire string
    FString CWUCleanPath = WoWPath.Replace(TEXT("\\"), TEXT("/")).Replace(TEXT(" "), TEXT(""));

    // 2. Extract components
    // BaseName: "BadlandsRock"
    FString CWUBaseName = FPaths::GetBaseFilename(CWUCleanPath);

    // FolderPath: "Tileset/TheBadlands"
    FString CWUFolderPath = FPaths::GetPath(CWUCleanPath);

    // 3. Build the final Unreal Reference
    // Result: "/Game/Tileset/TheBadlands/BadlandsRock.BadlandsRock"
    FString CWUFinalPath = FString::Printf(TEXT("/Game/%s/%s.%s"), *CWUFolderPath, *CWUBaseName, *CWUBaseName);

    return CWUFinalPath;
}

void AWoWMapImporterActor::ApplyMaterialToActor(FString InActorLabel, UMaterialInterface* Mat)
{
    for (TActorIterator<ADynamicMeshActor> It(GetWorld()); It; ++It)
    {
        if (It->GetActorLabel() == InActorLabel)
        {
            It->GetDynamicMeshComponent()->SetMaterial(0, Mat);
            break;
        }
    }
}


void AWoWMapImporterActor::ImportM2Doodads(const FIntPoint& Tile)
{
    FString ADTPath = FPaths::Combine(WorkingDirectory, FString::Printf(TEXT("%s_%d_%d.adt"), *MapName, Tile.X, Tile.Y));
    TArray<uint8> RawData;
    if (!FFileHelper::LoadFileToArray(RawData, *ADTPath)) return;

    FMemoryReader Reader(RawData);
    TArray<FString> M2Paths;

    // Progress Bar setup
    FScopedSlowTask Progress(100.0f, FText::FromString("Importing M2 Doodads..."));
    Progress.MakeDialog();

    float wowtilesize = (1600.0f / 3.0f);
    float woworigin = wowtilesize * 32.0f;
    

    while (!Reader.AtEnd())
    {
        uint32 ChunkID;
        uint32 ChunkSize;
        Reader << ChunkID;
        Reader << ChunkSize;
        int64 NextChunk = Reader.Tell() + ChunkSize;

        // Convert 4-byte ID to String (e.g., 'X DMM' -> "MMDX")
        ANSICHAR IdChars[5] = { (ANSICHAR)(ChunkID & 0xFF), (ANSICHAR)((ChunkID >> 8) & 0xFF), (ANSICHAR)((ChunkID >> 16) & 0xFF), (ANSICHAR)((ChunkID >> 24) & 0xFF), '\0' };
        FString Tag(IdChars);
        // Reverse because of Endianness
        Tag = Tag.Reverse();

        if (Tag == "MMDX")
        {
            TArray<uint8> StringBlock;
            StringBlock.AddUninitialized(ChunkSize);
            Reader.Serialize(StringBlock.GetData(), ChunkSize);

            FString Current;
            for (uint8 Byte : StringBlock)
            {
                if (Byte == 0) {
                    if (!Current.IsEmpty()) M2Paths.Add(Current);
                    Current = "";
                }
                else {
                    Current.AppendChar((ANSICHAR)Byte);
                }
            }
        }
        else if (Tag == "MDDF")
        {
            int32 EntryCount = ChunkSize / 36;
            for (int i = 0; i < EntryCount; i++)
            {
                uint32 NameID, UniqueID;
                float pX, pY, pZ; // Raw floats for position
                float rX, rY, rZ; // Raw floats for rotation
                uint16 Scale, Flags;

                // Read exactly 36 bytes per entry
                Reader << NameID;
                Reader << UniqueID;
                Reader << pX; Reader << pY; Reader << pZ; // Position
                Reader << rX; Reader << rY; Reader << rZ; // Rotation
                Reader << Scale;
                Reader << Flags;

                // UE_LOG(LogTemp, Warning, TEXT("M2 Raw Pos: %f, %f, %f | Rot: %f, %f, %f | Scale: %d"), pX, pY, pZ, rX, rY, rZ, Scale);

                if (M2Paths.IsValidIndex(NameID))
                {
                    // ***** POSITION *****
                    // Raw values from MDDF (pX, pY, pZ)
                    float meshueX = pZ;
                    float meshueY = pX;
                    float meshueZ = pY;

                    float UnrealX = (-woworigin + meshueX) * 100.0f;
                    float UnrealY = (woworigin - meshueY) * 100.0f;
                    float UnrealZ = meshueZ * 100.0f;

                    FVector FinalPos(UnrealX, UnrealY, UnrealZ);
                    
                    // ***** ROTATION *****
                    // 1. Create the 'Stand Up' base (The 90-degree Roll that worked manually)
                    FQuat StandUpQuat = FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(-90.0f));

                    // 2. Create the 'Spin' (Your 12.5 degree Yaw)
                    // Note: We use the raw values from the MDDF chunk
                    FQuat WoW_Roll = FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(-rZ)); //rX
                    FQuat WoW_Pitch = FQuat(FVector(0, 1, 0), FMath::DegreesToRadians(rX)); //rY
                    FQuat WoW_Yaw = FQuat(FVector(0, 0, 1), FMath::DegreesToRadians(-rY));   //rZ

                    // 3. Combine them in the correct World Order
                    // We multiply from right to left (Base first, then Lean, then Spin)
                    FQuat FinalQuat = WoW_Yaw * WoW_Pitch * WoW_Roll * StandUpQuat;

                    // 4. Convert to Rotator
                    FRotator FinalRot = FinalQuat.Rotator();

                    // ***** Scale *****
                    float FinalScale = (float)Scale / 1024.0f * 100.0f;

                    // Now pass this to your spawn function
                    SpawnM2Doodad(M2Paths[NameID], FinalPos, FinalRot, FVector(FinalScale));
                }
            }
        }
        Reader.Seek(NextChunk);
    }
}

void AWoWMapImporterActor::SpawnM2Doodad(FString WoWPath, FVector Loc, FRotator Rot, FVector Scale)
{
    // Convert WoW path to Unreal Path
    FString UnrealPath = ConvertWoWPathToUnreal(WoWPath);
    FString MeshName = FPaths::GetBaseFilename(WoWPath);

    // Try to load the mesh. LoadObject is fast if already in memory.
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *UnrealPath);
    bool bIsMissing = (Mesh == nullptr);

    if (bIsMissing)
    {
        // --- RECORD THE MISS ---
        // This will create/update the MapName_M2 table in /Game/maps/MapName/
        RecordMissingM2(WoWPath);

        // Use a basic Engine cube as a placeholder
        Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    }

    // Spawn the Actor
    FActorSpawnParameters SpawnParams;
    AStaticMeshActor* NewDoodad = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Loc, Rot, SpawnParams);

    if (NewDoodad && Mesh)
    {
        UStaticMeshComponent* SMC = NewDoodad->GetStaticMeshComponent();
        SMC->SetStaticMesh(Mesh);
        NewDoodad->SetActorScale3D(Scale);

        // Finalize
        SMC->UpdateBounds();
        
        // Use the "MISSING_" prefix so the Update function can find them later
        FString Label = bIsMissing ? FString::Printf(TEXT("MISSING_%s"), *MeshName) : MeshName;
        NewDoodad->SetActorLabel(Label);
        NewDoodad->SetFolderPath(FName(TEXT("Doodads")));

        // --- APPLY TWO-SIDED MATERIAL ---
        // Would you like the code here to apply the alpha-transparency material?
        // Maybe in another time in the future!!
    }
}

void AWoWMapImporterActor::RecordMissingM2(FString WoWPath)
{
    FString PackagePath = FString::Printf(TEXT("/Game/maps/%s/%s_M2"), *MapName, *MapName);
    UDataTable* Table = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *PackagePath));

    if (!Table)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        Table = NewObject<UDataTable>(Package, FName(*(MapName + "_M2")), RF_Public | RF_Standalone);
        Table->RowStruct = FMissingM2Row::StaticStruct();
        FAssetRegistryModule::AssetCreated(Table);
    }

    if (Table->RowStruct == nullptr)
    {
        Table->RowStruct = FMissingM2Row::StaticStruct();
        // This prevents the "corrupted" state from persisting 
        // by marking the file as 'Needs Saving' with the new fix.
        Table->MarkPackageDirty();

    }

    FName RowName = FName(*FPaths::GetBaseFilename(WoWPath));
    if (!Table->FindRow<FMissingM2Row>(RowName, ""))
    {
        FMissingM2Row NewRow;
        NewRow.FullFilePath = WoWPath;
        NewRow.ChosenMesh = nullptr;
        Table->AddRow(RowName, NewRow);
        Table->MarkPackageDirty();
    }
}

void AWoWMapImporterActor::UpdateM2Doodads()
{
    FString PackagePath = FString::Printf(TEXT("/Game/maps/%s/%s_M2"), *MapName, *MapName);
    UDataTable* Table = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *PackagePath));

    // 1. Check if table exists
    if (!Table)
    {
        UE_LOG(LogTemp, Warning, TEXT("M2 Table not found. No Doodads imports or tile without Doodads."));
        return;
    }

    if (Table->RowStruct == nullptr)
    {
        Table->RowStruct = FMissingM2Row::StaticStruct();

        // This prevents the "corrupted" state from persisting 
        // by marking the file as 'Needs Saving' with the new fix.
        Table->MarkPackageDirty();
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AStaticMeshActor::StaticClass(), AllActors);
    TArray<FName> RowsToDelete;

    // 2. Iterate through table
    Table->ForeachRow<FMissingM2Row>(TEXT("Scanning"), [&](const FName& Key, const FMissingM2Row& Row)
        {
            if (Row.ChosenMesh)
            {
                FString SearchPattern = FString::Printf(TEXT("MISSING_%s"), *Key.ToString());
                bool bFoundAtLeastOne = false;

                for (AActor* Actor : AllActors)
                {
                    if (Actor->GetActorLabel().StartsWith(SearchPattern))
                    {
                        AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor);
                        SMA->GetStaticMeshComponent()->SetStaticMesh(Row.ChosenMesh);
                        SMA->GetStaticMeshComponent()->UpdateBounds();
                        SMA->SetActorLabel(Key.ToString());
                        bFoundAtLeastOne = true;
                    }
                }
                if (bFoundAtLeastOne) RowsToDelete.Add(Key);
            }
        });

    // 3. Cleanup and Log result
    for (FName RowKey : RowsToDelete)
    {
        Table->RemoveRow(RowKey);
    }

    Table->MarkPackageDirty();
    UE_LOG(LogTemp, Log, TEXT("M2 Update Complete. Fixed %d types."), RowsToDelete.Num());
}










void AWoWMapImporterActor::ImportWMOs(const FIntPoint& Tile) {}