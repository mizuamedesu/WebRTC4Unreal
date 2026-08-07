#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealTransport.h"
#include "PixelStreamingPeerConnection.h"

class FJsonObject;
class FPixelStreamingDataChannel;
class IHttpResponse;
class IWebSocket;

/** Secrets and room routing owned only by the Cloudflare Realtime adapter. */
struct FCloudflareRealtimeTransportContext final : public IWebRTC4UnrealTransportContext
{
	FString WorkerUrl;
	FString SignalUrl;
	FString Protocol;
	FString RoomId;
	FString RoomName;
	FString ParticipantId;
	FString ParticipantToken;
	FString HostId;
	int32 MaxParticipants = 0;
};

/** One WebRTC PeerConnection to Cloudflare's SFU, with one channel per Unreal client. */
class FCloudflareRealtimeTransport final : public IWebRTC4UnrealTransportEndpoint,
	public TSharedFromThis<FCloudflareRealtimeTransport>
{
public:
	FCloudflareRealtimeTransport();
	virtual ~FCloudflareRealtimeTransport() override;

	virtual bool Start(const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
		FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error) override;
	virtual bool Send(const FString& PeerId, const uint8* Data, int32 NumBytes) override;
	virtual void ClosePeer(const FString& PeerId) override;
	virtual void Close() override;
	virtual bool IsValid() const override;

private:
	using FHttpCallback = TFunction<void(TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool)>;

	struct FPeerState
	{
		FString RemoteSessionId;
		TSharedPtr<FPixelStreamingDataChannel> SendDataChannel;
		TSharedPtr<FPixelStreamingDataChannel> ReceiveDataChannel;
		bool bPublishRequested = false;
		bool bSubscribeRequested = false;
		bool bUplinkPublishRequested = false;
		bool bUplinkSubscribeRequested = false;
		bool bChannelAnswerSent = false;
		bool bLocalReadySent = false;
		bool bSendDataChannelOpen = false;
		bool bReceiveDataChannelOpen = false;
		bool bLocalChannelsOpenLogged = false;
		bool bRemoteReady = false;
		bool bConnectedNotified = false;
		bool bDisconnectNotified = false;
	};

	void ConfigurePeerConnection();
	void ConnectControlWebSocket();
	void OnControlConnected();
	void OnControlConnectionError(const FString& Error);
	void OnControlClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnControlMessage(const FString& Message);
	bool SendControl(const TSharedRef<FJsonObject>& Frame);

	void CreateRealtimeSession();
	void EstablishDataChannelTransport();
	void AcceptRealtimeOffer(const FString& Sdp);
	void SendRealtimeAnswer(const webrtc::SessionDescriptionInterface* Answer);
	void MarkTransportReady();
	void OnIceStateChanged(webrtc::PeerConnectionInterface::IceConnectionState NewState);
	void ProcessReadyPeers();
	void QueueHostPeer(const FString& PeerId, const FString& SessionId);
	void PublishHostChannel(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void SubscribeHostChannel(const FString& PeerId, const FString& PublisherSessionId,
		const FString& DataChannelName);
	void QueueClientChannel(const FString& PublisherSessionId, const FString& DataChannelName);
	void SubscribeClientChannel();
	void PublishClientChannel();

	void SendWorkerRequest(const FString& Suffix, const FString& Verb,
		const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback);
	bool ParseJsonResponse(const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded, const TArray<int32>& ExpectedCodes,
		TSharedPtr<FJsonObject>& OutJson, FString& OutError) const;
	static int32 ReadDataChannelId(const TSharedPtr<FJsonObject>& Json);
	static FString GetString(const TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName);

	TSharedRef<FPeerState> FindOrAddPeer(const FString& PeerId);
	TSharedPtr<FPeerState> FindPeer(const FString& PeerId) const;
	void BindSendDataChannel(const FString& PeerId, const TSharedRef<FPeerState>& Peer,
		const TSharedPtr<FPixelStreamingDataChannel>& Channel);
	void BindReceiveDataChannel(const FString& PeerId, const TSharedRef<FPeerState>& Peer,
		const TSharedPtr<FPixelStreamingDataChannel>& Channel);
	void OnDataChannelOpen(const FString& PeerId, bool bSendChannel);
	void MaybeNotifyPeerConnected(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void HandleChannelMessage(const FString& PeerId, uint8 Type,
		const webrtc::DataBuffer& Buffer);
	void NotifyPeerDisconnected(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void NotifyClosed();
	void ReportPeerError(const FString& PeerId, const FString& Error);
	void ReportEndpointError(const FString& Error);

	TSharedPtr<FCloudflareRealtimeTransportContext> Context;
	bool bIsHost = false;
	bool bClosing = false;
	bool bClosedNotified = false;
	bool bSessionRequestStarted = false;
	bool bDataTransportEstablished = false;
	bool bIceConnected = false;
	bool bReadyRequestStarted = false;
	FString SessionId;
	FString PendingPublisherSessionId;
	FString PendingDataChannelName;
	webrtc::PeerConnectionInterface::RTCConfiguration IceConfig;
	TUniquePtr<FPixelStreamingPeerConnection> PeerConnection;
	TSharedPtr<FPixelStreamingDataChannel> ServerEventsChannel;
	TSharedPtr<IWebSocket> ControlSocket;
	FDelegateHandle ControlConnectedHandle;
	FDelegateHandle ControlErrorHandle;
	FDelegateHandle ControlClosedHandle;
	FDelegateHandle ControlMessageHandle;
	TMap<FString, TSharedPtr<FPeerState>> Peers;
	FWebRTC4UnrealTransportCallbacks Callbacks;
};
