// Fill out your copyright notice in the Description page of Project Settings.

#include "Stimulus.h"

#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"


AStimulus::AStimulus()
{
	PrimaryActorTick.bCanEverTick = true;
	
	mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = mesh;
}

void AStimulus::BeginPlay()
{
	Super::BeginPlay();
	
	initWS();

    UTexture2D *tex = loadTexture2DFromFile("C:\\Users\\User\\Documents\\shaka.001.png");
    UMaterialInterface* myMatI = mesh->GetMaterial(0);

    UMaterialInstanceDynamic* dyn = mesh->CreateAndSetMaterialInstanceDynamic(0);
    dyn->SetTextureParameterValue(FName(TEXT("DynTex")), tex);

    //mesh->SetRelativeScale3D(FVector(16.0 / 9.0, 1.0, 1.0));
}

void AStimulus::initWS()
{
}

void AStimulus::wsRun()
{
	m_server.start();
}

void AStimulus::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

#undef UpdateResource
UTexture2D* AStimulus::loadTexture2DFromFile(const FString& fullFilePath)
{
    UTexture2D* loadedT2D = nullptr;

    IImageWrapperModule& imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> imageWrapper = imageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    TArray<uint8> rawFileData;
    if (!FFileHelper::LoadFileToArray(rawFileData, *fullFilePath))
        return nullptr;

    if (imageWrapper.IsValid() && imageWrapper->SetCompressed(rawFileData.GetData(), rawFileData.Num()))
    {
        TArray<uint8> uncompressedBGRA;
        if (imageWrapper->GetRaw(ERGBFormat::BGRA, 8, uncompressedBGRA))
        {
            loadedT2D = UTexture2D::CreateTransient(imageWrapper->GetWidth(), imageWrapper->GetHeight(), PF_B8G8R8A8);
            if (!loadedT2D)
                return nullptr;

            void* textureData = loadedT2D->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(textureData, uncompressedBGRA.GetData(), uncompressedBGRA.Num());
            loadedT2D->PlatformData->Mips[0].BulkData.Unlock();

            loadedT2D->UpdateResource();
        }
    }

    return loadedT2D;
}



