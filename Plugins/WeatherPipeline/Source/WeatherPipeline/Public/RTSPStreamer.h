#pragma once

#include "CoreMinimal.h"
#include <stdio.h>
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "HAL/PlatformProcess.h"
#include "Networking.h"
#include "Sockets.h"
#include "SocketSubsystem.h"



class FRTSPStreamer
{
public:
	FRTSPStreamer();
	~FRTSPStreamer();

	// Start the FFmpeg process
	bool StartStream(int32 InWidth, int32 InHeight, int32 InFPS, FString RTSPURL);

	// Kill the process
	void StopStream();

	bool IsStreaming() const { return bIsStreaming; }

	// Push raw FColor data to ffmpeg over the local TCP server. This is the only frame path in use -
	// the older pipe-based SendFrame/ConnectToFFmpeg pair was removed as neither had any caller.
	void SendFrameTCP(const TArray<FColor>& Bitmap);
	
	bool StartTCPServer(int32 InWidth, int32 InHeight);

	// Overrides the built-in defaults for ffmpeg location and stream endpoints.
	// Call before StartStream/StartTCPServer (AMyActor_weather does this in StartStreamRTSP).
	void Configure(const FString& InFFmpegPath, const FString& InIPAddress, int32 InVideoPort, int32 InDataPort);

	FSocket* ListenSocket = nullptr;
	FSocket* ClientSocket = nullptr;

	// Metadata channel (listens on dataPort)
	FSocket* MetadataListenSocket = nullptr;
	FSocket* MetadataClientSocket = nullptr;

	// ... existing variables ...
	float YawRate;
	float PitchRate;

	// New function to start the metadata listener
	bool StartMetadataServer();

	// New function to read metadata
	bool ReceiveMetadata();

private:
	int32 Width;
	int32 Height;
	bool bIsStreaming ;
	// Member variables needed for the class:
	 FProcHandle FFmpegProcessHandle;
	 void* PipeWriteChild;
	 void* PipeReadChild;
	 FSocket* Socket = nullptr;
	 // Defaults — override via Configure() (driven by AMyActor_weather config UPROPERTYs)
	 FString FFmpegPath = TEXT("ffmpeg.exe");
	 FString ip_address = TEXT("127.0.0.1");
	 bool IsConnected = false;
	 int32 rstpPort = 9000;
	 int32 dataPort = 9001;
};


class FAcceptTask : public FNonAbandonableTask
{
public:
	FRTSPStreamer* Streamer;

	FAcceptTask(FRTSPStreamer* InStreamer) : Streamer(InStreamer) {}

	void DoWork()
	{
		bool Pending;
		while (true)
		{
			if (Streamer->ListenSocket->HasPendingConnection(Pending) && Pending)
			{
				Streamer->ClientSocket = Streamer->ListenSocket->Accept(TEXT("FFmpegClient"));
				if (Streamer->ClientSocket)
				{
					UE_LOG(LogTemp, Log, TEXT("FFmpeg connected!"));
				}
				return;
			}
			FPlatformProcess::Sleep(0.01f);
		}
	}

	FORCEINLINE TStatId GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(FAcceptTask, STATGROUP_ThreadPoolAsyncTasks); }
};


class FMetadataAcceptTask : public FNonAbandonableTask
{
public:
	FRTSPStreamer* Streamer;

	FMetadataAcceptTask(FRTSPStreamer* InStreamer) : Streamer(InStreamer) {}

	void DoWork()
	{
		bool PendingData;
		while (true)
		{
			if (Streamer->MetadataListenSocket->HasPendingConnection(PendingData) && PendingData)
			{
				Streamer->MetadataClientSocket = Streamer->MetadataListenSocket->Accept(TEXT("FFmpegClient"));
				if (Streamer->MetadataClientSocket)
				{
					UE_LOG(LogTemp, Log, TEXT("FFmpeg connected!"));
				}
				return;
			}
			FPlatformProcess::Sleep(0.01f);
		}
	}

	FORCEINLINE TStatId GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(FAcceptTask, STATGROUP_ThreadPoolAsyncTasks); }
};