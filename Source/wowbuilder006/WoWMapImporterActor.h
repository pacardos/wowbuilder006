#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// data table of missing meshes
#include "Engine/DataTable.h"
#include "WoWMapImporterActor.generated.h"



USTRUCT(BlueprintType)
struct FMissingM2Row : public FTableRowBase
{
    GENERATED_USTRUCT_BODY() // Use this for maximum stability with DataTables

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoW")
    FString FullFilePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoW")
    UStaticMesh* ChosenMesh = nullptr;
};



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
    void RecordMissingM2(FString WoWPath);
    void UpdateM2Doodads();





    void ImportWMOs(const FIntPoint& Tile);
};