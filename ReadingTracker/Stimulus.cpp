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
    m_calibIndex = 0;
    
    m_stimulusW = 0;
    m_stimulusH = 0;
    m_activeAOI = -1;
    m_inSelectionMode = false;
    m_rReleased = false;
    m_imgUpdated = false;

    m_camera = nullptr;

    m_needsCustomCalib = false;
    m_customCalibSamples = 0;
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

    auto &ep = m_server.endpoint["^/ue4/?$"];

    ep.on_message = [this](shared_ptr<WSServer::Connection> connection, shared_ptr<WSServer::InMessage> msg)
    {
        auto text = msg->string();
        TSharedPtr<FJsonObject> jsonParsed;
        TSharedRef<TJsonReader<TCHAR>> jsonReader = TJsonReaderFactory<TCHAR>::Create(text.c_str());
        if (FJsonSerializer::Deserialize(jsonReader, jsonParsed))
        {
            if (jsonParsed->TryGetField("calibrate"))
                calibrate();
            else if (jsonParsed->TryGetField("customCalibrate"))
                customCalibrate();
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

    ep.on_close = [](shared_ptr<WSServer::Connection> connection, int status, const string &)
    {
        UE_LOG(LogTemp, Display, TEXT("WebSocket: Closed"));
    };

    ep.on_handshake = [](shared_ptr<WSServer::Connection>, SimpleWeb::CaseInsensitiveMultimap &)
    {
        return SimpleWeb::StatusCode::information_switching_protocols;
    };

    ep.on_error = [](shared_ptr<WSServer::Connection> connection, const SimpleWeb::error_code &ec)
    {
        UE_LOG(LogTemp, Warning, TEXT("WebSocket: Error"));
    };

    m_serverThread = thread(&AStimulus::wsRun, this);
}

void AStimulus::wsRun()
{
    m_server.start();
}

FVector AStimulus::billboardToScene(const FVector2D &pos) const
{
    FVector actorOrigin, actorExtent;
    GetActorBounds(true, actorOrigin, actorExtent, false);
    return FVector((2.0f * (1.0f - pos.X) - 1.0f) * actorExtent.X + actorOrigin.X,
                   actorOrigin.Y,
                   (2.0f * (1.0f - pos.Y) - 1.0f) * actorExtent.Z + actorOrigin.Z);
}

FVector2D AStimulus::sceneToBillboard(const FVector &pos) const
{
    FVector actorOrigin, actorExtent;
    GetActorBounds(true, actorOrigin, actorExtent, false);
    return FVector2D(1.0f - ((pos.X - actorOrigin.X) / actorExtent.X + 1.0f) / 2.0f,
                     1.0f - ((pos.Z - actorOrigin.Z) / actorExtent.Z + 1.0f) / 2.0f);
}

bool AStimulus::pointInTriangle(const FVector2D &p, const FVector2D &a, const FVector2D &b, const FVector2D &c) const
{
    #define calcSign(p1, p2, p3) ((p1.X - p3.X) * (p2.Y - p3.Y) - (p2.X - p3.X) * (p1.Y - p3.Y))
    float d1 = calcSign(p, a, b);
    float d2 = calcSign(p, b, c);
    float d3 = calcSign(p, c, a);
    #undef calcSign

    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(hasNeg && hasPos);
}

bool AStimulus::findTriangle(const FVector2D &gazeLoc, CalibPoint &cp1, CalibPoint &cp2, CalibPoint &cp3) const
{
    CalibPoint *p1 = nullptr;
    CalibPoint *p2 = nullptr;
    CalibPoint *p3 = nullptr;
    for (int i = 0, n = m_customCalibPoints.Num(); i < n; ++i)
    {
        float d = m_customCalibPoints[i].pDist(gazeLoc);
        if (!p1 || d < p1->pDist(gazeLoc))
        {
            p2 = p1;
            p1 = &m_customCalibPoints[i];
        }
        else if (!p2 || d < p2->pDist(gazeLoc))
        {
            p2 = &m_customCalibPoints[i];
        }
    }
    for (int i = 0, n = m_customCalibPoints.Num(); i < n; ++i)
    {
        if (&m_customCalibPoints[i] != p1 &&
            &m_customCalibPoints[i] != p2 &&
            pointInTriangle(gazeLoc, *p1, *p2, m_customCalibPoints[i]) &&
            (!p3 || m_customCalibPoints[i].pDist(gazeLoc) < p3->pDist(gazeLoc)))
        {
            p3 = &m_customCalibPoints[i];
        }
    }
    if (p3)
    {
        cp1 = *p1;
        cp2 = *p2;
        cp3 = *p3;
        return true;
    }
    return false;
}

FQuat AStimulus::barycentric(const FVector2D &gazeLoc, const CalibPoint &cp1, const CalibPoint &cp2, const CalibPoint &cp3) const
{
    #define Xv1 cp1.gazeXY.X
    #define Xv2 cp2.gazeXY.X
    #define Xv3 cp3.gazeXY.X
    #define Yv1 cp1.gazeXY.Y
    #define Yv2 cp2.gazeXY.Y
    #define Yv3 cp3.gazeXY.Y
    #define Px gazeLoc.X
    #define Py gazeLoc.Y
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

    FQuat result = cp1.qCorr * w0 + cp2.qCorr * w1 + cp3.qCorr * w2;
    result.Normalize();
    return result;
}

bool AStimulus::castRay(const FVector &origin, const FVector &ray, FVector &hitPoint) const
{
    FCollisionQueryParams traceParam = FCollisionQueryParams(FName("traceParam"), true, m_camera);
    traceParam.bTraceComplex = true;
    traceParam.bReturnPhysicalMaterial = false;
    FHitResult hitResult(ForceInit);
    bool result;

    if (HIT_RADIUS == 0.0f)
    {
        result = m_camera->GetWorld()->LineTraceSingleByChannel(hitResult, origin, ray,
                                                                    ECollisionChannel::ECC_WorldStatic, traceParam);
    }
    else
    {
        FCollisionShape sph = FCollisionShape();
        sph.SetSphere(HIT_RADIUS);
        result = m_camera->GetWorld()->SweepSingleByChannel(hitResult, origin, ray, FQuat(0.0f, 0.0f, 0.0f, 0.0f),
                                                            ECollisionChannel::ECC_WorldStatic, sph, traceParam);
    }

    if (result)
        hitPoint = hitResult.Location;

    return result;
}

FVector2D AStimulus::posForIdx(int idx) const
{
    if ((idx / POINTS_PER_ROW) % 2)
        idx += POINTS_PER_ROW - (idx % POINTS_PER_ROW);
    return FVector2D((idx % POINTS_PER_ROW) * END_POSITION / (POINTS_PER_ROW) + START_POSITION,
                     (idx / POINTS_PER_ROW) * END_POSITION / (ROWS_IN_PATTERN) + START_POSITION);
}

void AStimulus::applyCustomCalib(const FVector &gazeOrigin, const FVector &gazeTarget, const FVector2D &gazeLoc,
                                 FVector &correctedGazeTarget, bool &needsUpdateDynContour)
{
    if (m_needsCustomCalib)
    {
        m_customCalibPhase = CalibPhase::StartDecreases;
        m_customCalibSamples = 0;
        m_customCalibPoints.Empty();
        m_needsCustomCalib = false;
    }

    switch (m_customCalibPhase)
    {
        case CalibPhase::None:
        {
            correctedGazeTarget = gazeTarget;
            needsUpdateDynContour = false;
            break;
        }

        case CalibPhase::StartDecreases:
        {
            correctedGazeTarget = gazeTarget;
            needsUpdateDynContour = true;
            m_customCalibTarget.location = FVector2D(CENTER_POSTION, CENTER_POSTION);
            m_customCalibTarget.radius = map(m_customCalibSamples, 0, SAMPLES_TO_START, TARGET_MAX_RADIUS, TARGET_MIN_RADIUS);
            ++m_customCalibSamples;
            if (m_customCalibSamples == SAMPLES_TO_START)
            {
                m_customCalibSamples = 0;
                m_customCalibPhase = CalibPhase::StartMoves;
            }
            break;
        }

        case CalibPhase::StartMoves:
        {
            correctedGazeTarget = gazeTarget;
            needsUpdateDynContour = true;
            float pos = map(m_customCalibSamples, 0, SAMPLES_TO_START_MOVE, CENTER_POSTION, START_POSITION);
            ++m_customCalibSamples;
            if (m_customCalibSamples == SAMPLES_TO_START_MOVE)
            {
                m_customCalibTarget.location = FVector2D(START_POSITION, START_POSITION);
                m_customCalibSamples = 0;
                m_customCalibAccumReportedGaze = FVector(0.0f);
                m_customCalibAccumRealGaze = FVector(0.0f);
                m_customCalibAccumLocation = FVector2D(0.0f);
                m_customCalibPhase = CalibPhase::TargetDecreases;
            }
            else
            {
                m_customCalibTarget.location = FVector2D(pos, pos);
            }
            break;
        }

        case CalibPhase::TargetDecreases:
        {
            correctedGazeTarget = gazeTarget;
            needsUpdateDynContour = true;
            m_customCalibTarget.radius = map(m_customCalibSamples, 0, SAMPLES_TO_DECREASE, TARGET_MAX_RADIUS, TARGET_MIN_RADIUS);
            FVector reportedGaze = gazeTarget - gazeOrigin;
            reportedGaze.Normalize();
            FVector realGaze = billboardToScene(m_customCalibTarget.location) - gazeOrigin;
            realGaze.Normalize();
            if (FMath::RadiansToDegrees(acosf(FVector::DotProduct(reportedGaze, realGaze))) < OUTLIER_THRESHOLD)
            {
                if (m_customCalibSamples > SAMPLES_TO_REJECT)
                {
                    m_customCalibAccumReportedGaze += reportedGaze;
                    m_customCalibAccumReportedGaze.Normalize();
                    m_customCalibAccumRealGaze += realGaze;
                    m_customCalibAccumRealGaze.Normalize();
                    m_customCalibAccumLocation += gazeLoc;
                }
                ++m_customCalibSamples;
            }
            else
            {
                m_customCalibSamples = 0;
                m_customCalibAccumReportedGaze = FVector(0.0f);
                m_customCalibAccumRealGaze = FVector(0.0f);
                m_customCalibAccumLocation = FVector2D(0.0f);
            }
            if (m_customCalibSamples == SAMPLES_TO_DECREASE)
            {
                CalibPoint cp;
                cp.gazeXY = m_customCalibAccumLocation / (float)(SAMPLES_TO_DECREASE - SAMPLES_TO_REJECT);
                cp.qCorr = FQuat::FindBetween(m_customCalibAccumReportedGaze, m_customCalibAccumRealGaze);
                m_customCalibPoints.Add(cp);
                m_customCalibSamples = 0;
                m_customCalibPhase = CalibPhase::TargetMoves;
            }
            break;
        }

        case CalibPhase::TargetMoves:
        {
            correctedGazeTarget = gazeTarget;
            needsUpdateDynContour = true;
            int idx = m_customCalibPoints.Num();
            if (idx == POINTS_PER_ROW * ROWS_IN_PATTERN)
                m_customCalibPhase = CalibPhase::Done;
            else
            {
                FVector2D posTo = posForIdx(idx);
                ++m_customCalibSamples;
                if (m_customCalibSamples == SAMPLES_TO_MOVE)
                {
                    m_customCalibTarget.location = posTo;
                    m_customCalibSamples = 0;
                    m_customCalibAccumReportedGaze = FVector(0.0f);
                    m_customCalibAccumRealGaze = FVector(0.0f);
                    m_customCalibAccumLocation = FVector2D(0.0f);
                    m_customCalibPhase = CalibPhase::TargetDecreases;
                }
                else
                {
                    FVector2D posFrom = posForIdx(idx - 1);
                    m_customCalibTarget.location = FVector2D(map(m_customCalibSamples, 0, SAMPLES_TO_MOVE, posFrom.X, posTo.X),
                                                             map(m_customCalibSamples, 0, SAMPLES_TO_MOVE, posFrom.Y, posTo.Y));
                }
            }
            break;
        }

        case CalibPhase::Done:
        {
            CalibPoint cp1, cp2, cp3;
            if (findTriangle(gazeLoc, cp1, cp2, cp3))
            {
                FQuat corr = barycentric(gazeLoc, cp1, cp2, cp3);
                FVector reportedGazeOrigin, reportedGazeDirection;
                if (USRanipalEye_FunctionLibrary::GetGazeRay(GazeIndex::COMBINE, reportedGazeOrigin, reportedGazeDirection))
                {
                    FVector camLocation = m_camera->GetCameraLocation();
                    FRotator camRotation = m_camera->GetCameraRotation();
                    FVector gazeRay = (corr.RotateVector(camRotation.RotateVector(reportedGazeDirection)) * MAX_DISTANCE) + camLocation;
                    if (!castRay(camLocation, gazeRay, correctedGazeTarget))
                        correctedGazeTarget = gazeTarget;
                }
                else
                {
                    correctedGazeTarget = gazeTarget;
                }
            }
            else
            {
                correctedGazeTarget = gazeTarget;
            }
            needsUpdateDynContour = false;
            break;
        }
    }
}

bool AStimulus::focus(FVector &gazeOrigin, FVector &rawGazeTarget, FVector &correctedGazeTarget,
                      float &leftPupilDiam, float &rightPupilDiam, bool &needsUpdateDynContour)
{
    FFocusInfo focusInfo;
    FVector gazeTarget;
    bool hit = USRanipalEye_FunctionLibrary::Focus(GazeIndex::COMBINE, 1000.0f, 1.0f, m_camera, ECollisionChannel::ECC_WorldStatic, focusInfo, gazeOrigin, gazeTarget);
    if (hit && focusInfo.actor == this)
    {
        ViveSR::anipal::Eye::VerboseData vd;
        SRanipalEye_Core::Instance()->GetVerboseData(vd);

        rawGazeTarget = focusInfo.point;
        leftPupilDiam = vd.left.pupil_diameter_mm;
        rightPupilDiam = vd.right.pupil_diameter_mm;

        FVector2D gazeLoc((vd.right.gaze_direction_normalized.X + vd.left.gaze_direction_normalized.X) / 2.0f,
                          (vd.right.gaze_direction_normalized.Y + vd.left.gaze_direction_normalized.Y) / 2.0f);
        applyCustomCalib(gazeOrigin, rawGazeTarget, gazeLoc, correctedGazeTarget, needsUpdateDynContour);

        return true;
    }
    return false;
}

void AStimulus::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    FVector gazeOrigin, rawGazeTarget, correctedGazeTarget;
    float leftPupilDiam, rightPupilDiam;
    bool needsUpdateDynContour;
    bool hit = focus(gazeOrigin, rawGazeTarget, correctedGazeTarget, leftPupilDiam, rightPupilDiam, needsUpdateDynContour);
    if (hit)
    {
        FVector2D uv = sceneToBillboard(correctedGazeTarget);

        bool selected = false;

#ifdef EYE_DEBUG
        m_corrTarget = uv;
        m_rawTarget = sceneToBillboard(rawGazeTarget);
        FVector camHitPoint;
        FVector camLocation = m_camera->GetCameraLocation();
        FRotator camRotation = m_camera->GetCameraRotation();
        FVector gazeRay = camRotation.RotateVector(FVector::ForwardVector) * MAX_DISTANCE + camLocation;
        castRay(camLocation, gazeRay, camHitPoint);
        m_camTarget = sceneToBillboard(camHitPoint);
        
        int currentAOI = -1;
        needsUpdateDynContour = true;
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
            needsUpdateDynContour = true;
        }
#endif // EYE_DEBUG

        if (m_dynContour && needsUpdateDynContour)
            m_dynContour->UpdateResource();

        FDateTime t = FDateTime::Now();
        string msg = to_string(t.ToUnixTimestamp() * 1000 + t.GetMillisecond()) + " " +
                     to_string(uv.X) + " " + to_string(uv.Y) + " " +
                     to_string(gazeOrigin.X) + " " + to_string(gazeOrigin.Y) + " " + to_string(gazeOrigin.Z) + " " +
                     to_string(correctedGazeTarget.X) + " " + to_string(correctedGazeTarget.Y) + " " + to_string(correctedGazeTarget.Z) + " " +
                     to_string(leftPupilDiam) + " " + to_string(rightPupilDiam) + " " +
                     to_string(currentAOI);
        string msgToSend = msg + (selected ? " SELECT" : " LOOKAT");
        for (auto &connection : m_server.get_connections())
            connection->send(msgToSend);
        if (m_rReleased)
        {
            msgToSend = msg + " R_RELD";
            m_rReleased = false;
            for (auto &connection : m_server.get_connections())
                connection->send(msgToSend);
        }
        if (m_imgUpdated)
        {
            msgToSend = msg + " IMG_UP";
            m_imgUpdated = false;
            for (auto &connection : m_server.get_connections())
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
}

UTexture2D *AStimulus::loadTexture2DFromFile(const FString &fullFilePath)
{
    UTexture2D *loadedT2D = nullptr;

    IImageWrapperModule &imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
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

            void *textureData = loadedT2D->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(textureData, uncompressedBGRA.GetData(), uncompressedBGRA.Num());
            loadedT2D->PlatformData->Mips[0].BulkData.Unlock();

            loadedT2D->UpdateResource();
        }
    }

    return loadedT2D;
}

UTexture2D *AStimulus::loadTexture2DFromBytes(const TArray<uint8> &bytes, EImageFormat fmt, int &w, int &h)
{
    UTexture2D *loadedT2D = nullptr;

    IImageWrapperModule &imageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
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

            void *textureData = loadedT2D->PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(textureData, uncompressedBGRA.GetData(), uncompressedBGRA.Num());
            loadedT2D->PlatformData->Mips[0].BulkData.Unlock();

            loadedT2D->UpdateResource();
        }
    }

    return loadedT2D;
}

void AStimulus::updateDynTex(const TArray<uint8> &img, EImageFormat fmt, float sx, float sy, const TArray<TSharedPtr<FJsonValue>> &aois)
{
    int w, h;
    UTexture2D *tex = loadTexture2DFromBytes(img, fmt, w, h);
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

void AStimulus::strokeCircle(UCanvas *cvs, const FVector2D &pos, float radius, float thickness, const FLinearColor &color) const
{
    const int n = 8;
    for (int i = 0; i < n; ++i)
    {
        cvs->K2_DrawLine(FVector2D(pos.X + radius * cos((float)i / (float)(n - 1) * 2.0f * PI),
                                   pos.Y + radius * sin((float)i / (float)(n - 1) * 2.0f * PI)),
                         FVector2D(pos.X + radius * cos((float)(i + 1) / (float)(n - 1) * 2.0f * PI),
                                   pos.Y + radius * sin((float)(i + 1) / (float)(n - 1) * 2.0f * PI)),
                         thickness, color);
    }
}

void AStimulus::fillCircle(UCanvas *cvs, const FVector2D &pos, float radius) const
{
    cvs->K2_DrawPolygon(nullptr, pos, FVector2D(radius), 16, FLinearColor(0, 0, 0, 1));
}

void AStimulus::drawContourOfAOI(UCanvas *cvs, const FLinearColor &color, float th, int aoi) const
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
    strokeCircle(cvs, FVector2D(m_rawTarget.X * m_stimulusW, m_rawTarget.Y * m_stimulusH), 2.0f * th, th, FLinearColor(1, 0, 0, 1));
    strokeCircle(cvs, FVector2D(m_corrTarget.X * m_stimulusW, m_corrTarget.Y * m_stimulusH), 2.0f * th, th, FLinearColor(0, 1, 0, 1));
    strokeCircle(cvs, FVector2D(m_camTarget.X * m_stimulusW, m_camTarget.Y * m_stimulusH), 2.0f * th, th, FLinearColor(1, 0, 1, 1));
#else
    float th = max(round((float)max(m_stimulusW, m_stimulusH) * 0.0025f), 1.0f);
    for (auto aoi : m_selectedAOIs)
        drawContourOfAOI(cvs, FLinearColor(0, 0.2, 0, 1), th, aoi);
    if (m_activeAOI != -1)
        drawContourOfAOI(cvs, FLinearColor(1, 0, 0, 1), th, m_activeAOI);
#endif // EYE_DEBUG

    if (m_customCalibPhase != CalibPhase::None && m_customCalibPhase != CalibPhase::Done)
        fillCircle(cvs, FVector2D(m_customCalibTarget.location.X * m_stimulusW, m_customCalibTarget.location.Y * m_stimulusH), m_customCalibTarget.radius);
}

bool AStimulus::pointInPolygon(const FVector2D &pt, const TArray<FVector2D> &poly) const
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

bool AStimulus::pointInBBox(const FVector2D &pt, const BBox &bbox) const
{
    return pt.X >= bbox.lt.X && pt.Y >= bbox.lt.Y && pt.X <= bbox.rb.X && pt.Y <= bbox.rb.Y;
}

bool AStimulus::hitTest(const FVector2D &pt, const AOI &aoi) const
{
    return pointInBBox(pt, aoi.bbox) && pointInPolygon(pt, aoi.path);
}

int AStimulus::findActiveAOI(const FVector2D &pt) const
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

void AStimulus::customCalibrate()
{
    m_needsCustomCalib = true;
}
