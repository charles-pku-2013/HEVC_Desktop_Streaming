#ifndef _SERVICE_HPP_
#define _SERVICE_HPP_

#include "connection.hpp"

class Service;
typedef std::shared_ptr<Service>        ServicePtr;

struct ClientInfo {
    typedef boost::asio::ip::tcp::socket::endpoint_type  endpoint_type;

    ClientInfo() : msgConnReady(false), dataConnReady(false) {}

    ~ClientInfo();

    bool ready() const { return (msgConnReady && dataConnReady); }

    void addService( const std::string &name, ServicePtr svr )
    { services[name] = svr; }

    void removeService( const std::string &name )
    { services.erase(name); }

    void sendMsg( const std::string &msg )
    { sendMsg(std::make_shared<std::string>(msg)); }

    void sendMsg( const StringPtr &pMsg )
    { msgConn->sendMsg(pMsg); }

    void sendData( const BytesArrayPtr &pBuf )
    { dataConn->sendData( pBuf ); }

    bool                            msgConnReady;
    bool                            dataConnReady;
    TcpConnectionPtr                msgConn, dataConn;
    std::map<std::string, ServicePtr>   services;
};

typedef std::shared_ptr<ClientInfo>     ClientInfoPtr;


class Service {
public:
    typedef boost::system::error_code                                   ErrType;
    typedef std::function<void(const std::string&, const ErrType&)>   JobRoutine;

    struct JobItem {
        JobItem( const JobRoutine &_Routine,
                    const std::string &_Msg = std::string(), const ErrType &_Error = ErrType() )
                : routine(_Routine), msg(_Msg), error(_Error) {}

        JobRoutine          routine;
        std::string         msg;
        ErrType             error;
    };

public:
    explicit Service( const std::string &_Name, ClientInfo *_Client, int _HandlerNO )
            : name_(_Name), pClient(_Client), handlerNO(_HandlerNO)
            , active(false), jobRunning(false)
    {
        pClient->msgConn->addMsgHandler( handlerNO, std::bind(&Service::handle_msg, this,
                    std::placeholders::_1, std::placeholders::_2) );
        pClient->msgConn->addErrorHandler( handlerNO, std::bind(&Service::handle_error, this,
                    std::placeholders::_1, std::placeholders::_2) );
        pClient->dataConn->addErrorHandler( handlerNO, std::bind(&Service::handle_error, this,
                    std::placeholders::_1, std::placeholders::_2) );
    }

    virtual ~Service()
    {
        // TODO to avoid race, do it in conn's strand
        pClient->msgConn->removeMsgHandler(handlerNO);
        pClient->msgConn->removeErrorHandler(handlerNO);
        pClient->dataConn->removeErrorHandler(handlerNO);

        if( pWorkThread ) {
            jobRunning = false;     // make job routine return
            active = false;
            cond.notify_one();
            if (pWorkThread->joinable()) {
                pWorkThread->join();
            } // if
        } // if

        DBG_STREAM("Service " << name() << " destructor");
    }

    const std::string& name() const { return name_; }

    virtual void start()
    {
        if( active ) {
            terminate();
        } // if

        active = true;
        pWorkThread.reset( new std::thread(std::bind(&Service::DoWork, this)) );
        // pWorkThread->detach();
    }

    virtual void terminate() // end all
    {
        DBG_STREAM("Service::terminate()");

        jobRunning = false;     // make job routine return
        active = false;
        cond.notify_one();
        if (pWorkThread && pWorkThread->joinable()) {
            pWorkThread->join();
        } // if
        pWorkThread.reset();
    }

public:
    // async notify
    virtual bool handle_msg( const std::string &msg, TcpConnectionPtr msg_conn ) = 0;
    virtual bool handle_error( const boost::system::error_code& error, TcpConnectionPtr conn ) = 0;

protected:
    virtual void DoWork()
    {
        DBG_STREAM("Service " << name_ << " start!");

        std::unique_ptr<JobItem>    pCurJob;
        while( active ) {
            std::unique_lock<std::mutex> lk(lock);
            while( active && !pNextJob )
                cond.wait(lk);
            if( !active ) {
                lk.unlock();
                break;
            } // if active
            pCurJob.reset( pNextJob.release() );
            lk.unlock();
            jobRunning = true;
            (pCurJob->routine)(pCurJob->msg, pCurJob->error); // maybe very long
        } // while

        DBG_STREAM("Service " << name_ << " end!");
    }

protected:
    std::string                 name_;
    bool                        active; // when active is false, stop populating new job in DoWork()
    bool                        jobRunning; // job func need to check it
    std::unique_ptr<JobItem>    pNextJob;
    std::mutex                  lock;
    std::condition_variable     cond;
    ClientInfo*                 pClient;
    std::unique_ptr<std::thread> pWorkThread;
    int                          handlerNO;
};



#endif

