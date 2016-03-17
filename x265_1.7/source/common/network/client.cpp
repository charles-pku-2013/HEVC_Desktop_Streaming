// compile: c++ -o client client_test.cpp desktop_streaming_request.cpp -I/home/charles/smb_share/ffmpeg -lboost_system -L/home/charles/smb_share/ffmpeg_lib -lavformat -lavcodec -lavutil -lswscale -lswresample -lx265 `sdl-config --cflags --libs` -lz -lm -std=c++11 -pthread -g

#include "desktop_streaming_request.hpp"
#include <chrono>
#include <fstream>

using boost::asio::ip::tcp;
using boost::asio::ip::address;

class ClientMsgConnection : public MsgConnection {
public:
    ClientMsgConnection(const std::string &svr_addr, uint16_t port,
                boost::asio::io_service& io_service)
                : MsgConnection(io_service) 
                , connected(false) 
                , svrEp( address::from_string(svr_addr), port )
    { connect(svrEp); }

    void connect( const endpoint_type &server )
    {
        socket().async_connect(server, std::bind(&TcpConnection::handle_connect, 
                    this, std::placeholders::_1));
    }

protected:
    void handle_connect(const boost::system::error_code& error)
    {
        if( error ) {
            OnError(error);
            return;
        }
        DBG_STREAM("ClientMsgConnection::handle_connect()");
        connected = true;

        recvMsg(); //!!?? must put here
    }

    bool isConnected() const { return connected; }

protected:
    bool                connected;
    endpoint_type       svrEp;
};


class ClientDataConnection : public DataConnection {
public:
    ClientDataConnection(const std::string &svr_addr, uint16_t port,
                boost::asio::io_service& io_service)
                : DataConnection(io_service) 
                , connected(false) 
                , svrEp( address::from_string(svr_addr), port )
    { connect(svrEp); }

    void connect( const endpoint_type &server )
    {
        socket().async_connect(server, std::bind(&TcpConnection::handle_connect, 
                    this, std::placeholders::_1));
    }

    bool isConnected() const { return connected; }

protected:
    void handle_connect(const boost::system::error_code& error)
    {
        if( error ) {
            OnError(error);
            return;
        }
        DBG_STREAM("ClientDataConnection::handle_connect()");
        connected = true;
    }

protected:
    bool                connected;
    endpoint_type       svrEp;
};


class TcpClient {
    static const int    HANDLER_NO = 10;
public:
    TcpClient( boost::asio::io_service &io_service,
                const std::string &svr_addr, uint16_t port )
    {
        msgConn.reset( new ClientMsgConnection(svr_addr, port, io_service) );
        dataConn.reset( new ClientDataConnection(svr_addr, port + 1, io_service) );

        msgConn->addMsgHandler( HANDLER_NO, std::bind(&TcpClient::handle_msg, this, 
                    std::placeholders::_1, std::placeholders::_2) );
        msgConn->addErrorHandler( HANDLER_NO, std::bind(&TcpClient::handle_error, this, 
                    std::placeholders::_1, std::placeholders::_2) );
        dataConn->addErrorHandler( HANDLER_NO, std::bind(&TcpClient::handle_error, this, 
                    std::placeholders::_1, std::placeholders::_2) );
    }

    bool Start();

    // TEST
    void writeToFile( boost::asio::streambuf* pBuf, size_t len )
    {
        static int call_time = 0;
        DBG_STREAM("writeToFile len = " << len << " call_time = " << ++call_time);

        using namespace std;

        static ofstream ofs( "test.dat", ios::out | ios::binary );
        ofs << pBuf << flush;

        dataConn->recvData( std::bind(&TcpClient::writeToFile, this, 
                    std::placeholders::_1, std::placeholders::_2) );
    }

    // TEST
    void writeToFile1( BytesArrayPtr pBuf, size_t len )
    {
        static int call_time = 0;
        DBG_STREAM("writeToFile1 len = " << len << " call_time = " << ++call_time);

        using namespace std;

        static ofstream ofs( "test.dat", ios::out | ios::binary );
        ofs.write( pBuf->ptr(), len );
        ofs.flush();

        dataConn->recvData(pBuf, std::bind(&TcpClient::writeToFile1, this, 
                    std::placeholders::_1, std::placeholders::_2));
    }

protected:
    bool isConnected() const 
    { return (msgConn->isConnected() && dataConn->isConnected()); }

    bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn )
    {
        std::cout << "Server message: " << msg << std::endl << std::flush;
        return true;
    }

    bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn )
    {
        DBG_STREAM("client error on connection " << ADDR_STR(conn) << " " << error);
        return true;
    }

protected:
    TcpConnectionPtr        msgConn, dataConn;
// FOR TEST
private:
    void Test1();
    void Test2();
};


bool TcpClient::Start()
{
    using namespace std;

    int count = 1000;         // timeout = 10s
    while( !isConnected() && count-- )
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if( !isConnected() ) {
        cerr << "Connect to server error!" << endl;
        return false;
    } // if

    DBG_STREAM("Client connected to server!");
    msgConn->sendMsg( "service DesktopStreaming\n" );
    RequestPtr pRequest(new DesktopStreamingRequest(msgConn, dataConn));
    pRequest->Start();


/*
 *     // FOR TEST
 *     // Test2();
 * 
 *     // msgConn->recvMsg();
 * 
 *     cout << "Server connected, input command on the console:" << endl;
 *     StringPtr msg;
 * 
 *     // require service
 *     msg.reset( new std::string("service FileTransfer\n") );
 *     msgConn->sendMsg( msg );
 *     // request file
 *     msg.reset( new std::string("get 1.mp4\n") );
 *     msgConn->sendMsg( msg );
 * 
 *     // start receive data thru data conn
 *     dataConn->recvData( std::bind(&TcpClient::writeToFile, this, 
 *                     std::placeholders::_1, std::placeholders::_2) );
 *     // BytesArrayPtr data( new BytesArray );
 *     // data->resize(8192);
 *     // dataConn->recvData(data, std::bind(&TcpClient::writeToFile1, this, 
 *                     // std::placeholders::_1, std::placeholders::_2));
 * 
 *     msg.reset( new std::string );
 *     while( getline(cin, *msg) ) {
 *         msg->append(1, '\n');
 *         msgConn->sendMsg( msg );
 *         msg.reset( new std::string );
 *     } // while
 */

    string msg;
    while( getline(cin, msg) ) {
        msg.append(1, '\n');
        msgConn->sendMsg(msg);
    } // while

    return true;
}



/*
 * static
 * void test1( const std::string &svrIP, uint16_t svrPort )
 * {
 *     boost::asio::io_service io_service;
 *     TcpConnectionPtr pConn( new ClientMsgConnection(svrIP, port, io_service) );
 * }
 */


int main(int argc, char **argv)
{
    if (argc != 2)
        err_ret(-1, "usage: tcpcli <IP:Port>");

    try {
        const char*           svrIP;
        uint16_t              svrPort;

        char *pSep = strchr( argv[1], ':' );
        if( !pSep )
            err_ret(-1, "Bad IPAddr format!");
        *pSep++ = 0;
        svrIP = argv[1];
        if( sscanf(pSep, "%hu", &svrPort) != 1 )
            err_ret( -1, "Bad Port format!" );

        // test1( svrIP, svrPort );
        // return 0;

        boost::asio::io_service io_service;
        TcpClient client(io_service, svrIP, svrPort);

        //!! 在run之前应该绑定好所有的事件对象 connection 等，否则收不到任何反馈
        std::thread workThread( [](boost::asio::io_service &io_service)
                                {io_service.run();}, std::ref(io_service) );
        workThread.detach();

        // getchar();
        client.Start();

    } catch (const std::exception& e) {
        std::cerr << "exception at main(): " << e.what() << std::endl;
    }

    return 0;
}




void TcpClient::Test1()
{
    using namespace std;

    boost::system::error_code error;
    boost::asio::streambuf response;
    std::string word;
    std::istream ins(&response);

    StringPtr msg;
    msg.reset(new std::string);
    getline(cin, *msg);
    msg->append(1, '\n');
    msgConn->sendMsg(msg);
    boost::asio::read_until(msgConn->socket(), response, '\n', error);
    ins.clear();
    ins >> word;
    cout << "word = " << word << endl;

    msg.reset(new std::string);
    getline(cin, *msg);
    msg->append(1, '\n');
    msgConn->sendMsg(msg);
    //!! 如果ins第一次没有读完(读到delim)，因为 delim 字符还在streambuf的get_area中
    // 所以这时候调用read_until会立即返回
    boost::asio::read_until(msgConn->socket(), response, '\n', error);
    ins.clear();
    // 接着输出上次剩下的
    while (ins >> word)
        cout << "word = " << word << endl;

    // 真正接收新数据
    boost::asio::read_until(msgConn->socket(), response, '\n', error);
    ins.clear();
    while (ins >> word)
        cout << "word = " << word << endl;

    return;
}


void TcpClient::Test2()
{
    using namespace std;

    boost::system::error_code error;
    boost::asio::streambuf response;
    std::string word;
    std::istream ins(&response);

    StringPtr msg;
    msg.reset(new std::string);
    getline(cin, *msg);
    msg->append(1, '\n');
    msgConn->sendMsg(msg);
    //boost::asio::read_until(msgConn->socket(), response, '\n', error);
    boost::asio::read(msgConn->socket(), response, boost::asio::transfer_exactly(18), error);
    ins.clear();
    while (ins >> word)
        cout << "word = " << word << endl;

    msg.reset(new std::string);
    getline(cin, *msg);
    msg->append(1, '\n');
    msgConn->sendMsg(msg);
    //!! transfer_all() Reading until a buffer is full 可能永远阻塞
    // transfer_at_least(1) 一有数据就返回
    //!! 新读入的数据不断添加到streambuf的get_area中
    boost::asio::read(msgConn->socket(), response, boost::asio::transfer_at_least(1), error);
    ins.clear();
    while (ins >> word)
        cout << "word = " << word << endl;

    return;
}

/*
 * hello
 * word = Server
 * word = received
 * word = yo
 * fuck you
 * word = ur
 * word = msg:
 * word = hello
 * word = Server
 * word = received
 * word = your
 * word = msg:
 * word = fuck
 * word = you
 */
