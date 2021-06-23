// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#define DEPRECATED
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#define ASIO_STANDALONE 1
#include "ws/server_ws.hpp"
THIRD_PARTY_INCLUDES_END
#undef UI

using namespace std;

using WSServer = SimpleWeb::SocketServer<SimpleWeb::WS>;

#include "SRanipal_Eyes_Enums.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Stimulus.generated.h"


UCLASS()
class READINGTRACKER_API AStimulus : public AActor
{
	GENERATED_BODY()
	
private:
	WSServer m_server;
	thread m_serverThread;
	UMaterialInstanceDynamic* m_dynTex;
	mutex m_mutex;
	float m_aspect;
	atomic<bool> m_needsUpdate;

	void initWS();
	void wsRun();
	UTexture2D* loadTexture2DFromFile(const FString& fullFilePath);
	UTexture2D* loadTexture2DFromBytes(const TArray<uint8>& bytes, EImageFormat fmt, int &w, int &h);
	void updateDynTex(const TArray<uint8>& img, EImageFormat fmt);
	
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	
public:	
	AStimulus();
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) UStaticMeshComponent *mesh;
	UPROPERTY(EditAnywhere)	SupportedEyeVersion EyeVersion;
};
