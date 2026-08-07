#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealTransport.h"
#include "PixelStreamingPeerConnection.h"

class FJsonObject;
class FPixelStreamingDataChannel;
class IHttpResponse;
class IWebSocket;

/** Opaque participant routing data returned by the trusted Worker. */
struct FCloudflareDirectTransportContext final : public IWebRTC4UnrealTransportContext
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

/** One direct WebRTC PeerConnection and bidirectional DataChannel per Unreal peer. */
class FCloudflareDirectTransport final : public IWebRTC4UnrealTransportEndpoint,
	public TSharedFromThis<FCloudflareDirectTransport>
{
public:
	FCloudflareDirectTransport();
	virtual ~FCloudflareDirectTransport() override;

	virtual bool Start(const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
		FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error) override;
	virtual bool Send(const FString& PeerId, const uint8* Data, int32 NumBytes) override;
	virtual void ClosePeer(const FString& PeerId) override;
	virtual void Close() override;
	virtual bool IsValid() const override;

private:
	using FHttpCallback = TFunction<void(TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool)>;

	struct FIceCandidate
	{
		FString SdpMid;
		int32 SdpMLineIndex = 0;
		FString Candidate;
	};

	struct FPeerState
	{
		TUniquePtr<FPixelStreamingPeerConnection> PeerConnection;
		TSharedPtr<FPixelStreamingDataChannel> DataChannel;
		TArray<FIceCandidate> PendingLocalCandidates;
		TArray<FIceCandidate> PendingRemoteCandidates;
		bool bOfferStarted = false;
		bool bAnswerStarted = false;
		bool bLocalDescriptionSent = false;
		bool bRemoteDescriptionSet = false;
		bool bIceConnected = false;
		bool bChannelOpen = false;
		bool bConnectedNotified = false;
		bool bDisconnectNotified = false;
	};

	void RequestIceServers();
	bool ParseIceConfiguration(const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded, FString& Error);
	void ConnectControlWebSocket();
	void OnControlConnected();
	void OnControlConnectionError(const FString& Error);
	void OnControlClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnControlMessage(const FString& Message);
	bool SendControl(const TSharedRef<FJsonObject>& Frame);

	TSharedPtr<FPeerState> CreatePeer(const FString& PeerId, bool bCreateDataChannel);
	void BeginHostOffer(const FString& PeerId);
	void ReceiveOffer(const FString& PeerId, const FString& Sdp);
	void ReceiveAnswer(const FString& PeerId, const FString& Sdp);
	void SendLocalDescription(const FString& PeerId, const FString& Type, const FString& Sdp);
	void OnLocalIceCandidate(const FString& PeerId, FIceCandidate Candidate);
	void OnRemoteIceCandidate(const FString& PeerId, FIceCandidate Candidate);
	void FlushLocalIceCandidates(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void FlushRemoteIceCandidates(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void OnIceStateChanged(const FString& PeerId,
		webrtc::PeerConnectionInterface::IceConnectionState NewState);

	void BindDataChannel(const FString& PeerId, const TSharedPtr<FPixelStreamingDataChannel>& Channel);
	void OnDataChannelOpen(const FString& PeerId);
	void HandleChannelMessage(const FString& PeerId, uint8 Type,
		const webrtc::DataBuffer& Buffer);
	void MaybeNotifyPeerConnected(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void RemovePeer(const FString& PeerId, bool bNotify);

	void SendWorkerRequest(const FString& Suffix, const FString& Verb,
		const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback);
	void NotifyPeerDisconnected(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void NotifyClosed();
	void ReportPeerError(const FString& PeerId, const FString& Error);
	void ReportEndpointError(const FString& Error);

	TSharedPtr<FCloudflareDirectTransportContext> Context;
	bool bIsHost = false;
	bool bClosing = false;
	bool bClosedNotified = false;
	bool bIceRequestStarted = false;
	webrtc::PeerConnectionInterface::RTCConfiguration IceConfig;
	TSharedPtr<IWebSocket> ControlSocket;
	FDelegateHandle ControlConnectedHandle;
	FDelegateHandle ControlErrorHandle;
	FDelegateHandle ControlClosedHandle;
	FDelegateHandle ControlMessageHandle;
	TMap<FString, TSharedPtr<FPeerState>> Peers;
	TMap<FString, TArray<FIceCandidate>> OrphanRemoteCandidates;
	FWebRTC4UnrealTransportCallbacks Callbacks;
};
