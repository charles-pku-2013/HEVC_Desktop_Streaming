// compile: c++ -o server server_test.cpp -lboost_system -std=c++11 -pthread -g

#include "network/desktop_streaming_service.hpp"
#include <sstream>
#include <fstream>

#define SERVER_PORT                 8888

using boost::asio::ip::tcp;

// def
BufferMgr<BytesArray>           gServerBufMgr(INIT_FRAME_SIZE, 10);


class ServerDataConnection : public DataConnection {
public:
    ServerDataConnection(boost::asio::io_service& io_service)
                : DataConnection(io_service) {}
protected:
    void handle_sendData(BytesArrayPtr pData, const boost::system::error_code& error,
                size_t bytes_transferred)
    {
        DBG_STREAM("ServerDataConnection::handle_sendData() bytes_transferred = " << bytes_transferred);
        DataConnection::handle_sendData( pData, error, bytes_transferred );
        gServerBufMgr.put( pData );
    }
};


class FileTransferService : public Service {
    static const int HANDLER_NO = 1;
public:
    FileTransferService( ClientInfo *client )
            : Service("FileTransfer", client, HANDLER_NO) {}

    bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn )
    {
        DBG_STREAM("FileTransferService received msg from " << ADDR_STR(msg_conn) << ": " << msg);

        std::stringstream sstr(msg);
        std::string cmd, arg;

        sstr >> cmd;
        if( "stop" == cmd ) {
            terminate();
            return true;
        } else if( "get" == cmd ) {
            sstr >> arg;
            if( !sstr ) return false;
            std::unique_lock<std::mutex> lk(lock);
            pNextJob.reset( new JobItem(std::bind(&FileTransferService::SendFile,
                            dynamic_cast<FileTransferService*>(this),
                            std::placeholders::_1, std::placeholders::_2), arg) );
            lk.unlock();
            cond.notify_one();
            return true;
        }

        return false;
    }

    bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn )
    {
        DBG_STREAM("FileTransferService::handle_error on connection " << ADDR_STR(conn) << " " << error);
        // return false means forward it to next handler
        return false;
    }

    void SendFile( const std::string &filename, const ErrType &error )
    {
        DBG_STREAM("FileTransferService::SendFile: " << filename);

        using namespace std;

        char msgBuf[128];

        ifstream ifs( filename, ios::in | ios::binary );

        if( !ifs ) {
            sprintf( msgBuf, "File %s not exists.\n", filename.c_str() );
            pClient->sendMsg( msgBuf );
            return;
        } // if

        std::size_t nread = 0;
        do {
            BytesArrayPtr pBuf = gServerBufMgr.get();
            pBuf->resize( INIT_FRAME_SIZE );
            ifs.read( pBuf->ptr(), INIT_FRAME_SIZE );
            nread = ifs.gcount();
            if( nread > 0 ) {
                pBuf->resize( nread );
                pClient->sendData( pBuf );
                DBG_STREAM("Send " << nread << " bytes to server.");
            } // if
        } while( jobRunning && nread > 0 );

        if(ifs.bad()) {
            sprintf(msgBuf, "read file %s fail!\n", filename.c_str());
            DBG_STREAM(msgBuf);
        } else if( ifs.eof() ) {
            sprintf(msgBuf, "send file %s finish!\n", filename.c_str());
            DBG_STREAM(msgBuf);
        } // if ifs

        pClient->sendMsg( msgBuf );
    }

protected:
    int handler_NO() const { return HANDLER_NO; }
};


class ServiceFactory {
public:
    ServicePtr CreateService( const std::string &msg, ClientInfo *pClient )
    {
        if( msg.find("FileTransfer") != std::string::npos )
            return std::make_shared<FileTransferService>( pClient );
        else if( msg.find("DesktopStreaming") != std::string::npos )
            return DesktopStreamingService::CreateInstance( pClient );

        return nullptr;
    }

    static ServiceFactory& instance()
    { return _instance; }

private:
    ServiceFactory(){}
    ServiceFactory( const ServiceFactory& );
    ServiceFactory& operator= (const ServiceFactory&);

    static ServiceFactory       _instance;
};

ServiceFactory                  ServiceFactory::_instance;



class TcpServer
{
public:
    TcpServer(boost::asio::io_service& io_service)
                : msgAcceptor(io_service, tcp::endpoint(tcp::v4(), SERVER_PORT))
                , dataAcceptor(io_service, tcp::endpoint(tcp::v4(), SERVER_PORT + 1))
    {
        start();
    }

protected:
    void start()
    {
        MsgAccept();
        DataAccept();
        std::cout << "server started at port " << SERVER_PORT << std::endl;
    }

    void MsgAccept()
    {
        TcpConnectionPtr msgConn( new MsgConnection(msgAcceptor.get_io_service()) );
        msgConn->addErrorHandler( 10, std::bind(&TcpServer::handle_error, this, std::placeholders::_1, std::placeholders::_2) );
        msgConn->addMsgHandler( 10, std::bind(&TcpServer::handle_msg, this, std::placeholders::_1, std::placeholders::_2) );
        msgAcceptor.async_accept( msgConn->socket(),
               std::bind(&TcpServer::handle_MsgAccept, this, msgConn, std::placeholders::_1) );
    }

    void DataAccept()
    {
        TcpConnectionPtr dataConn( new ServerDataConnection(dataAcceptor.get_io_service()) );
        dataConn->addErrorHandler( 10, std::bind(&TcpServer::handle_error, this, std::placeholders::_1, std::placeholders::_2) );
        dataAcceptor.async_accept( dataConn->socket(),
               std::bind(&TcpServer::handle_DataAccept, this, dataConn, std::placeholders::_1) );
    }

    void handle_MsgAccept(TcpConnectionPtr msg_conn,
                const boost::system::error_code& error)
    {
        if( error ) {
            std::cerr << "handle_MsgAccept() error: " << error << std::endl;
            return;
        }

        // Serve one client at one time
        if( connectedClients.size() > 0 ) {
            std::cout << "Thread " << std::this_thread::get_id()
                    << " refused msg connection from " << msg_conn->socket().remote_endpoint() << std::endl;
            MsgAccept();
            return;
        }

        std::cout << "Thread " << std::this_thread::get_id()
                << " accepted msg connection from " << msg_conn->socket().remote_endpoint() << std::endl;

        std::string cliAddr = ADDR_STR(msg_conn);
        std::unique_lock<std::mutex> lk(lock);
        auto ret = notReadyClients.insert( std::make_pair(cliAddr, std::make_shared<ClientInfo>()) );
        ClientInfoPtr pClient = (ret.first)->second;
        pClient->msgConn = msg_conn;
        pClient->msgConnReady = true;
        if( pClient->ready() ) {
            auto ret1 = connectedClients.insert( *(ret.first) );
            notReadyClients.erase( ret.first );
            lk.unlock();
            if( !(ret1.second) )
                std::cerr << "client " << cliAddr << " already exists!" << std::endl;
            else
                servClient( *pClient );
        } else {
            lk.unlock();
        } // if

        MsgAccept();
    }

    void handle_DataAccept(TcpConnectionPtr data_conn,
                const boost::system::error_code& error)
    {
        if( error ) {
            std::cerr << "handle_DataAccept() error: " << error << std::endl;
            return;
        }

        // Serve one client at one time
        if( connectedClients.size() > 0 ) {
            std::cout << "Thread:" << std::this_thread::get_id()
                    << " refused data connection from " << data_conn->socket().remote_endpoint() << std::endl;
            DataAccept();
            return;
        }

        std::cout << "Thread:" << std::this_thread::get_id()
                << " accepted data connection from " << data_conn->socket().remote_endpoint() << std::endl;

        std::string cliAddr = ADDR_STR(data_conn);
        std::unique_lock<std::mutex> lk(lock);
        auto ret = notReadyClients.insert( std::make_pair(cliAddr, std::make_shared<ClientInfo>()) );
        ClientInfoPtr pClient = (ret.first)->second;
        pClient->dataConn = data_conn;
        pClient->dataConnReady = true;
        if( pClient->ready() ) {
            auto ret1 = connectedClients.insert( *(ret.first) );
            notReadyClients.erase( ret.first );
            lk.unlock();
            if( !(ret1.second) )
                std::cerr << "client " << cliAddr << " already exists!" << std::endl;
            else
                servClient( *pClient );
        } else {
            lk.unlock();
        } // if

        DataAccept();
    }

    void servClient( ClientInfo &client )
    {
        client.msgConn->recvMsg();
    }

    void removeClient( const std::string &addr )
    {
        std::unique_lock<std::mutex> lk(lock);
        connectedClients.erase(addr);
    }

    // TODO 应该对错误进行分类，忽略不严重的错误
    bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn )
    {
        DBG_STREAM("TcpServer::handle_error() on connection " << conn->socket().remote_endpoint() << " " << error);
        std::string cliAddr = ADDR_STR(conn);
        std::cout << "client " << cliAddr << " quit." << std::endl;
        removeClient( cliAddr );

        // last handler in handler_chain, so always return true;
        return true;
    }

    bool handle_msg( const std::string &msg, TcpConnectionPtr conn )
    {
        DBG_STREAM( "received msg from " << conn->socket().remote_endpoint() << ": " << msg );

        // StringPtr response( new std::string("Server received your msg: ") );
        // response->append( msg );
        // response->append( 1, '\n' );
        // conn->sendMsg( response );
        // return true;

        std::stringstream sstr(msg);
        StringPtr pKeyword(new std::string);

        sstr >> *pKeyword;
        if( "service" == *pKeyword ) {
            sstr >> *pKeyword;
            auto it = connectedClients.find( ADDR_STR(conn) );
            if( it == connectedClients.end() ) {
                *pKeyword = "Your info is not tracked on the server.\n";
                conn->sendMsg( pKeyword );
                return false;
            }
            ClientInfoPtr pClient = it->second;
            ServicePtr pService = ServiceFactory::instance().CreateService( *pKeyword, pClient.get() );
            if( !pService ) {
                *pKeyword = "Invalid service request!\n";
                conn->sendMsg( pKeyword );
                return false;
            }
            pClient->addService( pService->name(), pService );
            pService->start();
            *pKeyword = "Request service ";
            pKeyword->append( pService->name() ).append( " success.\n" );
            conn->sendMsg( pKeyword );
            return true;
        } else {
            *pKeyword = "Invalid request!\n";
            conn->sendMsg( pKeyword );
            return false;
        }

        // last handler in handler_chain, so always return true;
        return true;
    }

protected:
    tcp::acceptor               msgAcceptor;
    tcp::acceptor               dataAcceptor;
protected:
    std::map< std::string, ClientInfoPtr >      notReadyClients, connectedClients;
    std::mutex                                  lock;
};


// def provided
DesktopStreamingService* DesktopStreamingService::pInstance = NULL;

std::unique_ptr<boost::asio::deadline_timer>    fps_timer_counter;
bool g_fps_count_flag = false;
uint32_t g_fps_count = 0;

void FPS_CountHandler(const boost::system::error_code &ec)
{
    using namespace std;
    cout << "FPS is " << g_fps_count << endl;
    g_fps_count = 0;
    if( g_fps_count_flag ) {
        fps_timer_counter->expires_from_now(boost::posix_time::seconds(1));
        fps_timer_counter->async_wait( FPS_CountHandler );    
    } else {
        fps_timer_counter->cancel();
    } // if
}


int main( int argc, char **argv )
{
    try {
        boost::asio::io_service io_service;
        fps_timer_counter.reset(new boost::asio::deadline_timer(io_service, boost::posix_time::seconds(1)));
        TcpServer server(io_service);
        io_service.run();

    } catch ( const std::exception &ex ) {
        std:: cerr << "Exception caught: " << ex.what() << std::endl;
        // throw ex;
        return -1;
    }

    return 0;
}

