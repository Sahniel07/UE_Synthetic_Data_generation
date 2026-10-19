#include "RTSPStreamer.h"

// Initialiser order must match the declaration order in RTSPStreamer.h - members are constructed in
// declaration order regardless of how they are listed here, so a mismatch is a latent bug the moment
// one member's initialiser ever reads another.
FRTSPStreamer::FRTSPStreamer()
	: Width(0), Height(0), bIsStreaming(false), FFmpegProcessHandle(nullptr)
{
}

FRTSPStreamer::~FRTSPStreamer()
{
	StopStream();
}

void FRTSPStreamer::Configure(const FString& InFFmpegPath, const FString& InIPAddress, int32 InVideoPort, int32 InDataPort)
{
	if (!InFFmpegPath.IsEmpty()) FFmpegPath = InFFmpegPath;
	if (!InIPAddress.IsEmpty())  ip_address = InIPAddress;
	if (InVideoPort > 0)         rstpPort   = InVideoPort;
	if (InDataPort > 0)          dataPort   = InDataPort;
}

bool FRTSPStreamer::StartStream(int32 InWidth, int32 InHeight, int32 InFPS, FString RTSPURL)
{
    if (bIsStreaming) return true;

    Width = InWidth;
    Height = InHeight;

    // ffmpeg reads raw frames from the local TCP server (see StartTCPServer)
    FString Args = FString::Printf(
        TEXT("-re -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i tcp://127.0.0.1:%d -c:v libx264 -preset ultrafast -tune zerolatency -vf \"scale=720:480\" -f rtsp -rtsp_transport tcp %s"),
        Width,
        Height,
        InFPS,
        rstpPort,
        *RTSPURL
    );

    // Create STDIN pipe for FFmpeg
    // FPlatformProcess::CreatePipe(PipeReadChild, PipeWriteChild);

    // Start FFmpeg with child STDIN bound to PipeWriteChild
    FFmpegProcessHandle = FPlatformProcess::CreateProc(
        *FFmpegPath,
        *Args,
        false,   // detached
        false,   // hidden
        false,   // launch hidden window
        nullptr,
        0,
        nullptr,
        nullptr,   // Child STDIN
        nullptr          // Child STDOUT (optional)
    );

    if (!FFmpegProcessHandle.IsValid())
    {
        FPlatformProcess::ClosePipe(PipeWriteChild, PipeReadChild);
        UE_LOG(LogTemp, Error, TEXT("RTSP: Failed to launch FFmpeg."));
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("RTSP: Failed to launch FFmpeg."));
        return false;
    }

    // Parent does not use child end
     //FPlatformProcess::ClosePipe(PipeWriteChild, nullptr);

    bIsStreaming = true;
    UE_LOG(LogTemp, Log, TEXT("RTSP: Streaming started to %s"), *RTSPURL);
    if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, TEXT("ffmpeg started"));
    return true;
}


void FRTSPStreamer::StopStream()
{
    // 1. Check if streaming is actually running
    if (!bIsStreaming)
    {
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("Stopping RTSP Stream..."));

    // 2. Close the TCP Socket (Input Source)
    if (ClientSocket)
    {
        // Close the socket connection. The implementation depends on your networking library (e.g., FSocket::Close)
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        ClientSocket = nullptr;
    }

    // 3. Terminate the External FFmpeg Process
    if (FFmpegProcessHandle.IsValid())
    {
        // Use the FPlatformProcess to terminate the running FFmpeg executable.
        // This is necessary to release the file handle and stop encoding.
        FPlatformProcess::TerminateProc(FFmpegProcessHandle);
        FPlatformProcess::CloseProc(FFmpegProcessHandle); // Close the handle resource

        // Reset the handle pointer
        FFmpegProcessHandle.Reset();
    }

    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    if (MetadataClientSocket)
    {
        MetadataClientSocket->Close();
        SocketSubsystem->DestroySocket(MetadataClientSocket);
        MetadataClientSocket = nullptr;
    }

    if (MetadataListenSocket)
    {
        MetadataListenSocket->Close();
        SocketSubsystem->DestroySocket(MetadataListenSocket);
        MetadataListenSocket = nullptr;
    }

    // 4. Update state flag
    bIsStreaming = false;
    UE_LOG(LogTemp, Log, TEXT("RTSP Stream stopped successfully."));
}

void FRTSPStreamer::SendFrameTCP(const TArray<FColor>& Bitmap)
{
    if (!ClientSocket) {
        // No client connected yet → just skip sending this frame
        return;
    }
    if (!bIsStreaming) { return; }
    int32 BytesSent = 0;
    int32 TotalSize = Width * Height * 4;

    const uint8* RawBGRA = reinterpret_cast<const uint8*>(Bitmap.GetData());

    // Keep sending until all bytes are pushed
    while (BytesSent < TotalSize)
    {
        int32 SentNow = 0;
        ClientSocket->Send(RawBGRA + BytesSent, TotalSize - BytesSent, SentNow);
        if (SentNow <= 0)
        {
            UE_LOG(LogTemp, Error, TEXT("Socket send failed."));
            if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("Socket send failed."));
            break;
        }
        BytesSent += SentNow;
    }
    //if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Yellow, TEXT("Add image to stream"));
}

bool FRTSPStreamer::StartTCPServer(int32 InWidth, int32 InHeight )
{
    Height = InHeight;
    Width = InWidth;
    if (bIsStreaming) { return true; }
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("FFmpegStreamSocket"), false);
    if (!ListenSocket)
    {
        UE_LOG(LogTemp, Error, TEXT("Socket creation failed."));
        return false;
    }
    // (bypassing the OS's TIME_WAIT state).
    bool bReuseAddrSuccess = ListenSocket->SetReuseAddr(true);
    if (!bReuseAddrSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to set SO_REUSEADDR option. Bind failures may occur."));
    }

    

    FIPv4Address Addr;
    FIPv4Address::Parse(ip_address, Addr);

    TSharedRef<FInternetAddr> InternetAddr = SocketSubsystem->CreateInternetAddr();
    InternetAddr->SetIp(Addr.Value);
    InternetAddr->SetPort(rstpPort);

    // Bind
    if (!ListenSocket->Bind(*InternetAddr))
    {
        UE_LOG(LogTemp, Error, TEXT("Bind failed on port &d."), rstpPort);
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("Bind failed."));
        return false;
    }

    // Listen
    if (!ListenSocket->Listen(1))
    {
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("Listening failed."));
        return false;
    }

    // Accept connection in async thread
    (new FAutoDeleteAsyncTask<FAcceptTask>(this))->StartBackgroundTask();
    bIsStreaming = true;
    if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, TEXT("TCP server started"));
    return true;
}

// FRTSPStreamer.cpp

bool FRTSPStreamer::StartMetadataServer()
{
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    // 1. Create the new listening socket
    MetadataListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MetadataSocket"), false);
    if (!MetadataListenSocket) return false;

    // 2. Set SO_REUSEADDR (Fixes Bind Fail on restart)
    MetadataListenSocket->SetReuseAddr(true);

    // 3. Setup Address (using the same IP as before)
    FIPv4Address Addr;
    FIPv4Address::Parse(ip_address, Addr); // Assuming ip_address is a member or defined

    TSharedRef<FInternetAddr> InternetAddr1 = SocketSubsystem->CreateInternetAddr();
    InternetAddr1->SetIp(Addr.Value);
    InternetAddr1->SetPort(dataPort); // Use the new port (e.g., 9001)

    // 4. Bind and Listen
    if (!MetadataListenSocket->Bind(*InternetAddr1))
    {
        UE_LOG(LogTemp, Error, TEXT("Metadata Bind failed on port %d."), dataPort);
        SocketSubsystem->DestroySocket(MetadataListenSocket);
        MetadataListenSocket = nullptr;
        return false;
    }

    if (!MetadataListenSocket->Listen(1))
    {
        UE_LOG(LogTemp, Error, TEXT("Metadata Listening failed."));
        SocketSubsystem->DestroySocket(MetadataListenSocket);
        MetadataListenSocket = nullptr;
        return false;
    }

    // 5. Spawn an async task/thread to accept the connection
    (new FAutoDeleteAsyncTask<FMetadataAcceptTask>(this))->StartBackgroundTask();

    UE_LOG(LogTemp, Log, TEXT("Metadata TCP server started on port %d."), dataPort);
    IsConnected = true;
    return true;
}



bool FRTSPStreamer::ReceiveMetadata()
{
    //UE_LOG(LogTemp, Log, TEXT("In receive Metadata"));
    if (!MetadataClientSocket || !IsConnected)
    {
        //UE_LOG(LogTemp, Log, TEXT("Receive Metadata socket not connected"));
        //if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("Metadata client socket not connected "));
        return false; // No connected client
    }
    bool recerived = false;
    uint32 Size;
    while (MetadataClientSocket->HasPendingData(Size))
    {
        //UE_LOG(LogTemp, Log, TEXT("In receive Metadata Listen Socket"));
        TArray<uint8> ReceivedData;
        ReceivedData.SetNumUninitialized(FMath::Min((int32)Size, 64)); // Read up to 64 bytes

        int32 BytesRead = 0;
        MetadataClientSocket->Recv(ReceivedData.GetData(), ReceivedData.Num(), BytesRead);
        //UE_LOG(LogTemp, Log, TEXT("In receive Metadata Listen Socket after BytesRead"));
        if (BytesRead > 0)
        {
            // Convert received bytes to an FString (assuming ASCII/UTF8 format)
            FString ReceivedString = FString(BytesRead, (char*)ReceivedData.GetData());
            ReceivedString = ReceivedString.TrimStartAndEnd(); // Clean up whitespace
            //UE_LOG(LogTemp, Log, TEXT("receiving string... "));
            // --- PARSING LOGIC: Assuming data is sent as "Alpha,Angle" ---
            TArray<FString> Parts;
            if (ReceivedString.ParseIntoArray(Parts, TEXT(","), true) == 2)
            {
                // Convert string parts to float
                YawRate = FCString::Atof(*Parts[0]);
                PitchRate = FCString::Atof(*Parts[1]);
                recerived = true;
                // Log and use the values
                UE_LOG(LogTemp, Log, TEXT("Metadata Received: Yaw=%.3f , Pitch=%.3f"), YawRate, PitchRate);
                //FString msg = TEXT("Received Roll and Pitch: ") + ReceivedString;
                //if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, msg);

            }
        }
    }
    //UE_LOG(LogTemp, Log, TEXT("out of receive Metadata"));
    return recerived;
}