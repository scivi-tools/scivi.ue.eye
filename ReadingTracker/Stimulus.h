// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#define DEPRECATED
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#define ASIO_STANDALONE 1
#include "ws/server_ws.hpp"
THIRD_PARTY_INCLUDES_END
#undef UI
#undef ERROR
#undef UpdateResource

using namespace std;

using WSServer = SimpleWeb::SocketServer<SimpleWeb::WS>;

#include "SRanipal_Eyes_Enums.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Stimulus.generated.h"


#define EYE_DEBUG

UCLASS()
class READINGTRACKER_API AStimulus : public AActor
{
	GENERATED_BODY()
	
private:
	struct BBox
	{
		FVector2D lt;
		FVector2D rb;
	};
	struct AOI
	{
		FString name;
		TArray<FVector2D> path;
		BBox bbox;
	};
	struct CalibPt
	{
		float cAlpha;
		float cBeta;
		float errU;
		float errV;
		CalibPt(float cA, float cB, float eU, float eV): cAlpha(cA), cBeta(cB), errU(eU), errV(eV) {};
		float pDist(float ca, float cb) const { return fabs(cAlpha - ca) + fabs(cBeta - cb); };
	};
	struct CalibQ
	{
		FQuat qgaz; // rotate from cam to gaze
		FQuat qerr; // rotate from gaze to real
	};

	WSServer m_server;
	thread m_serverThread;

	UMaterialInstanceDynamic* m_dynTex;
	UCanvasRenderTarget2D* m_dynContour;
	mutex m_mutex;
	float m_aspect;
	float m_scaleX;
	float m_scaleY;
	int m_dynTexW;
	int m_dynTexH;
	TArray<AOI> m_dynAOIs;
	atomic<bool> m_needsUpdate;
	atomic<bool> m_needsUpdateCalib;
	int m_calibIndex;

	bool m_inSelectionMode;
	bool m_rReleased;
	bool m_imgUpdated;
	int m_stimulusW;
	int m_stimulusH;
	int m_activeAOI;
	TArray<AOI> m_aois;
	TArray<int> m_selectedAOIs;
#ifdef EYE_DEBUG
	float m_u;
	float m_v;
	float m_cu;
	float m_cv;
#endif // EYE_DEBUG
	TArray<CalibPt> m_calibBack;
	TArray<CalibPt> m_calib;
	TArray<CalibQ> m_calibQ;
	
	APlayerCameraManager* m_camera;

	void initWS();
	void wsRun();
	UTexture2D* loadTexture2DFromFile(const FString& fullFilePath);
	UTexture2D* loadTexture2DFromBytes(const TArray<uint8>& bytes, EImageFormat fmt, int& w, int& h);
	void updateDynTex(const TArray<uint8>& img, EImageFormat fmt, float sx, float sy, const TArray<TSharedPtr<FJsonValue>>& aois);
	void drawContourOfAOI(UCanvas* cvs, const FLinearColor &color, float th, int aoi) const;
	bool pointInPolygon(const FVector2D& pt, const TArray<FVector2D> &poly) const;
	bool pointInBBox(const FVector2D& pt, const BBox& bbox) const;
	bool hitTest(const FVector2D& pt, const AOI& aoi) const;
	int findActiveAOI(const FVector2D& pt) const;
	void toggleSelectedAOI(int aoi);
	void calibrate();
	void applyCalib(float ca, float cb, float &u, float &v);
	
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	
public:	
	AStimulus();
	virtual void Tick(float DeltaTime) override;

	UFUNCTION() void drawContour(UCanvas* cvs, int32 w, int32 h);

	UFUNCTION(BlueprintCallable) void trigger(bool isPressed);

	UPROPERTY(EditAnywhere, BlueprintReadWrite) UStaticMeshComponent *mesh;
	UPROPERTY(EditAnywhere)	SupportedEyeVersion EyeVersion;
};
