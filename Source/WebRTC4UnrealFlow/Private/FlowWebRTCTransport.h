#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealTransport.h"
#include "PixelStreamingPeerConnection.h"

class FJsonObject;
class FPixelStreamingDataChannel;
class IWebSocket;

struct FFlowWebRTCIceServer
{
	TArray<FString> Urls;
	FString Username;
	FString Credential;
};

/** Flow-only secrets and signalling data; never exposed through Core/BP types. */
struct FFlowWebRTCTransportContext final : public IWebRTC4UnrealTransportContext
{
	FString RoomId;
	FString Protocol;
	TArray<FString> SignallingUrls;
	TArray<FString> AsyncApiUrls;
	FString PrincipalContext;
	FString Timestamp;
	FString Signature;
	FString LocalPrincipalId;
	FString HostPrincipalId;
	TArray<FFlowWebRTCIceServer> IceServers;
	FString IceExpiresAt;
	int32 MaxParticipants = 0;
};

/** One Flow signalling socket with one WebRTC PeerConnection per remote player. */
class FFlowWebRTCTransport final : public IWebRTC4UnrealTransportEndpoint,
	public TSharedFromThis<FFlowWebRTCTransport>
{
public:
	FFlowWebRTCTransport();
	virtual ~FFlowWebRTCTransport() override;

	virtual bool Start(const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
		FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error) override;
	virtual bool Send(const FString& PeerId, const uint8* Data, int32 NumBytes) override;
	virtual void ClosePeer(const FString& PeerId) override;
	virtual void Close() override;
	virtual bool IsValid() const override;

private:
	struct FPendingIceCandidate
	{
		FString SdpMid;
		int32 SdpMLineIndex = 0;
		FString Candidate;
	};

	struct FPeerState
	{
		TUniquePtr<FPixelStreamingPeerConnection> PeerConnection;
		TSharedPtr<FPixelStreamingDataChannel> DataChannel;
		TArray<FPendingIceCandidate> PendingIceCandidates;
		bool bOfferStarted = false;
		bool bDataChannelOpen = false;
		bool bDisconnectNotified = false;
	};

	void ConfigureIce();
	void ConnectSignalling();
	void OnSignallingConnected();
	void OnSignallingConnectionError(const FString& Error);
	void OnSignallingClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnSignallingMessage(const FString& Message);
	void HandleAuthenticated(const TSharedPtr<FJsonObject>& Frame);
	void HandlePeerJoined(const TSharedPtr<FJsonObject>& Frame);
	void HandlePeerLeft(const TSharedPtr<FJsonObject>& Frame);
	void HandleSignal(const TSharedPtr<FJsonObject>& Frame);
	bool AddRemotePeer(const FString& PeerId);
	bool IsAllowedPeer(const FString& PeerId) const;

	bool SendJson(const TSharedRef<FJsonObject>& Frame);
	bool SendSignal(const FString& PeerId, const FString& Kind,
		const TSharedRef<FJsonObject>& Payload);
	void SendSessionDescription(const FString& PeerId, const FString& Kind,
		const webrtc::SessionDescriptionInterface* Sdp);
	void SendIceCandidate(const FString& PeerId, const webrtc::IceCandidateInterface* Candidate);

	TSharedPtr<FPeerState> FindPeer(const FString& PeerId) const;
	bool EnsurePeerConnection(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void BeginHostOffer(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void AcceptOffer(const FString& PeerId, const TSharedRef<FPeerState>& Peer, const FString& Sdp);
	void AcceptAnswer(const FString& PeerId, const TSharedRef<FPeerState>& Peer, const FString& Sdp);
	void AddRemoteIceCandidate(const TSharedRef<FPeerState>& Peer, const FString& SdpMid,
		int32 SdpMLineIndex, const FString& Candidate);
	void FlushPendingIceCandidates(const TSharedRef<FPeerState>& Peer);
	void BindDataChannel(const FString& PeerId, const TSharedRef<FPeerState>& Peer,
		const TSharedPtr<FPixelStreamingDataChannel>& Channel);
	void HandleChannelMessage(const FString& PeerId, uint8 Type,
		const webrtc::DataBuffer& Buffer);
	void NotifyPeerDisconnected(const FString& PeerId, const TSharedRef<FPeerState>& Peer);
	void NotifyClosed();
	void ReportPeerError(const FString& PeerId, const FString& Error);
	void ReportEndpointError(const FString& Error);
	TSharedPtr<FFlowWebRTCTransportContext> Context;
	bool bIsHost = false;
	bool bSignallingAuthenticated = false;
	bool bClosing = false;
	bool bClosedNotified = false;
	FString LocalPrincipalId;
	webrtc::PeerConnectionInterface::RTCConfiguration IceConfig;
	TSharedPtr<IWebSocket> SignallingSocket;
	FDelegateHandle SignallingConnectedHandle;
	FDelegateHandle SignallingErrorHandle;
	FDelegateHandle SignallingClosedHandle;
	FDelegateHandle SignallingMessageHandle;
	TMap<FString, TSharedPtr<FPeerState>> Peers;
	FWebRTC4UnrealTransportCallbacks Callbacks;
};
