#ifndef _REQUEST_HPP_
#define _REQUEST_HPP_

#include "connection.hpp"


class Request {
public:
    Request( const TcpConnectionPtr &pMsgConn, const TcpConnectionPtr &pDataConn,
                int _HandlerNO )
                : msgConn(pMsgConn), dataConn(pDataConn)
                , handlerNO(_HandlerNO)
    {
        msgConn->addMsgHandler( handlerNO, std::bind(&Request::handle_msg, this,
                    std::placeholders::_1, std::placeholders::_2) );
        msgConn->addErrorHandler( handlerNO, std::bind(&Request::handle_error, this,
                    std::placeholders::_1, std::placeholders::_2) );
        dataConn->addErrorHandler( handlerNO, std::bind(&Request::handle_error, this,
                    std::placeholders::_1, std::placeholders::_2) );
    }

    virtual void Start() = 0;

    virtual ~Request()
    {
        // TODO to avoid race, do it in conn's strand
        msgConn->removeMsgHandler(handlerNO);
        msgConn->removeErrorHandler(handlerNO);
        dataConn->removeErrorHandler(handlerNO);
    }

public:
    // async notify
    virtual bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn ) = 0;
    virtual bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn ) = 0;

    void sendMsg( const std::string &msg )
    { sendMsg(std::make_shared<std::string>(msg)); }

    void sendMsg( const StringPtr &pMsg )
    { msgConn->sendMsg(pMsg); }

protected:
    TcpConnectionPtr                msgConn, dataConn;
    int                             handlerNO;
};

typedef std::shared_ptr<Request>        RequestPtr;

#endif

