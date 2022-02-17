// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RTHelpers.generated.h"


UENUM(BlueprintType)
enum class ERTHelpersImageFormats: uint8
{
    JPG  UMETA(DisplayName="JPG        "),
    PNG  UMETA(DisplayName="PNG        "),
    BMP  UMETA(DisplayName="BMP        "),
    ICO  UMETA(DisplayName="ICO        "),
    EXR  UMETA(DisplayName="EXR        "),
    ICNS UMETA(DisplayName="ICNS        ")
};

UCLASS()
class READINGTRACKER_API URTHelpers : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
    UFUNCTION(BlueprintPure, Category = "Helpers|Load Texture From File",meta=(Keywords="image png jpg jpeg bmp bitmap ico icon exr icns"))
    static UTexture2D *loadTexture2DFromFile(const FString &fullFilePath, ERTHelpersImageFormats imageFormat, bool &isValid, int32 &width, int32 &height);
};
