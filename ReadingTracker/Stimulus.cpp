// Fill out your copyright notice in the Description page of Project Settings.

#include "Stimulus.h"

#include "SRanipalEye_FunctionLibrary.h"
#include "SRanipalEye_Framework.h"
#include "SRanipal_API_Eye.h"
#include "SRanipalEye_Core.h"
#include "Engine.h"
#include "Json.h"
#include "IXRTrackingSystem.h"


AStimulus::AStimulus()
{
	PrimaryActorTick.bCanEverTick = true;
	
	mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = mesh;

    m_aspect = 1.0f;
    m_scaleX = 1.0f;
    m_scaleY = 1.0f;
    m_dynTexW = 0;
    m_dynTexH = 0;
    m_needsUpdate = false;
    m_needsUpdateCalib = false;
    m_calibIndex = 0;
    
    m_stimulusW = 0;
    m_stimulusH = 0;
    m_activeAOI = -1;
    m_inSelectionMode = false;
    m_rReleased = false;
    m_imgUpdated = false;

    m_camera = nullptr;

#ifdef EYE_DEBUG
    m_u = m_v = m_cu = m_cv = 0.0f;
#endif // EYE_DEBUG
}

void AStimulus::BeginPlay()
{
	Super::BeginPlay();

    m_camera = UGameplayStatics::GetPlayerController(GetWorld(), 0)->PlayerCameraManager;

    SRanipalEye_Framework::Instance()->StartFramework(EyeVersion);
	
	initWS();

    m_dynTex = mesh->CreateAndSetMaterialInstanceDynamic(0);
    m_dynContour = nullptr;

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
            if (jsonParsed->TryGetField("calibrate"))
                calibrate();
            else
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
                    this->updateDynTex(img, fmt, jsonParsed->GetNumberField("scaleX"), jsonParsed->GetNumberField("scaleY"), jsonParsed->GetArrayField("AOIs"));
            }
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

    ///////////////////
    auto& epCalib = m_server.endpoint["^/calib/?$"];

    epCalib.on_message = [this](shared_ptr<WSServer::Connection> connection, shared_ptr<WSServer::InMessage> msg)
    {
        auto text = msg->string();
        TSharedPtr<FJsonObject> jsonParsed;
        TSharedRef<TJsonReader<TCHAR>> jsonReader = TJsonReaderFactory<TCHAR>::Create(text.c_str());
        if (FJsonSerializer::Deserialize(jsonReader, jsonParsed))
        {
            lock_guard<mutex> lock(m_mutex);
            auto points = jsonParsed->GetArrayField("data");
            m_calibBack.Empty();
            for (auto point : points)
            {
                m_calibBack.Add(CalibPt(point->AsArray()[0]->AsNumber(), point->AsArray()[2]->AsNumber(),
                                    point->AsArray()[1]->AsNumber(), point->AsArray()[3]->AsNumber()));
            }
            m_needsUpdateCalib = true;
        }
    };

    epCalib.on_open = [](shared_ptr<WSServer::Connection> connection)
    {
        UE_LOG(LogTemp, Display, TEXT("[calib] WebSocket: Opened"));
    };

    epCalib.on_close = [](shared_ptr<WSServer::Connection> connection, int status, const string&)
    {
        UE_LOG(LogTemp, Display, TEXT("[calib] WebSocket: Closed"));
    };

    epCalib.on_handshake = [](shared_ptr<WSServer::Connection>, SimpleWeb::CaseInsensitiveMultimap&)
    {
        return SimpleWeb::StatusCode::information_switching_protocols;
    };

    epCalib.on_error = [](shared_ptr<WSServer::Connection> connection, const SimpleWeb::error_code& ec)
    {
        UE_LOG(LogTemp, Warning, TEXT("[calib] WebSocket: Error"));
    };
    ///////////////////

    m_serverThread = thread(&AStimulus::wsRun, this);
}

void AStimulus::wsRun()
{
	m_server.start();
}

void AStimulus::applyCalib(float ca, float cb, float &u, float &v)
{
    if (m_calib.Num() > 0)
    {
        //UE_LOG(LogTemp, Warning, TEXT("apply calib"));
        m_calib.Sort([ca, cb](const CalibPt &cpt1, const CalibPt &cpt2) { return cpt1.pDist(ca, cb) < cpt2.pDist(ca, cb); });
#define Xv1 m_calib[0].cAlpha
#define Xv2 m_calib[1].cAlpha
#define Xv3 m_calib[2].cAlpha
#define Yv1 m_calib[0].cBeta
#define Yv2 m_calib[1].cBeta
#define Yv3 m_calib[2].cBeta
#define Px ca
#define Py cb
        float w0 = ((Yv2 - Yv3) * (Px - Xv3) + (Xv3 - Xv2) * (Py - Yv3)) / ((Yv2 - Yv3) * (Xv1 - Xv3) + (Xv3 - Xv2) * (Yv1 - Yv3));
        float w1 = ((Yv3 - Yv1) * (Px - Xv3) + (Xv1 - Xv3) * (Py - Yv3)) / ((Yv2 - Yv3) * (Xv1 - Xv3) + (Xv3 - Xv2) * (Yv1 - Yv3));
        float w2 = 1.0f - w0 - w1;
#undef Xv1
#undef Xv2
#undef Xv3
#undef Yv1
#undef Yv2
#undef Yv3
#undef Px
#undef Py
        u -= w0 * m_calib[0].errU + w1 * m_calib[1].errU + w2 * m_calib[2].errU;
        v -= w0 * m_calib[0].errV + w1 * m_calib[1].errV + w2 * m_calib[2].errV;
    }
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
        float u = 1.0f - ((focusInfo.point.X - actorOrigin.X) / actorExtent.X + 1.0f) / 2.0f;
        float v = 1.0f - ((focusInfo.point.Z - actorOrigin.Z) / actorExtent.Z + 1.0f) / 2.0f;
        /*((AStaticMeshActor*)m_pointer)->SetActorLocation(focusInfo.point);*/
        //UE_LOG(LogTemp, Warning, TEXT("pos %f %f // %f %f %f // %f %f %f // %f %f %f"), u, v, actorOrigin.X, actorOrigin.Y, actorOrigin.Z, actorExtent.X, actorExtent.Y, actorExtent.Z, focusInfo.point.X, focusInfo.point.Y, focusInfo.point.Z);
        ViveSR::anipal::Eye::VerboseData vd;
        SRanipalEye_Core::Instance()->GetVerboseData(vd);
        //UE_LOG(LogTemp, Warning, TEXT("pupil %f %f"), vd.left.pupil_diameter_mm, vd.right.pupil_diameter_mm);
        bool selected = false;

        FVector gazeVec = gazeTarget - gazeOrigin;
        gazeVec.Normalize();
        FQuat headOrientation = m_camera->GetCameraRotation().Quaternion();
        gazeVec = headOrientation.UnrotateVector(gazeVec);
        FVector gazeVecXY(gazeVec.X, gazeVec.Y, 0.0f);
        gazeVecXY.Normalize();
        FVector gazeVecXZ(gazeVec.X, 0.0f, gazeVec.Z);
        gazeVecXZ.Normalize();
        float cAlpha = FVector::DotProduct(gazeVecXY, FVector(0.0f, 1.0f, 0.0f));
        float cBeta = FVector::DotProduct(gazeVecXZ, FVector(0.0f, 0.0f, 1.0f));
        //GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, FString::Printf(TEXT("%f %f"), cAlpha, cBeta));

        GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, FString::Printf(TEXT("%f %f"), vd.right.pupil_position_in_sensor_area.X, vd.right.pupil_position_in_sensor_area.Y));
        
        if (m_rReleased && m_calibIndex < 9)
        {
            const FVector2D calibRefs[] = 
            {
                FVector2D(0.05, 0.05), FVector2D(0.5, 0.05), FVector2D(0.95, 0.05),
                FVector2D(0.05, 0.5), FVector2D(0.5, 0.5), FVector2D(0.95, 0.5),
                FVector2D(0.05, 0.95), FVector2D(0.5, 0.95), FVector2D(0.95, 0.95)
            };
            FVector2D realUV = calibRefs[m_calibIndex++];
            FVector realPos((2.0f * (1.0f - realUV.X) - 1.0f) * actorExtent.X + actorOrigin.X,
                            actorOrigin.Y,
                            (2.0f * (1.0f - realUV.Y) - 1.0f) * actorExtent.Z + actorOrigin.Z);
            FVector real = realPos - gazeOrigin;
            CalibQ cq;
            FVector gaze = gazeTarget - gazeOrigin;
            cq.qgaz = FQuat::FindBetween(headOrientation.GetForwardVector(), gaze);
            cq.qerr = FQuat::FindBetween(gaze, real);
            m_calibQ.Add(cq);
        }


        //applyCalib(cAlpha, cBeta, u, v);

#ifdef EYE_DEBUG
        m_u = u;
        m_v = v;
        /*applyCalib(cAlpha, cBeta, u, v);
        m_cu = u;
        m_cv = v;*/
        if (m_calibQ.Num() == 9)
        {
            m_calibQ.Sort();
        }
        FVector realGaze;
        
        float u = 1.0f - ((focusInfo.point.X - actorOrigin.X) / actorExtent.X + 1.0f) / 2.0f;
        float v = 1.0f - ((focusInfo.point.Z - actorOrigin.Z) / actorExtent.Z + 1.0f) / 2.0f;
        int currentAOI = -1;
        if (m_dynContour)
            m_dynContour->UpdateResource();
#else
        int currentAOI = findActiveAOI(FVector2D(u * m_stimulusW, v * m_stimulusH));
        int newAOI = m_inSelectionMode ? currentAOI : -1;
        if (m_activeAOI != newAOI && m_dynContour)
        {
            if (newAOI == -1 && !m_inSelectionMode)
            {
                selected = true;
                toggleSelectedAOI(m_activeAOI);
            }
            m_activeAOI = newAOI;
            m_dynContour->UpdateResource();
        }
#endif // EYE_DEBUG

        FDateTime t = FDateTime::Now();
        string msg = to_string(t.ToUnixTimestamp() * 1000 + t.GetMillisecond()) + " " +
                     to_string(u) + " " + to_string(v) + " " +
                     to_string(gazeOrigin.X) + " " + to_string(gazeOrigin.Y) + " " + to_string(gazeOrigin.Z) + " " +
                     to_string(focusInfo.point.X) + " " + to_string(focusInfo.point.Y) + " " + to_string(focusInfo.point.Z) + " " +
                     /*to_string(vd.left.pupil_diameter_mm) + " " + to_string(vd.right.pupil_diameter_mm) +*/
                     to_string(cAlpha) + " " + to_string(cBeta) + " " + to_string(currentAOI);
        string msgToSend = msg + (selected ? " SELECT" : " LOOKAT");
        for (auto& connection : m_server.get_connections())
            connection->send(msgToSend);
        if (m_rReleased)
        {
            msgToSend = msg + " R_RELD";
            m_rReleased = false;
            for (auto& connection : m_server.get_connections())
                connection->send(msgToSend);
        }
        if (m_imgUpdated)
        {
            msgToSend = msg + " IMG_UP";
            m_imgUpdated = false;
            for (auto& connection : m_server.get_connections())
                connection->send(msgToSend);
        }
    }

    if (m_needsUpdate)
    {
        lock_guard<mutex> lock(m_mutex);
        mesh->SetRelativeScale3D(FVector(m_aspect * 1.218 * m_scaleX, 1.218 * m_scaleY, 1.0));
        m_dynContour = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(GetWorld(), UCanvasRenderTarget2D::StaticClass(), m_dynTexW, m_dynTexH);
        m_dynContour->ClearColor = FLinearColor(0, 0, 0, 0);
        m_dynContour->OnCanvasRenderTargetUpdate.AddDynamic(this, &AStimulus::drawContour);
        m_dynTex->SetTextureParameterValue(FName(TEXT("ContourTex")), m_dynContour);
        m_stimulusW = m_dynTexW;
        m_stimulusH = m_dynTexH;
        m_aois = m_dynAOIs;
        m_activeAOI = -1;
        m_selectedAOIs.Empty();
        m_dynContour->UpdateResource();
        m_needsUpdate = false;
        m_imgUpdated = true;
    }

    if (m_needsUpdateCalib)
    {
        lock_guard<mutex> lock(m_mutex);
        m_calib = m_calibBack;
        m_needsUpdateCalib = false;
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

void AStimulus::updateDynTex(const TArray<uint8>& img, EImageFormat fmt, float sx, float sy, const TArray<TSharedPtr<FJsonValue>>& aois)
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
            m_dynTexW = w;
            m_dynTexH = h;
            m_dynAOIs.Empty();
            for (auto value : aois)
            {
                AOI aoi;
                auto nameField = value->AsObject()->TryGetField("name");
                if (nameField)
                    aoi.name = nameField->AsString();
                auto pathField = value->AsObject()->TryGetField("path");
                if (pathField)
                {
                    auto path = pathField->AsArray();
                    for (auto point : path)
                        aoi.path.Add(FVector2D(point->AsArray()[0]->AsNumber(), point->AsArray()[1]->AsNumber()));
                }
                auto bboxField = value->AsObject()->TryGetField("bbox");
                if (bboxField)
                {
                    auto bbox = bboxField->AsArray();
                    aoi.bbox.lt = FVector2D(bbox[0]->AsNumber(), bbox[1]->AsNumber());
                    aoi.bbox.rb = FVector2D(bbox[2]->AsNumber(), bbox[3]->AsNumber());
                }
                m_dynAOIs.Add(aoi);
            }
            m_needsUpdate = true;
        }
    }
}

void AStimulus::drawContourOfAOI(UCanvas* cvs, const FLinearColor& color, float th, int aoi) const
{
    FVector2D pt = m_aois[aoi].path[0];
    for (int i = 1, n = m_aois[aoi].path.Num(); i < n; ++i)
    {
        cvs->K2_DrawLine(pt, m_aois[aoi].path[i], th, color);
        pt = m_aois[aoi].path[i];
    }
    cvs->K2_DrawLine(pt, m_aois[aoi].path[0], th, color);
}

void AStimulus::drawContour(UCanvas *cvs, int32 w, int32 h)
{
#ifdef EYE_DEBUG
    float th = max(round((float)max(m_stimulusW, m_stimulusH) * 0.0025f), 1.0f);
    float x = m_u * m_stimulusW;
    float y = m_v * m_stimulusH;
    FLinearColor color = FLinearColor(1, 0, 0, 1);
    const int n = 8;
    for (int i = 0; i < n; ++i)
    {
        cvs->K2_DrawLine(FVector2D(x + 2.0f * th * cos((float)i / (float)(n - 1) * 2.0f * PI), y + 2.0f * th * sin((float)i / (float)(n - 1) * 2.0f * PI)),
            FVector2D(x + 2.0f * th * cos((float)(i + 1) / (float)(n - 1) * 2.0f * PI), y + 2.0f * th * sin((float)(i + 1) / (float)(n - 1) * 2.0f * PI)), th, color);
    }
    float cx = m_cu * m_stimulusW;
    float cy = m_cv * m_stimulusH;
    FLinearColor ccolor = FLinearColor(0, 1, 0, 1);
    
    for (int i = 0; i < n; ++i)
    {
        cvs->K2_DrawLine(FVector2D(cx + 2.0f * th * cos((float)i / (float)(n - 1) * 2.0f * PI), cy + 2.0f * th * sin((float)i / (float)(n - 1) * 2.0f * PI)),
            FVector2D(cx + 2.0f * th * cos((float)(i + 1) / (float)(n - 1) * 2.0f * PI), cy + 2.0f * th * sin((float)(i + 1) / (float)(n - 1) * 2.0f * PI)), th, ccolor);
    }
#else
    float th = max(round((float)max(m_stimulusW, m_stimulusH) * 0.0025f), 1.0f);
    for (auto aoi : m_selectedAOIs)
        drawContourOfAOI(cvs, FLinearColor(0, 0.2, 0, 1), th, aoi);
    if (m_activeAOI != -1)
        drawContourOfAOI(cvs, FLinearColor(1, 0, 0, 1), th, m_activeAOI);
#endif // EYE_DEBUG
}

bool AStimulus::pointInPolygon(const FVector2D& pt, const TArray<FVector2D>& poly) const
{
    bool result = false;
    int n = poly.Num();
    for (int i = 0, j = n - 1; i < n; j = i++)
    {
        if (((poly[i].Y > pt.Y) != (poly[j].Y > pt.Y)) && (pt.X < (poly[j].X - poly[i].X) * (pt.Y - poly[i].Y) / (poly[j].Y - poly[i].Y) + poly[i].X))
            result = !result;
    }
    return result;
}

bool AStimulus::pointInBBox(const FVector2D& pt, const BBox& bbox) const
{
    return pt.X >= bbox.lt.X && pt.Y >= bbox.lt.Y && pt.X <= bbox.rb.X && pt.Y <= bbox.rb.Y;
}

bool AStimulus::hitTest(const FVector2D& pt, const AOI& aoi) const
{
    return pointInBBox(pt, aoi.bbox) && pointInPolygon(pt, aoi.path);
}

int AStimulus::findActiveAOI(const FVector2D& pt) const
{
    for (int i = 0, n = m_aois.Num(); i < n; ++i)
    {
        if (hitTest(pt, m_aois[i]))
            return i;
    }
    return -1;
}

void AStimulus::toggleSelectedAOI(int aoi)
{
    if (m_selectedAOIs.Contains(aoi))
        m_selectedAOIs.Remove(aoi);
    else
        m_selectedAOIs.Add(aoi);
}

void AStimulus::trigger(bool isPressed)
{
    m_inSelectionMode = isPressed;
    if (!isPressed)
        m_rReleased = true;
}

void AStimulus::calibrate()
{
    ViveSR::anipal::Eye::LaunchEyeCalibration(nullptr);
}
