// Fill out your copyright notice in the Description page of Project Settings.

#include "Stimulus.h"


AStimulus::AStimulus()
{
	PrimaryActorTick.bCanEverTick = true;
	
	mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = mesh;
    m_aspect = 1.0f;
    m_needsUpdate = false;
}

void AStimulus::BeginPlay()
{
	Super::BeginPlay();
	
	initWS();

    m_dynTex = mesh->CreateAndSetMaterialInstanceDynamic(0);
}

void AStimulus::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    m_server.stop();
    m_serverThread.join();
}

void AStimulus::initWS()
{
    m_server.config.port = 81;

    auto& ep = m_server.endpoint["^/ue4/?$"];

    ep.on_message = [this](shared_ptr<WSServer::Connection> connection, shared_ptr<WSServer::InMessage> msg)
    {
        auto text = msg->string();
        TArray<uint8> img;
        string png = "data:image/png;base64,";
        string jpg = "data:image/jpeg;base64,";
        EImageFormat fmt = EImageFormat::Invalid;
        int startPos = 0;
        if (equal(png.begin(), png.end(), text.begin()))
        {
            fmt = EImageFormat::PNG;
            startPos = png.length();
        }
        else if (equal(jpg.begin(), jpg.end(), text.begin()))
        {
            fmt = EImageFormat::JPEG;
            startPos = jpg.length();
        }
        if (fmt != EImageFormat::Invalid && FBase64::Decode(&text.c_str()[startPos], img))
            this->updateDynTex(img, fmt);
    };

    ep.on_open = [](shared_ptr<WSServer::Connection> connection)
    {
        UE_LOG(LogTemp, Display, TEXT("WebSocket: Opened"));
    };

    ep.on_close = [](shared_ptr<WSServer::Connection> connection, int status, const string&)
    {
        UE_LOG(LogTemp, Display, TEXT("WebSocket: Closed"));
    };

    ep.on_handshake = [](shared_ptr<WSServer::Connection>, SimpleWeb::CaseInsensitiveMultimap&)
    {
        return SimpleWeb::StatusCode::information_switching_protocols;
    };

    ep.on_error = [](shared_ptr<WSServer::Connection> connection, const SimpleWeb::error_code& ec)
    {
        UE_LOG(LogTemp, Warning, TEXT("WebSocket: Error"));
    };

    m_serverThread = thread(&AStimulus::wsRun, this);
}

void AStimulus::wsRun()
{
	m_server.start();
}

void AStimulus::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

    if (m_needsUpdate)
    {
        lock_guard<mutex> lock(m_mutex);
        mesh->SetRelativeScale3D(FVector(m_aspect * 1.218, 1.218, 1.0));
        m_needsUpdate = false;
    }

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

UTexture2D* AStimulus::loadTexture2DFromBytes(const TArray<uint8>& bytes, EImageFormat fmt, int& w, int& h)
{
    UTexture2D* loadedT2D = nullptr;

    IImageWrapperModule& imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> imageWrapper = imageWrapperModule.CreateImageWrapper(fmt);

    if (imageWrapper.IsValid() && imageWrapper->SetCompressed(bytes.GetData(), bytes.Num()))
    {
        TArray<uint8> uncompressedBGRA;
        if (imageWrapper->GetRaw(ERGBFormat::BGRA, 8, uncompressedBGRA))
        {
            w = imageWrapper->GetWidth();
            h = imageWrapper->GetHeight();
            loadedT2D = UTexture2D::CreateTransient(w, h, PF_B8G8R8A8);
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

void AStimulus::updateDynTex(const TArray<uint8>& img, EImageFormat fmt)
{
    int w, h;
    UTexture2D* tex = loadTexture2DFromBytes(img, fmt, w, h);
    if (tex)
    {
        m_dynTex->SetTextureParameterValue(FName(TEXT("DynTex")), tex);
        {
            lock_guard<mutex> lock(m_mutex);
            m_aspect = (float)w / (float)h;
            m_needsUpdate = true;
            //mesh->SetRelativeScale3D(FVector(a, 1.0, 1.0));
        }
    }
}
