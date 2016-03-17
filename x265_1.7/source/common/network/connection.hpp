#ifndef _CONNECTION_HPP_
#define _CONNECTION_HPP_

#include "common_utils.hpp"

#define ADDR_STR(conn)          (conn)->socket().remote_endpoint().address().to_string()
#define SHUTDOWN_W              (boost::asio::ip::tcp::socket::shutdown_send)
#define SHUTDOWN_R              (boost::asio::ip::tcp::socket::shutdown_receive)
#define SHUTDOWN_RW             (boost::asio::ip::tcp::socket::shutdown_both)

/*
 * error list:
 * 对方进程退出，正在读的连接 connection read error: asio.misc:2
 */

class TcpConnection : public std::enable_shared_from_this<TcpConnection>
                    , protected boost::noncopyable
{
public:
    enum ConnType { MSG, DATA };
public:
    typedef std::shared_ptr<TcpConnection>                              pointer;
    typedef boost::asio::ip::tcp::socket::endpoint_type                 endpoint_type;
     /*
      * error_handler 和 msg_handler，多个hanler存放在map中，顺序调用每一个handler，任何一个返回true
      * 表示已经处理，否则继续由下一个hanler处理。
      */
    typedef std::function<bool(const boost::system::error_code&, pointer)>      ErrorHandlerType;
    typedef std::function<bool(const std::string&, pointer)>                    MsgHandlerType;
    typedef std::function<void(BytesArrayPtr, size_t)>                          DataHandlerType;
    typedef std::function<void(boost::asio::streambuf*, size_t)>                DataStreamHandlerType;
public:
    explicit TcpConnection(boost::asio::io_service& io_service, ConnType _Type)
                : socket_(io_service), strand_(io_service), type_(_Type) {}

    virtual ~TcpConnection() 
    {
        DBG("Connection to %s destructor type: %s", ADDR_STR(this).c_str(), type() == MSG ? "MSG" : "DATA");
    }

    boost::asio::ip::tcp::socket& socket() { return socket_; }
    boost::asio::io_service::strand& strand() { return strand_; }

    virtual void shutdown(boost::asio::ip::tcp::socket::shutdown_type type) 
    { 
        DBG_STREAM("Shutdowning connection to " << socket().remote_endpoint());
        boost::system::error_code ignored_ec;
        socket_.shutdown(type, ignored_ec);
    }

    virtual void sendData( BytesArrayPtr data ) { DBG_STREAM("TcpConnection::sendData()"); }
    virtual void recvData( BytesArrayPtr data, const DataHandlerType &on_data_handler )
    { DBG_STREAM("TcpConnection::recvData()"); }
    virtual void recvData( const DataStreamHandlerType &on_data_handler, size_t len = 1 )
    { DBG_STREAM("TcpConnection::recvData() stream"); }
    virtual void sendMsg( const std::string &msg ) {}
    virtual void sendMsg( StringPtr msg ) { DBG_STREAM("TcpConnection::sendMsg()"); }
    virtual void recvMsg() { DBG_STREAM("TcpConnection::recvMsg()"); }
    virtual void connect( const endpoint_type &server ) { DBG_STREAM("TcpConnection::connect()"); }

    ConnType type() const { return type_; }
    // void connect( const std::string &server, uint16_t port )
    // {
        // using boost::asio::ip::tcp;
        // using boost::asio::ip::address;
        // tcp::endpoint endpoint(address::from_string(server), port);
        // connect( endpoint );
    // }

// protected: //!! for std::bind they cannot be non-public
    virtual void handle_sendMsg(const boost::system::error_code& error,
                size_t bytes_transferred) {}
    virtual void handle_recvMsg(const boost::system::error_code& error) {}
    virtual void handle_sendData(BytesArrayPtr data, const boost::system::error_code& error,
                size_t bytes_transferred) {}
    virtual void handle_recvData(BytesArrayPtr data, const DataHandlerType &on_data_handler, 
                const boost::system::error_code& error, size_t bytes_transferred) {}
    virtual void handle_recvDataStream(const DataStreamHandlerType &on_data_handler, 
                const boost::system::error_code& error, size_t bytes_transferred) {}
    virtual void handle_connect(const boost::system::error_code& error) {}
    virtual bool isConnected() const { return true; }

    void addErrorHandler( int no, const ErrorHandlerType &err_handler )
    { errHandlers[no] = err_handler; }

    void addMsgHandler( int no, const MsgHandlerType &msg_handler )
    { msgHandlers[no] = msg_handler; }

    void removeErrorHandler( int no )
    { errHandlers.erase(no); }

    void removeMsgHandler( int no )
    { msgHandlers.erase(no); }

    // error categories can check boost/asio/error.hpp
    virtual void OnError(const boost::system::error_code& error) 
    { 
        DBG_STREAM( "Connection to " << socket().remote_endpoint() 
                    << " error: " << error );

        for( auto& v : errHandlers ) {
            if( (v.second)(error, shared_from_this()) )
                break;
        } // for
    }

    virtual void OnMsg( const std::string &msg )
    {
        // DBG_STREAM("Connection to " << socket().remote_endpoint() 
                // << " received msg: " << msg );
        
        for( auto& v : msgHandlers ) {
            if( (v.second)(msg, shared_from_this()) )
                break;
        } // for 
    }

protected:
    boost::asio::ip::tcp::socket        socket_;
    /// Strand to ensure the connection's handlers are not called concurrently.
    boost::asio::io_service::strand     strand_;
    boost::asio::streambuf              recvBuf;
    std::map<int, ErrorHandlerType>     errHandlers;
    std::map<int, MsgHandlerType>       msgHandlers;
    ConnType                            type_;
};

typedef std::shared_ptr<TcpConnection>          TcpConnectionPtr;

//!! 所有的Connection都是TcpConnection*，所以bind的成员函数也必须是TcpConnection的成员函数，
// 所以这些函数被定义为虚函数。

// strand::dispatch under certain conditions the user's handler will be executed immediately,
// dispatch will call it rightaway if the dispatch-caller was called from io_service itself, but queue it otherwise.
// strand::post handler is always added to the queue.
// http://stackoverflow.com/questions/7754695/boost-asio-async-write-how-to-not-interleaving-async-write-calls
class DataConnection : public TcpConnection
{
public:
    DataConnection(boost::asio::io_service& io_service)
            : TcpConnection(io_service, ConnType::DATA) {}

    void sendData( BytesArrayPtr data )
    {
        // final aim is to call async_write
        /*
         * 让io_service执行async_write，如果直接调用，不用strand，不清楚io_service线程中
         * 是否有回调函数正在执行。
         * 因为调用senData的caller肯定不和运行io_service.run()的在一个线程中。所以用post，queue callback
         */
        strand_.post(
                std::bind( &DataConnection::sendDataImpl,
                    std::dynamic_pointer_cast<DataConnection>(shared_from_this()), data )
                );
    }

    /*
     * //!! read does not need strand 调用者要保证read handle_read串行化
     */
    void recvData( BytesArrayPtr data, const DataHandlerType &on_data_handler )
    {
        size_t len = data->size();
        assert( len > 0 );

        // transfer_exactly will omit the data more remain of len
        // at_least_one(1) normally fill up the data buf
        boost::asio::async_read( socket_, boost::asio::buffer(*data),
                boost::asio::transfer_exactly(len),
                std::bind(&TcpConnection::handle_recvData,  shared_from_this(), 
                        data, on_data_handler, std::placeholders::_1, std::placeholders::_2) );
        // boost::asio::async_read( socket_, boost::asio::buffer(*data),
                // boost::asio::transfer_at_least(1),
                // std::bind(&TcpConnection::handle_recvData,  shared_from_this(), 
                        // data, on_data_handler, std::placeholders::_1, std::placeholders::_2) );
    }

    void recvData( const DataStreamHandlerType &on_data_handler, size_t len )
    {
        assert( len > 0 );

        boost::asio::async_read( socket_, recvBuf,
                boost::asio::transfer_at_least(len),
                std::bind(&TcpConnection::handle_recvDataStream, shared_from_this(), 
                        on_data_handler, std::placeholders::_1, std::placeholders::_2) );
    }

protected:
    void sendDataImpl( BytesArrayPtr data )
    {
        outgoingQueue.push_back( data );
        if ( outgoingQueue.size() > 1 ) {
            // leave the job to handle_sendData
            return;
        } // if

        DoSendData();
    }

    void DoSendData()
    {
        BytesArrayPtr data = outgoingQueue.front();

        boost::asio::async_write(socket_, boost::asio::buffer(*data),
                    strand_.wrap(std::bind(&TcpConnection::handle_sendData, shared_from_this(), data,
                        std::placeholders::_1, std::placeholders::_2)));
    }

    void handle_sendData(BytesArrayPtr pData, const boost::system::error_code& error,
                size_t bytes_transferred)
    {
        outgoingQueue.pop_front();

        if( error ) {
            OnError( error );
            return;
        } // if

        if( !outgoingQueue.empty() )
            DoSendData();
    }

    void handle_recvData(BytesArrayPtr data, const DataHandlerType &on_data_handler,
            const boost::system::error_code& error, size_t bytes_transferred)
    {
        // if (error == boost::asio::error::eof)
        if( error ) {
            OnError( error );
            return;
        } // if
        
        // call recvData again in handler
        on_data_handler(data, bytes_transferred);
    }

    void handle_recvDataStream(const DataStreamHandlerType &on_data_handler, 
            const boost::system::error_code& error, size_t bytes_transferred)
    {
        // if (error == boost::asio::error::eof)
            // DBG_STREAM("EOF bytes_transformed = " << bytes_transferred);
        if( error ) {
            OnError( error );
            return;
        } // if

        on_data_handler( &recvBuf, bytes_transferred );
    }

protected:
    std::deque<BytesArrayPtr>        outgoingQueue;
};


class MsgConnection : public TcpConnection
{
public:
    MsgConnection(boost::asio::io_service& io_service)
            : TcpConnection(io_service, ConnType::MSG) {}

    void sendMsg( const std::string &msg )
    { this->sendMsg( std::make_shared<std::string>(msg) ); }

    void sendMsg( StringPtr msg )
    {
        assert( (*msg)[msg->length()-1] == '\n' );
        // std::cout << "SendMsg: " << *msg << std::endl;
        strand_.post(
                std::bind( &MsgConnection::sendMsgImpl,
                    std::dynamic_pointer_cast<MsgConnection>(shared_from_this()), msg  )
                );
    }

    void recvMsg()
    {
        boost::asio::async_read_until(socket_, recvBuf, "\n",
                std::bind(&TcpConnection::handle_recvMsg, shared_from_this(),
                    std::placeholders::_1));
    }

    bool recvMsgSync( std::string &msg )
    {
        boost::system::error_code error;
        boost::asio::streambuf response;

        std::size_t nread = boost::asio::read_until(socket_, response, '\n', error);
        if( !nread ) {
            std::cerr << "MsgConnection::recvMsg error: " << error << std::endl;
            msg.clear();
            return false;
        }

        std::istream response_stream(&response);
        msg.assign( std::istreambuf_iterator<char>(response_stream), 
                            std::istreambuf_iterator<char>() );

        rstrip_string( msg );

        return true;
    }

protected:
    void sendMsgImpl( StringPtr msg )
    {
        outgoingQueue.push_back( msg );
        if ( outgoingQueue.size() > 1 ) {
            // leave the job to handle_sendMsg
            return;
        } // if

        DoSendMsg();
    }

    void DoSendMsg()
    {
        StringPtr msg = outgoingQueue.front();

        boost::asio::async_write(socket_, boost::asio::buffer(*msg),
                    strand_.wrap(std::bind(&TcpConnection::handle_sendMsg, shared_from_this(),
                        std::placeholders::_1, std::placeholders::_2)));
    }

    void handle_sendMsg(const boost::system::error_code& error,
                size_t bytes_transferred)
    {
        outgoingQueue.pop_front();

        if( error ) {
            OnError( error );
            return;
        } // if

        if( !outgoingQueue.empty() )
            DoSendMsg();
    }

    void handle_recvMsg(const boost::system::error_code& error)
    {
        if( error ) {
            OnError(error);
            return;
        }

        std::istream inStream(&recvBuf);
        // recvdMsg.assign( std::istreambuf_iterator<char>(inStream), 
                            // std::istreambuf_iterator<char>() );
        std::getline( inStream, recvdMsg );

        rstrip_string( recvdMsg );

        OnMsg( recvdMsg );

        // read next msg
        recvMsg();
    }

protected:
    std::string                 recvdMsg;
    std::deque<StringPtr>       outgoingQueue;
};



#endif

