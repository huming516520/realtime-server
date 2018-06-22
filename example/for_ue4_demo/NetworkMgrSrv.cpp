#include "for_ue4_demo_shared.h"


#ifdef IS_LINUX
AtomicInt32 NetworkMgrSrv::kNewPlayerId;
#else
int NetworkMgrSrv::kNewPlayerId = 1;
#endif //IS_LINUX

std::unique_ptr<NetworkMgrSrv> NetworkMgrSrv::sInst;

namespace
{
	const float kTimeBetweenStatePackets = 0.033f;
	const float kClientDisconnectTimeout = 6.f;
}


bool NetworkMgrSrv::StaticInit( uint16_t inPort )
{
	sInst.reset( new NetworkMgrSrv() );
	return sInst->Init( inPort );
}

#ifdef IS_LINUX

AtomicInt32 NetworkMgrSrv::kNewNetworkId;

NetworkMgrSrv::NetworkMgrSrv() :
	mTimeBetweenStatePackets( 0.033f ),
	mLastCheckDCTime( 0.f ),
	mTimeOfLastStatePacket( 0.f ),
	mNetworkIdToGameObjectMap( new IntToGameObjectMap ),
	mUdpConnToClientMap( new UdpConnToClientMap ),
	mPlayerIdToClientMap( new IntToClientMap )
{
	kNewNetworkId.getAndSet( 1 );
	kNewPlayerId.getAndSet( 1 );
}

void NetworkMgrSrv::SendPacket( const OutputBitStream& inOutputStream, const UdpConnectionPtr& conn )
{
	conn->send(
		inOutputStream.GetBufferPtr(), inOutputStream.GetByteLength() );
}

void NetworkMgrSrv::Start()
{
	server_->start();
	loop_.loop();
}

void NetworkMgrSrv::threadInit( EventLoop* loop )
{
	MutexLockGuard lock( mutex_ );
	loops_.insert( loop );
}

IntToGameObjectMapPtr NetworkMgrSrv::GetNetworkIdToGameObjectMap()
{
	MutexLockGuard lock( mutex_ );
	return mNetworkIdToGameObjectMap;
}

void NetworkMgrSrv::NetworkIdToGameObjectMapCOW()
{
	if ( !mNetworkIdToGameObjectMap.unique() )
	{
		mNetworkIdToGameObjectMap.reset( new IntToGameObjectMap( *mNetworkIdToGameObjectMap ) );
	}
	assert( mNetworkIdToGameObjectMap.unique() );
}

int NetworkMgrSrv::GetNewNetworkId()
{
	int toRet = kNewNetworkId.getAndAdd( 1 );
	if ( kNewNetworkId.get() < toRet )
	{
		LOG( "Network ID Wrap Around!!! You've been playing way too long...", 0 );
	}

	return toRet;
}

EntityPtr NetworkMgrSrv::GetGameObject( int inNetworkId )
{
	auto tempNetworkIdToGameObjectMap = GetNetworkIdToGameObjectMap();
	auto gameObjectIt = tempNetworkIdToGameObjectMap->find( inNetworkId );
	if ( gameObjectIt != tempNetworkIdToGameObjectMap->end() )
	{
		return gameObjectIt->second;
	}
	else
	{
		return EntityPtr();
	}
}

//int NetworkMgrSrv::RegistEntityAndRetNetID( EntityPtr inGameObject )
//{
//	int newNetworkId = GetNewNetworkId();
//	inGameObject->SetNetworkId( newNetworkId );
//	{
//		MutexLockGuard lock( mutex_ );
//		NetworkIdToGameObjectMapCOW();
//		( *mNetworkIdToGameObjectMap )[newNetworkId] = inGameObject;
//	}
//	return newNetworkId;
//	//UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
//	//for ( const auto& pair : *tempUdpConnToClientMap )
//	//{
//	//	pair.second->GetReplicationManager().ReplicateCreate( newNetworkId, inGameObject->GetAllStateMask() );
//	//}
//}

int NetworkMgrSrv::UnregistEntityAndRetNetID( Entity* inGameObject )
{
	int networkId = inGameObject->GetNetworkId();
	{
		MutexLockGuard lock( mutex_ );
		NetworkIdToGameObjectMapCOW();
		mNetworkIdToGameObjectMap->erase( networkId );
	}
	return networkId;

	//UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	//for ( const auto& pair : *tempUdpConnToClientMap )
	//{
	//	pair.second->GetReplicationManager().ReplicateDestroy( networkId );
	//}
}

//void NetworkMgrSrv::onConnection( const UdpConnectionPtr& conn )
//{
//	LOG_INFO << conn->localAddress().toIpPort() << " -> "
//		<< conn->peerAddress().toIpPort() << " is "
//		<< ( conn->connected() ? "UP" : "DOWN" );
//}

void NetworkMgrSrv::onMessage( const muduo::net::UdpConnectionPtr& conn,
	muduo::net::Buffer* buf,
	muduo::Timestamp receiveTime )
{
	if ( buf->readableBytes() > 0 ) // kHeaderLen == 0
	{
		InputBitStream inputStream( buf->peek(), buf->readableBytes() * 8 );
		buf->retrieveAll();

		ProcessPacket( inputStream, conn );
		CheckForDisconnects();
		WorldUpdateCB_();
		SendOutgoingPackets();
	}
}

bool NetworkMgrSrv::Init( uint16_t inPort )
{
	InetAddress serverAddr( inPort );
	server_.reset( new UdpServer( &loop_, serverAddr, "realtime_srv" ) );

	server_->setConnectionCallback(
		std::bind( &NetworkMgrSrv::onConnection, this, _1 ) );
	server_->setMessageCallback(
		std::bind( &NetworkMgrSrv::onMessage, this, _1, _2, _3 ) );

	server_->setThreadNum( THREAD_NUM );
	server_->setThreadInitCallback( std::bind( &NetworkMgrSrv::threadInit, this, _1 ) );

	return true;
}

void NetworkMgrSrv::onConnection( const UdpConnectionPtr& conn )
{
	//NetworkMgrSrv::onConnection( conn );
	LOG_INFO << conn->localAddress().toIpPort() << " -> "
		<< conn->peerAddress().toIpPort() << " is "
		<< ( conn->connected() ? "UP" : "DOWN" );
	if ( !conn->connected() )
	{
		MutexLockGuard lock( mutex_ );
		UdpConnToClientMapCOW();
		PlayerIdToClientMapCOW();

		LOG( "Player %d disconnect", ( ( *mUdpConnToClientMap )[conn] )->GetPlayerId() );

		mPlayerIdToClientMap->erase( ( ( *mUdpConnToClientMap )[conn] )->GetPlayerId() );
		mUdpConnToClientMap->erase( conn );
	}
}

UdpConnToClientMapPtr NetworkMgrSrv::GetUdpConnToClientMap()
{
	MutexLockGuard lock( mutex_ );
	return mUdpConnToClientMap;
}

void NetworkMgrSrv::UdpConnToClientMapCOW()
{
	if ( !mUdpConnToClientMap.unique() )
	{
		mUdpConnToClientMap.reset( new UdpConnToClientMap( *mUdpConnToClientMap ) );
	}
	assert( mUdpConnToClientMap.unique() );
}

PlayerIdToClientMapPtr NetworkMgrSrv::GetPlayerIdToClientMap()
{
	MutexLockGuard lock( mutex_ );
	return mPlayerIdToClientMap;
}

void NetworkMgrSrv::PlayerIdToClientMapCOW()
{
	if ( !mPlayerIdToClientMap.unique() )
	{
		mPlayerIdToClientMap.reset( new IntToClientMap( *mPlayerIdToClientMap ) );
	}
	assert( mPlayerIdToClientMap.unique() );
}

void NetworkMgrSrv::ProcessPacket( InputBitStream& inInputStream,
	const UdpConnectionPtr& inUdpConnetction )
{
	UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	auto it = tempUdpConnToClientMap->find( inUdpConnetction );
	if ( it == tempUdpConnToClientMap->end() )
	{
		HandlePacketFromNewClient(
			inInputStream,
			inUdpConnetction
		);
	}
	else
	{
		DoProcessPacket( ( *it ).second, inInputStream );
	}
}

void NetworkMgrSrv::HandlePacketFromNewClient( InputBitStream& inInputStream,
	const UdpConnectionPtr& inUdpConnetction )
{
	uint32_t	packetType;
	inInputStream.Read( packetType );
	if ( packetType == kHelloCC
		|| packetType == kInputCC
		|| packetType == kResetedCC )
	{
		std::string playerName = "realtime_srv_test_player_name";
		//inInputStream.Read( playerName );	

		ClientProxyPtr newClientProxy =
			std::make_shared< ClientProxy >(
				playerName,
				kNewPlayerId.getAndAdd( 1 ),
				inUdpConnetction );

		{
			MutexLockGuard lock( mutex_ );
			UdpConnToClientMapCOW();
			PlayerIdToClientMapCOW();
			( *mUdpConnToClientMap )[inUdpConnetction] = newClientProxy;
			( *mPlayerIdToClientMap )[newClientProxy->GetPlayerId()] = newClientProxy;
		}

		// fixme
		RealTimeSrv::sInstance.get()->HandleNewClient( newClientProxy );

		IntToGameObjectMapPtr tempNetworkIdToGameObjectMap = GetNetworkIdToGameObjectMap();
		for ( const auto& pair : *tempNetworkIdToGameObjectMap )
		{
			newClientProxy->GetReplicationManager().ReplicateCreate( pair.first, pair.second->GetAllStateMask() );
		}

		if ( packetType == kHelloCC )
		{
			//SendWelcomePacket( newClientProxy );
			SendGamePacket( newClientProxy, kWelcomeCC );
		}
		else
		{
			// Server reset
			newClientProxy->SetRecvingServerResetFlag( true );
			//SendResetPacket( newClientProxy );
			SendGamePacket( newClientProxy, kResetCC );
			LOG( "SendResetPacket", 0 );
		}

		//LOG_INFO << inUdpConnetction->localAddress().toIpPort() 
		//	<< " -> "
		//	<< inUdpConnetction->peerAddress().toIpPort() << " is "
		//	<< ( inUdpConnetction->connected() ? "UP" : "DOWN" );

		LOG( "a new client named '%s' as PlayerID %d ",
			newClientProxy->GetName().c_str(),
			newClientProxy->GetPlayerId() );
	}
	//else if ( packetType == kInputCC )
	//{
	//	// Server reset
	//	SendResetPacket( inFromAddress );
	//}
	else
	{
		LOG_INFO << "Bad incoming packet from unknown client at socket "
			<< inUdpConnetction->peerAddress().toIpPort() << " is "
			<< " - we're under attack!!";
	}
}

int NetworkMgrSrv::RegistEntityAndRetNetID( EntityPtr inGameObject )
{
	int newNetworkId = GetNewNetworkId();
	inGameObject->SetNetworkId( newNetworkId );
	{
		MutexLockGuard lock( mutex_ );
		NetworkIdToGameObjectMapCOW();
		( *mNetworkIdToGameObjectMap )[newNetworkId] = inGameObject;
	}

	UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	for ( const auto& pair : *tempUdpConnToClientMap )
	{
		pair.second->GetReplicationManager().ReplicateCreate( 
			newNetworkId, inGameObject->GetAllStateMask() );
	}
	return newNetworkId;
}

int NetworkMgrSrv::UnregistEntityAndRetNetID( Entity* inGameObject )
{
	int networkId = inGameObject->GetNetworkId();
	{
		MutexLockGuard lock( mutex_ );
		NetworkIdToGameObjectMapCOW();
		mNetworkIdToGameObjectMap->erase( networkId );
	}

	UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	for ( const auto& pair : *tempUdpConnToClientMap )
	{
		pair.second->GetReplicationManager().ReplicateDestroy( networkId );
	}
	return networkId;
}

void NetworkMgrSrv::CheckForDisconnects()
{
	float curTime = RealTimeSrvTiming::sInstance.GetCurrentGameTime();

	if ( curTime - mLastCheckDCTime < kClientDisconnectTimeout )
	{
		return;
	}
	mLastCheckDCTime = curTime;

	float minAllowedTime =
		RealTimeSrvTiming::sInstance.GetCurrentGameTime() - kClientDisconnectTimeout;

	vector< ClientProxyPtr > clientsToDisconnect;

	auto tempUdpConnToClientMap = GetUdpConnToClientMap();
	for ( const auto& pair : *tempUdpConnToClientMap )
	{
		if ( pair.second->GetLastPacketFromClientTime() < minAllowedTime )
		{
			//LOG( "Player %d disconnect", pair.second->GetPlayerId() );
			clientsToDisconnect.push_back( pair.second );
		}
	}

	if ( clientsToDisconnect.size() > 0 )
	{
		for ( auto cliToDC : clientsToDisconnect )
		{
			cliToDC->GetUdpConnection()->forceClose(); // -> NetworkMgrSrv::onConnection
		}
	}
}

void NetworkMgrSrv::SendOutgoingPackets()
{
	float time = RealTimeSrvTiming::sInstance.GetCurrentGameTime();
	if ( time < mTimeOfLastStatePacket + kTimeBetweenStatePackets )
	{
		return;
	}
	mTimeOfLastStatePacket = time;

	UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	for ( const auto& pair : *tempUdpConnToClientMap )
	{
		( pair.second )->GetDeliveryNotificationManager().ProcessTimedOutPackets();

		if ( ( pair.second )->IsLastMoveTimestampDirty() )
		{
			//SendStatePacketToClient( ( pair.second ) );
			SendGamePacket( ( pair.second ), kStateCC );
		}
	}
}

ClientProxyPtr NetworkMgrSrv::GetClientProxy( int inPlayerId )
{
	auto tempPlayerIdToClientMap = GetPlayerIdToClientMap();
	auto it = tempPlayerIdToClientMap->find( inPlayerId );
	if ( it != tempPlayerIdToClientMap->end() )
	{
		return it->second;
	}

	return nullptr;
}

void NetworkMgrSrv::SetStateDirty( int inNetworkId, uint32_t inDirtyState )
{
	UdpConnToClientMapPtr tempUdpConnToClientMap = GetUdpConnToClientMap();
	for ( const auto& pair : *tempUdpConnToClientMap )
	{
		pair.second->GetReplicationManager().SetStateDirty( inNetworkId, inDirtyState );
	}
}

#else

int NetworkMgrSrv::kNewNetworkId = 1;

NetworkMgrSrv::NetworkMgrSrv() :
	mTimeBetweenStatePackets( 0.033f ),
	mLastCheckDCTime( 0.f ),
	mDropPacketChance( 0.f ),
	mSimulatedLatency( 0.f ),
	mWhetherToSimulateJitter( false ),
	mTimeOfLastStatePacket( 0.f )
{}

void NetworkMgrSrv::ProcessIncomingPackets()
{
	ReadIncomingPacketsIntoQueue();

	ProcessQueuedPackets();

	//UpdateBytesSentLastFrame();
}

void NetworkMgrSrv::ReadIncomingPacketsIntoQueue()
{
	char packetMem[MAX_PACKET_BYTE_LENGTH];
	int packetSize = sizeof( packetMem );
	SocketAddrInterface fromAddress;

	int receivedPackedCount = 0;
	int totalReadByteCount = 0;

	while ( receivedPackedCount < kMaxPacketsPerFrameCount )
	{
		int readByteCount = mSocket->ReceiveFrom( packetMem, packetSize, fromAddress );
		if ( readByteCount == 0 )
		{
			//LOG( "ReadIncomingPacketsIntoQueue readByteCount = %d ", 0 );
			break;
		}
		else if ( readByteCount == -WSAECONNRESET )
		{
			HandleConnectionReset( fromAddress );
		}
		else if ( readByteCount > 0 )
		{
			InputBitStream inputStream( packetMem, readByteCount * 8 );
			++receivedPackedCount;
			totalReadByteCount += readByteCount;

			if ( RealTimeSrvMath::GetRandomFloat() >= mDropPacketChance )
			{
				float simulatedReceivedTime =
					RealTimeSrvTiming::sInstance.GetCurrentGameTime() +
					mSimulatedLatency +
					( GetIsSimulatedJitter() ?
						RealTimeSrvMath::Clamp( RealTimeSrvMath::GetRandomFloat(), 0.f, 0.06f ) : 0.f );
				mPacketQueue.emplace( simulatedReceivedTime, inputStream, fromAddress );
			}
			else
			{
				//LOG( "Dropped packet!", 0 );
			}
		}
		else
		{
		}
	}

	if ( totalReadByteCount > 0 )
	{
		//mBytesReceivedPerSecond.UpdatePerSecond( static_cast< float >( totalReadByteCount ) );
	}
}

int NetworkMgrSrv::GetNewNetworkId()
{
	int toRet = kNewNetworkId++;
	if ( kNewNetworkId < toRet )
	{
		LOG( "Network ID Wrap Around!!! You've been playing way too long...", 0 );
	}

	return toRet;
}

void NetworkMgrSrv::ProcessQueuedPackets()
{
	while ( !mPacketQueue.empty() )
	{
		ReceivedPacket& nextPacket = mPacketQueue.front();
		if ( RealTimeSrvTiming::sInstance.GetCurrentGameTime() > nextPacket.GetReceivedTime() )
		{
			ProcessPacket(
				nextPacket.GetPacketBuffer(),
				nextPacket.GetFromAddress(),
				nextPacket.GetUDPSocket()
				// , 
				// nextPacket.GetUdpConnection() 
			);
			mPacketQueue.pop();
		}
		else
		{
			break;
		}
	}
}

bool NetworkMgrSrv::Init( uint16_t inPort )
{
#if _WIN32
	UDPSocketInterface::StaticInit();
#endif //_WIN32

	mSocket = UDPSocketInterface::CreateUDPSocket();

	if ( mSocket == nullptr )
	{
		return false;
	}

	SocketAddrInterface ownAddress( INADDR_ANY, inPort );
	mSocket->Bind( ownAddress );

	LOG( "Initializing NetworkManager at port %d", inPort );

	if ( mSocket->SetNonBlockingMode( true ) != NO_ERROR )
	{
		return false;
	}

	return true;
}

void NetworkMgrSrv::SendPacket( const OutputBitStream& inOutputStream,
	const SocketAddrInterface& inSockAddr )
{
	if ( RealTimeSrvMath::GetRandomFloat() < mDropPacketChance )
	{
		return;
	}

	int sentByteCount = mSocket->SendTo( inOutputStream.GetBufferPtr(),
		inOutputStream.GetByteLength(),
		inSockAddr );
}

void NetworkMgrSrv::HandleConnectionReset( const SocketAddrInterface& inFromAddress )
{
	//auto it = mAddressToClientMap.find( inFromAddress );
	//if ( it != mAddressToClientMap.end() )
	//{
	//	mPlayerIdToClientMap.erase( it->second->GetPlayerId() );
	//	mAddressToClientMap.erase( it );
	//}
}

//int NetworkMgrSrv::RegistEntityAndRetNetID( EntityPtr inGameObject )
//{
//	int newNetworkId = GetNewNetworkId();
//	inGameObject->SetNetworkId( newNetworkId );
//
//	mNetworkIdToGameObjectMap[newNetworkId] = inGameObject;
//	return newNetworkId;
//
//	//for ( const auto& pair : mAddressToClientMap )
//	//{
//	//	pair.second->GetReplicationManager().ReplicateCreate( newNetworkId, inGameObject->GetAllStateMask() );
//	//}
//}
//
//int NetworkMgrSrv::UnregistEntityAndRetNetID( Entity* inGameObject )
//{
//	int networkId = inGameObject->GetNetworkId();
//	mNetworkIdToGameObjectMap.erase( networkId );
//	return networkId;
//
//	//for ( const auto& pair : mAddressToClientMap )
//	//{
//	//	pair.second->GetReplicationManager().ReplicateDestroy( networkId );
//	//}
//}

EntityPtr NetworkMgrSrv::GetGameObject( int inNetworkId )
{
	auto gameObjectIt = mNetworkIdToGameObjectMap.find( inNetworkId );
	if ( gameObjectIt != mNetworkIdToGameObjectMap.end() )
	{
		return gameObjectIt->second;
	}
	else
	{
		return EntityPtr();
	}
}

void NetworkMgrSrv::ProcessPacket(
	InputBitStream& inInputStream,
	const SocketAddrInterface& inFromAddress,
	const UDPSocketPtr& inUDPSocket
	// ,
	// const std::shared_ptr<UdpConnection>& inUdpConnetction
)
{
	auto it = mAddressToClientMap.find( inFromAddress );
	if ( it == mAddressToClientMap.end() )
	{
		HandlePacketFromNewClient(
			inInputStream,
			inFromAddress,
			inUDPSocket
			// , 
			// inUdpConnetction 
		);
	}
	else
	{
		DoProcessPacket( ( *it ).second, inInputStream );
	}
}

void NetworkMgrSrv::HandlePacketFromNewClient(
	InputBitStream& inInputStream,
	const SocketAddrInterface& inFromAddress,
	const UDPSocketPtr& inUDPSocket
	// ,
	// const std::shared_ptr<UdpConnection>& inUdpConnetction
)
{
	uint32_t	packetType;
	inInputStream.Read( packetType );
	if ( packetType == kHelloCC
		|| packetType == kInputCC
		|| packetType == kResetedCC
		)
	{
		std::string playerName = "RealTimeSrvTestPlayerName";
		//inInputStream.Read( playerName );	

		ClientProxyPtr newClientProxy =
			std::make_shared< ClientProxy >(
				inFromAddress,
				playerName,
				kNewPlayerId++,
				inUDPSocket
				// ,
				// inUdpConnetction
				);
		mAddressToClientMap[inFromAddress] = newClientProxy;
		mPlayerIdToClientMap[newClientProxy->GetPlayerId()] = newClientProxy;

		RealTimeSrv::sInstance.get()->HandleNewClient( newClientProxy );

		for ( const auto& pair : mNetworkIdToGameObjectMap )
		{
			newClientProxy->GetReplicationManager().ReplicateCreate( pair.first, pair.second->GetAllStateMask() );
		}

		if ( packetType == kHelloCC )
		{
			//SendWelcomePacket( newClientProxy );
			SendGamePacket( newClientProxy, kWelcomeCC );
		}
		else
		{
			// Server reset
			newClientProxy->SetRecvingServerResetFlag( true );
			//SendResetPacket( newClientProxy );
			SendGamePacket( newClientProxy, kResetCC );
			LOG( "SendResetPacket", 0 );
		}

		LOG( "a new client at socket %s, named '%s' as PlayerID %d ",
			inFromAddress.ToString().c_str(),
			newClientProxy->GetName().c_str(),
			newClientProxy->GetPlayerId()
		);
	}
	//else if ( packetType == kInputCC )
	//{
	//	// Server reset
	//	SendResetPacket( inFromAddress );
	//}
	else
	{
		LOG( "Bad incoming packet from unknown client at socket %s - we're under attack!!", inFromAddress.ToString().c_str() );
	}
}


void NetworkMgrSrv::CheckForDisconnects()
{
	float curTime = RealTimeSrvTiming::sInstance.GetCurrentGameTime();

	if ( curTime - mLastCheckDCTime < kClientDisconnectTimeout )
	{
		return;
	}
	mLastCheckDCTime = curTime;

	float minAllowedTime =
		RealTimeSrvTiming::sInstance.GetCurrentGameTime() - kClientDisconnectTimeout;

	for (
		AddressToClientMap::iterator it = mAddressToClientMap.begin();
		it != mAddressToClientMap.end();
		//++it
		)
	{
		if ( it->second->GetLastPacketFromClientTime() < minAllowedTime )
		{
			LOG( "Player %d disconnect", it->second->GetPlayerId() );

			mPlayerIdToClientMap.erase( it->second->GetPlayerId() );

			mAddressToClientMap.erase( it++ );
		}
		else
		{
			++it;
		}
	}
}

void NetworkMgrSrv::SendOutgoingPackets()
{
	float time = RealTimeSrvTiming::sInstance.GetCurrentGameTime();

	if ( time < mTimeOfLastStatePacket + kTimeBetweenStatePackets )
	{
		return;
	}

	mTimeOfLastStatePacket = time;

	for ( auto it = mAddressToClientMap.begin(), end = mAddressToClientMap.end(); it != end; ++it )
	{
		ClientProxyPtr clientProxy = it->second;

		clientProxy->GetDeliveryNotificationManager().ProcessTimedOutPackets();

		if ( clientProxy->IsLastMoveTimestampDirty() )
		{
			//SendStatePacketToClient( clientProxy );
			SendGamePacket( clientProxy, kStateCC );
		}
	}
}

ClientProxyPtr NetworkMgrSrv::GetClientProxy( int inPlayerId )
{
	auto it = mPlayerIdToClientMap.find( inPlayerId );
	if ( it != mPlayerIdToClientMap.end() )
	{
		return it->second;
	}

	return nullptr;
}

void NetworkMgrSrv::SetStateDirty( int inNetworkId, uint32_t inDirtyState )
{

	for ( const auto& pair : mAddressToClientMap )
	{
		pair.second->GetReplicationManager().SetStateDirty( inNetworkId, inDirtyState );
	}
}
int NetworkMgrSrv::RegistEntityAndRetNetID( EntityPtr inGameObject )
{
	int newNetworkId = GetNewNetworkId();
	inGameObject->SetNetworkId( newNetworkId );

	mNetworkIdToGameObjectMap[newNetworkId] = inGameObject;

	for ( const auto& pair : mAddressToClientMap )
	{
		pair.second->GetReplicationManager().ReplicateCreate( newNetworkId, inGameObject->GetAllStateMask() );
	}
	return newNetworkId;
}

int NetworkMgrSrv::UnregistEntityAndRetNetID( Entity* inGameObject )
{
	mNetworkIdToGameObjectMap.erase( networkId );

	for ( const auto& pair : mAddressToClientMap )
	{
		pair.second->GetReplicationManager().ReplicateDestroy( networkId );
	}
	return networkId;
}

#endif //IS_LINUX


void NetworkMgrSrv::DoProcessPacket( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	inClientProxy->UpdateLastPacketTime();

	uint32_t packetType = HandleServerReset( inClientProxy, inInputStream );

	switch ( packetType )
	{
	case kHelloCC:
		//SendWelcomePacket( inClientProxy );
		SendGamePacket( inClientProxy, kWelcomeCC );
		break;
	case kInputCC:
		// fixme
		if ( inClientProxy->GetDeliveryNotificationManager().ReadAndProcessState( inInputStream ) )
		{
			HandleInputPacket( inClientProxy, inInputStream );
		}
		break;
	case kNullCC:
		break;
	default:
#ifdef IS_LINUX
		LOG_INFO << "Unknown packet type received from "
			<< inClientProxy->GetUdpConnection()->peerAddress().toIpPort();
#else
		LOG( "Unknown packet type received from %s", inClientProxy->GetSocketAddress().ToString().c_str() );
#endif //IS_LINUX
		break;
	}
}

uint32_t NetworkMgrSrv::HandleServerReset( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t packetType;
	inInputStream.Read( packetType );

	if ( packetType == kResetedCC )
	{
		inClientProxy->SetRecvingServerResetFlag( false );
		inInputStream.Read( packetType );
	}

	if ( inClientProxy->GetRecvingServerResetFlag() == true )
	{
		//SendResetPacket( inClientProxy );
		SendGamePacket( inClientProxy, kResetCC );
		return kNullCC;
	}
	else
	{
		return packetType;
	}
}

void NetworkMgrSrv::HandleInputPacket( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t moveCount = 0;
	Action move;
	inInputStream.Read( moveCount, MOVE_COUNT_NUM );

	for ( ; moveCount > 0; --moveCount )
	{
		if ( move.Read( inInputStream ) )
		{
			if ( inClientProxy->GetUnprocessedMoveList().AddMoveIfNew( move ) )
			{
				inClientProxy->SetIsLastMoveTimestampDirty( true );
			}
		}
	}
}

void NetworkMgrSrv::SendGamePacket( ClientProxyPtr inClientProxy, const uint32_t inConnFlag )
{
	OutputBitStream outputPacket;
	outputPacket.Write( inConnFlag );

	InFlightPacket* ifp =
		inClientProxy->GetDeliveryNotificationManager().WriteState( 
			outputPacket, 
			&inClientProxy->GetReplicationManager(),
			this
		);

	switch ( inConnFlag )
	{
	case kResetCC:
	case kWelcomeCC:
		outputPacket.Write( inClientProxy->GetPlayerId() );
		outputPacket.Write( kTimeBetweenStatePackets );
		break;
	case kStateCC:
		WriteLastMoveTimestampIfDirty( outputPacket, inClientProxy );
	default:
		break;
	}

	inClientProxy->GetReplicationManager().Write( outputPacket, ifp );

#ifdef IS_LINUX
	SendPacket( outputPacket, inClientProxy->GetUdpConnection() );
#else
	SendPacket( outputPacket, inClientProxy->GetSocketAddress() );
#endif //IS_LINUX
}

void NetworkMgrSrv::WriteLastMoveTimestampIfDirty( OutputBitStream& inOutputStream, ClientProxyPtr inClientProxy )
{
	bool isTimestampDirty = inClientProxy->IsLastMoveTimestampDirty();
	inOutputStream.Write( isTimestampDirty );
	if ( isTimestampDirty )
	{
		inOutputStream.Write( inClientProxy->GetUnprocessedMoveList().GetLastMoveTimestamp() );
		inClientProxy->SetIsLastMoveTimestampDirty( false );
	}
}