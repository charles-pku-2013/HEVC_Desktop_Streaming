#ifndef _DESKTOP_STREAMING_REQUEST_HPP_
#define _DESKTOP_STREAMING_REQUEST_HPP_

#include "request.hpp"
#include <deque>

#define INIT_FRAME_SIZE             (256*1024)
// header: 0xFE + seqNO + timestamp + crc + frameSize
#define ENCODED_FRAME_HEADER_LEN        15


struct RecvdFrame {
    // RecvdFrame() : seqNO_(0), cksum_(0), timestamp_(0)
                 // , pData( gRecvBufMgr.get() ) {}

    char *ptr() { return pData->ptr(); }
    size_t size() const { return pData->size(); }

    // BytesArray& operator*() { return *pData; }

    BytesArrayPtr           pData;
    uint32_t                seqNO_;
    uint16_t                cksum_;
    uint32_t                timestamp_; // encoded time
};

typedef std::shared_ptr<RecvdFrame>     RecvdFramePtr;


// FOR DEBUG
namespace std {
    inline
    ostream& operator << ( ostream &os, const RecvdFrame &frm )
    {
        os << "RecvdFrame SeqNO: " << frm.seqNO_ << " size: " << frm.size() 
            << " checksum: " << frm.cksum_ << " created at " << frm.timestamp_;
        return os;
    }
} // namespace std 


class DesktopStreamingRequest : public Request {
    static const int            HANDLER_NO = 1;
    static const int            PLAYER_BUFSIZE = (512*1024);
public:
    DesktopStreamingRequest( const TcpConnectionPtr &msg_conn, const TcpConnectionPtr &data_conn )
                : Request(msg_conn, data_conn, HANDLER_NO)
    {
        pHeaderBuf.reset( new BytesArray );
        pHeaderBuf->resize( ENCODED_FRAME_HEADER_LEN );
    }

    // TODO can specify args
    void Start()
    {
        StringPtr pMsg = std::make_shared<std::string>("x265 - --preset ultrafast --bframes 0 --rc-lookahead 0 --ref 1 --no-b-pyramid --input-res 1920x1080 --input-csp i444 --fps 60 -o -\n");
        StartPlayer();
        RequestFrameHeader();
        msgConn->sendMsg(pMsg);
    }

    void StartPlayer()
    {
        playerThread.reset( new std::thread(std::bind(&DesktopStreamingRequest::PlayerRoutine, 
                        dynamic_cast<DesktopStreamingRequest*>(this))) );
        playerThread->detach();
    }

    int PlayerRoutine();

    void OnFrameHeader( BytesArrayPtr data, size_t len )
    {
        assert( len == ENCODED_FRAME_HEADER_LEN );
        assert( data == pHeaderBuf );

        char *p = data->ptr();
        if( *p++ != (char)0xfe ) {
            std::cerr << "wrong frame header format!" << std::endl;
            return;
        } // if

        using boost::asio::detail::socket_ops::network_to_host_short;
        using boost::asio::detail::socket_ops::network_to_host_long;

        // read seqNO
        memcpy( &(nextFrame.seqNO_), p, 4 );
        p += 4;
        nextFrame.seqNO_ = network_to_host_long( nextFrame.seqNO_ );

        // read timestamp
        memcpy( &(nextFrame.timestamp_), p, 4 );
        p += 4;
        nextFrame.timestamp_ = network_to_host_long( nextFrame.timestamp_ );

        // read crc
        memcpy( &(nextFrame.cksum_), p, 2 );
        p += 2;
        nextFrame.cksum_ = network_to_host_short( nextFrame.cksum_ );

        // read framesize 
        uint32_t framesize;
        memcpy( &framesize, p, 4 );
        framesize = network_to_host_long( framesize );
        nextFrame.pData = gRecvBufMgr.get();
        nextFrame.pData->resize( framesize );

        RequestFrameBody();
    }

    void OnFrameBody( BytesArrayPtr data, size_t len )
    {
        assert( len == data->size() );
        assert( data == nextFrame.pData );

        RequestFrameHeader();

        // for debug
        uint16_t crc = crc_checksum( data->ptr(), data->size() );
        if( crc != nextFrame.cksum_ ) {
            DBG_STREAM( "checksum inconsistent on frame: " << nextFrame << " local crc is " << crc );
            // exit(-1);
        } // if 

        frameQueue.push( nextFrame );
        DBG_STREAM( "Received " << nextFrame << " recv_time: " << gen_timestamp() );
    }

    // for player read_packet use
    SharedQueue<RecvdFrame>& FrameQueue() { return frameQueue; }

public:
    bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn )
    {
        DBG_STREAM("DesktopStreamingRequest received msg: " << msg);
        return false;
    }

    bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn )
    {
        DBG_STREAM("DesktopStreamingRequest::handle_error() " << error);
        return false;
    }

protected:
    void RequestFrameHeader()
    {
        dataConn->recvData( pHeaderBuf, std::bind(&DesktopStreamingRequest::OnFrameHeader, this, 
                    std::placeholders::_1, std::placeholders::_2) );
    }

    void RequestFrameBody()
    {
        dataConn->recvData( nextFrame.pData, std::bind(&DesktopStreamingRequest::OnFrameBody, this,
                    std::placeholders::_1, std::placeholders::_2) );
    }

protected:
    BytesArrayPtr                   pHeaderBuf;
    RecvdFrame                      nextFrame;
    SharedQueue<RecvdFrame>         frameQueue;
    std::unique_ptr<std::thread>    playerThread;
    static BufferMgr<BytesArray>    gRecvBufMgr;
};



#endif

