#ifndef _DESKTOP_STREAMING_SERVICE_HPP_
#define _DESKTOP_STREAMING_SERVICE_HPP_

#include "service.hpp"

#define INIT_FRAME_SIZE             (256*1024)
// header: 0xFE + seqNO + timestamp + crc + frameSize
#define ENCODED_FRAME_HEADER_LEN        15

struct YuvFrameInfo {
    YuvFrameInfo() {}
    YuvFrameInfo( uint32_t _SeqNO )
            : seqNO( _SeqNO ), timestamp(gen_timestamp()) {}

    uint32_t        seqNO;
    uint32_t        timestamp;
};

#define         YUV_HEADER_LEN sizeof(YuvFrameInfo)

extern std::unique_ptr<boost::asio::deadline_timer>    fps_timer_counter;
extern bool g_fps_count_flag;
extern uint32_t g_fps_count;
extern void FPS_CountHandler(const boost::system::error_code &ec);

/*
 * 有3个线程在工作
 * 1. 从Service继承来的pWorkThread, 用于处理输入命令
 * 2. pCaptureThread StartCapture启动，由x265_main执行input模块StartReader时启动
 * 3. pEncodeThread StartStreaming启动，执行x265_main
 */
class DesktopStreamingService : public Service {
    static const int            HANDLER_NO = 1;
    static const size_t         YUV_BUFSIZE = 2;
public:
    static ServicePtr CreateInstance( ClientInfo *client )
    {
        // pInstance.reset( new DesktopStreamingService(client) );
        // return pInstance;
        ServicePtr ret(new DesktopStreamingService(client));
        pInstance = dynamic_cast<DesktopStreamingService*>(ret.get());
        return ret;
    }

    static DesktopStreamingService* instance()
    { return pInstance; }

    ~DesktopStreamingService()
    {
        EndStreaming();
        DBG_STREAM("DesktopStreamingService destructor");
    }

    void StartStreaming( const std::string &cmd ) // start input and encoder
    {
        if( pEncodeThread )
            EndStreaming();
        yuvSeqNO = 0;
        pEncodeThread.reset( new std::thread(std::bind(&DesktopStreamingService::DoStartEncoder, this, cmd)) );
    }

    void StartCapture() // start continuous capture, called by yuv.cpp startReader, launched by x265
    {
        if( captureRunning )
            StopCapture();

        Start_FPS_Count();

        captureRunning = true;
        pCaptureThread.reset( new std::thread(std::bind(&DesktopStreamingService::DoStartCapture, this)) );
    }

    void StopCapture() // implement pause
    {
        captureRunning = false;
        if( pCaptureThread && pCaptureThread->joinable() )
            pCaptureThread->join();
        pCaptureThread.reset();

        Stop_FPS_Count();
    }

    void EndStreaming() // end all, capture and encoder
    {
        StopCapture();
        yuvSeqNO = 0;

        if( pEncodeThread ) {
            BytesArray emptyBuf;
            yuvBuf.push( emptyBuf );        // make readPicture return false;

            if( pEncodeThread->joinable() )
                pEncodeThread->join();
            pEncodeThread.reset();
        } // if

        DBG_STREAM("Streaming service end.");
    }

    void terminate() // override
    {
        DBG_STREAM("DesktopStreamingService::terminate()");

        EndStreaming();
        Service::terminate();
    }

    void SendEncodedFrame( uint32_t frame_no, const BytesArrayPtr &frame )
    {
        ++g_fps_count;
        uint32_t frameLen = frame->size() - ENCODED_FRAME_HEADER_LEN;
        genFrameHeader( frame_no, frame->ptr(), frameLen );
        pClient->sendData( frame );
    }

    void genFrameHeader( uint32_t frame_no, char *p, uint32_t frameLen )
    {
        using boost::asio::detail::socket_ops::host_to_network_short;
        using boost::asio::detail::socket_ops::host_to_network_long;

        *p++ = (char)0xFE;

        uint32_t nSeqNO = host_to_network_long( frame_no );
        memcpy( p, &nSeqNO, 4 );
        p += 4;

        uint32_t timestamp = gen_timestamp();
        uint32_t nTimestamp = host_to_network_long( timestamp );
        memcpy( p, &nTimestamp, 4 );
        p += 4;

        char *pCRC = p;             // caculate later
        p += 2;

        uint32_t nFrameLen = host_to_network_long( frameLen );
        memcpy( p, &nFrameLen, 4 );
        p += 4;

        // p now points at frame data
        uint16_t crc = crc_checksum( p, frameLen );
        uint16_t nCRC = host_to_network_short( crc );
        memcpy( pCRC, &nCRC, 2 );
    }

    SharedBuffer& YuvBuffer()
    { return yuvBuf; }

    void SetFrameSize(uint32_t _FrameSize)
    { framesize = _FrameSize; }

public:
    bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn )
    {
        DBG_STREAM("DesktopStreamingService received msg from " << ADDR_STR(msg_conn) << ": " << msg);

        // x265 encoding cmd, including all args
        if( msg.find("x265") == 0 ) {
            StartStreaming(msg);
            pClient->sendMsg("Streaming started.\n");
            return true;
        } else if( msg == "pause" ) {
            StopCapture();
            pClient->sendMsg( "Capture paused.\n" );
            return true;
        } else if( msg == "quit" ) {
            terminate();
            pClient->sendMsg( "Streaming terminated.\n" );
            return true;
        } else if( msg == "start" ) {
            if( !captureRunning ) {
                StartCapture();
                pClient->sendMsg( "Capture going on.\n" );
            } else {
                pClient->sendMsg( "Capture already running.\n" );
            } // if
            return true;
        } else if( isdigit(msg[0]) ) { // capture n frames
            if( captureRunning ) {
                pClient->sendMsg( "Capture running! you have to pause first.\n" );
                return true;
            } // if
            std::unique_lock<std::mutex> lk(lock);
            pNextJob.reset( new JobItem(std::bind(&DesktopStreamingService::Capture_n_frames,
                            dynamic_cast<DesktopStreamingService*>(this),
                            std::placeholders::_1, std::placeholders::_2), msg) );
            lk.unlock();
            cond.notify_one();
            return true;
        }

        // unrecogonized msg, just forward to next subscriber
        return false;
    }

    bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn )
    {
        DBG_STREAM("DesktopStreamingService::handle_error on connection " << ADDR_STR(conn) << " " << error);
        // return false means forward it to next handler
        return false;
    }

protected:
    void DoStartEncoder( const std::string &cmd ); // call x265_main
    void DoStartCapture();
    void Capture_n_frames( const std::string &cmd, const ErrType &error ); // run on workthread of Service
    char* CaptureOneFrame(std::size_t &len);        // inplement at yuv.cpp

    void Start_FPS_Count()
    {
        g_fps_count_flag = true;
        g_fps_count = 0;
        fps_timer_counter->expires_from_now(boost::posix_time::seconds(1));
        fps_timer_counter->async_wait( FPS_CountHandler );
    }

    void Stop_FPS_Count()
    {
        g_fps_count_flag = false;
        g_fps_count = 0;
        fps_timer_counter->cancel();
    }

protected:
    int handler_NO() const { return HANDLER_NO; }

protected:
    explicit DesktopStreamingService( ClientInfo *client )
            : Service("DesktopStreaming", client, HANDLER_NO)
            , yuvBuf(YUV_BUFSIZE, YUV_HEADER_LEN)
            , framesize(0), captureRunning(false)
    {
    }

    // DesktopStreamingService( const DesktopStreamingService& ) {}
    // DesktopStreamingService& operator = ( const DesktopStreamingService& ) {}

    static DesktopStreamingService*                   pInstance;

private:
    uint32_t                            framesize;
    uint32_t                            yuvSeqNO;
    bool                                captureRunning;
    SharedBuffer                        yuvBuf;
    std::unique_ptr<std::thread>        pCaptureThread;
    std::unique_ptr<std::thread>        pEncodeThread;

// for fps counting
private:
    // boost::asio::io_service             *fps_io_service;
    // std::unique_ptr<boost::asio::deadline_timer>    fps_timer_counter;

// FOR TEST
private:
    char* ReadFrameFromFile( std::size_t &len );
};


// declare
extern BufferMgr<BytesArray>            gServerBufMgr;

#endif

