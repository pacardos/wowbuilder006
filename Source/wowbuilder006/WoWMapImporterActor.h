#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoWMapImporterActor.generated.h"

UCLASS()
class WOWBUILDER006_API AWoWMapImporterActor : public AActor
{
    GENERATED_BODY()

public:
    AWoWMapImporterActor();

#if WITH_EDITORONLY_DATA

    UPROPERTY(EditAnywhere, Category = "WoW Importer")
    FFilePath WDTFile;

#endif

#if WITH_EDITOR

    UFUNCTION(CallInEditor, Category = "WoW Importer")
    void LoadWDT();

#endif

private:

    FString MapName;
    FString WorkingDirectory;

    TSet<FIntPoint> AvailableADTs;
    TSet<FIntPoint> SelectedADTs;

    bool bGenerateDynamicMesh = true;
    bool bParseWater = false;
    bool bParseTextures = false;
    bool bParseM2 = false;
    bool bParseWMO = false;

private:

    FString GetTagString(uint32 Tag);
    
    void ParseWDT(const TArray<uint8>& Data);
    void ShowTileGrid();

    void ParseSelectedTiles();

    void ImportTerrainOBJ(const FIntPoint& Tile);
    void LoadOBJTile(const FString& OBJPath, const FIntPoint& Tile);

    void ImportWater(const FIntPoint& Tile);

    void ImportTextures(const FIntPoint& Tile);
    UMaterialInstanceConstant* CreateOrGetMaterialInstance(const FString& Name, const FString& Folder);
    TArray<FString> GetTexturePathsFromList(const FIntPoint& Tile);
    FString ConvertWoWPathToUnreal(FString WoWPath);
    void ApplyMaterialToActor(FString InActorLabel, UMaterialInterface* Mat);

    void ImportM2Doodads(const FIntPoint& Tile);
    void SpawnM2Doodad(FString AssetPath, FVector Loc, FRotator Rot, FVector Scale);






    void ImportWMOs(const FIntPoint& Tile);
};