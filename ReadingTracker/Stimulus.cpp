// Fill out your copyright notice in the Description page of Project Settings.

#include "Stimulus.h"

#undef ERROR
#undef UpdateResource

#include "SRanipalEye_FunctionLibrary.h"
#include "SRanipalEye_Framework.h"
#include "Engine.h"
#include "Json.h"


AStimulus::AStimulus()
{
	PrimaryActorTick.bCanEverTick = true;
	
	mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = mesh;
    m_aspect = 1.0f;
    m_scaleX = 1.0f;
    m_scaleY = 1.0f;
    m_needsUpdate = false;
    m_camera = nullptr;
}

void AStimulus::BeginPlay()
{
	Super::BeginPlay();

    m_camera = UGameplayStatics::GetPlayerController(GetWorld(), 0)->PlayerCameraManager;

    SRanipalEye_Framework::Instance()->StartFramework(EyeVersion);
	
	initWS();

    m_dynTex = mesh->CreateAndSetMaterialInstanceDynamic(0);

    /*for (TActorIterator<AStaticMeshActor> Itr(GetWorld()); Itr; ++Itr)
    {
        if (Itr->GetName() == "Sphere_2")
        {
            m_pointer = *Itr;
            break;
        }
    }*/
}

void AStimulus::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    SRanipalEye_Framework::Instance()->StopFramework();

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
        TSharedPtr<FJsonObject> jsonParsed;
        TSharedRef<TJsonReader<TCHAR>> jsonReader = TJsonReaderFactory<TCHAR>::Create(text.c_str());
        if (FJsonSerializer::Deserialize(jsonReader, jsonParsed))
        {
            FString image = jsonParsed->GetStringField("image");
            TArray<uint8> img;
            FString png = "data:image/png;base64,";
            FString jpg = "data:image/jpeg;base64,";
            EImageFormat fmt = EImageFormat::Invalid;
            int startPos = 0;
            if (image.StartsWith(png))
            {
                fmt = EImageFormat::PNG;
                startPos = png.Len();
            }
            else if (image.StartsWith(jpg))
            {
                fmt = EImageFormat::JPEG;
                startPos = jpg.Len();
            }
            if (fmt != EImageFormat::Invalid && FBase64::Decode(&(image.GetCharArray()[startPos]), img))
                this->updateDynTex(img, fmt, jsonParsed->GetNumberField("scaleX"), jsonParsed->GetNumberField("scaleY"));
        }
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

    FFocusInfo focusInfo;
    FVector gazeOrigin, gazeTarget;
    bool hit = USRanipalEye_FunctionLibrary::Focus(GazeIndex::COMBINE, 1000.0f, 1.0f, m_camera, ECollisionChannel::ECC_WorldStatic, focusInfo, gazeOrigin, gazeTarget);
    if (hit && focusInfo.actor == this)
    {
        FVector actorOrigin, actorExtent;
        //UE_LOG(LogTemp, Warning, TEXT("HIT: %d || pos %f %f %f"), hit, focusInfo.point.X, focusInfo.point.Y, focusInfo.point.Z);
        GetActorBounds(true, actorOrigin, actorExtent, false);
        float u = ((focusInfo.point.X - actorOrigin.X) / actorExtent.X + 1.0f) / 2.0f;
        float v = ((focusInfo.point.Z - actorOrigin.Z) / actorExtent.Z + 1.0f) / 2.0f;
        /*((AStaticMeshActor*)m_pointer)->SetActorLocation(focusInfo.point);*/
        //UE_LOG(LogTemp, Warning, TEXT("pos %f %f // %f %f %f // %f %f %f // %f %f %f"), u, v, actorOrigin.X, actorOrigin.Y, actorOrigin.Z, actorExtent.X, actorExtent.Y, actorExtent.Z, focusInfo.point.X, focusInfo.point.Y, focusInfo.point.Z);
        string msg = to_string(u) + " " + to_string(v);
        for (auto& connection : m_server.get_connections())
            connection->send(msg);
    }


    if (m_needsUpdate)
    {
        lock_guard<mutex> lock(m_mutex);
        mesh->SetRelativeScale3D(FVector(m_aspect * 1.218 * m_scaleX, 1.218 * m_scaleY, 1.0));
        m_needsUpdate = false;
    }

}

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

void AStimulus::updateDynTex(const TArray<uint8>& img, EImageFormat fmt, float sx, float sy)
{
    int w, h;
    UTexture2D* tex = loadTexture2DFromBytes(img, fmt, w, h);
    if (tex)
    {
        m_dynTex->SetTextureParameterValue(FName(TEXT("DynTex")), tex);
        {
            lock_guard<mutex> lock(m_mutex);
            m_aspect = (float)w / (float)h;
            m_scaleX = sx;
            m_scaleY = sy;
            m_needsUpdate = true;
        }
    }
}
