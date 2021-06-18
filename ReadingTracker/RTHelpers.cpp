// Fill out your copyright notice in the Description page of Project Settings.


#include "RTHelpers.h"

#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"


UTexture2D *URTHelpers::loadTexture2DFromFile(const FString &fullFilePath, ERTHelpersImageFormats imageFormat,
                                              bool &isValid, int32 &width, int32 &height)
{
    isValid = false;
    UTexture2D *loadedT2D = nullptr;

    EImageFormat fmt;
    switch (imageFormat)
    {
        case ERTHelpersImageFormats::JPG:
            fmt = EImageFormat::JPEG;
            break;

        case ERTHelpersImageFormats::PNG:
            fmt = EImageFormat::PNG;
            break;

        case ERTHelpersImageFormats::BMP:
            fmt = EImageFormat::BMP;
            break;

        case ERTHelpersImageFormats::ICO:
            fmt =  EImageFormat::ICO;
            break;

        case ERTHelpersImageFormats::EXR:
            fmt =  EImageFormat::EXR;
            break;

        case ERTHelpersImageFormats::ICNS:
            fmt = EImageFormat::ICNS;
            break;
    }

    IImageWrapperModule &imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> imageWrapper = imageWrapperModule.CreateImageWrapper(fmt);

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

            width = imageWrapper->GetWidth();
            height = imageWrapper->GetHeight();

            void *textureData = loadedT2D->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(textureData, uncompressedBGRA.GetData(), uncompressedBGRA.Num());
            loadedT2D->PlatformData->Mips[0].BulkData.Unlock();

            loadedT2D->UpdateResource();
        }
    }

    isValid = true;
    return loadedT2D;
}
