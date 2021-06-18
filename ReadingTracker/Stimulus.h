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

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Stimulus.generated.h"

UCLASS()
class READINGTRACKER_API AStimulus : public AActor
{
	GENERATED_BODY()
	
private:
	WSServer m_server;
	thread m_serverThread;
	void initWS();
	void wsRun();
	UTexture2D* loadTexture2DFromFile(const FString& fullFilePath);
	
protected:
	virtual void BeginPlay() override;
	//virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	
public:	
	AStimulus();
	virtual void Tick(float DeltaTime) override;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite) UStaticMeshComponent *mesh;
};
