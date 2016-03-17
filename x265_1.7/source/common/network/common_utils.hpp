#ifndef _COMMON_UTILS_HPP_
#define _COMMON_UTILS_HPP_

#include <iostream>
#include <vector>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <string>
#include <cstring>
#include <boost/crc.hpp>
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp> 
#include "../LOG.h"


#define err_ret( retval, ... ) do { \
                            fprintf( stderr, __VA_ARGS__ ); \
                            fputc('\n', stderr); \
                            return retval; \
                        } while(0)

static inline
void rstrip_string( std::string &s )
{
    static const char *SPACES = " \t\f\r\v\n";

    std::string::size_type pos = s.find_last_not_of( SPACES );
    if( pos != std::string::npos )
        s.resize( pos + 1 );
    else
        s.clear();
}

static inline
unsigned short crc_checksum(const void *buf, size_t count)
{
    boost::crc_16_type         result;
    result.process_bytes( buf, count );

    return (unsigned short)(result.checksum());
}

static inline
uint32_t gen_timestamp()
{
    typedef std::chrono::system_clock Clock;

    auto now = Clock::now().time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now);

    return (uint32_t)(milliseconds.count());

    // return 0;
}


struct BytesArray : std::vector<char> {
    typedef std::vector<char>               BaseType;

    BytesArray() {}

    BytesArray( size_type n )
    { reserve(n); }

    char* ptr() { return &(*this)[0]; }
    const char* ptr() const { return &(*this)[0]; }

    void append( const void *src, size_type n )
    {
        size_type cursize = this->size();
        this->resize( cursize + n );
        char *dst = ptr() + cursize;
        ::memcpy( dst, src, n );
    }
    
    void append( char data )
    { this->push_back(data); }

    void setData( const void *src, size_type n )
    {
        this->resize( n );
        char *dst = ptr();
        ::memcpy( dst, src, n );
    }

    void setData( char data )
    {
        this->clear();
        this->append( data );
    }
};

typedef std::shared_ptr<BytesArray>             BytesArrayPtr;


class SharedBuffer {
public:
    SharedBuffer( std::size_t _QueSize, std::size_t _ArrSize = 0 ) 
                    : front(0), rear(0), queSize(_QueSize + 1)
    {
        queue.resize( queSize );
        if( _ArrSize ) {
            for( std::size_t i = 0; i < queSize; ++i )
                queue[i].reserve( _ArrSize );
            // readBuf_.reserve( _ArrSize );
            // writeBuf_.reserve( _ArrSize );
        } // if 

        // DBG("SharedBuffer constructor queSize = %lu arrSize = %lu @%lx\n", queSize, _ArrSize, (long)this);
    }

    // for dbg
    // ~SharedBuffer()
    // { DBG("SharedBuffer destructor @%lx\n", (long)this); }

    // bool full()
    // {
        // std::unique_lock<std::mutex> lk(lock);
        // return _full();
    // }

    // bool empty()
    // {
        // std::unique_lock<std::mutex> lk(lock);
        // return _empty();
    // }

    // NOTE!!! only for single writer
    BytesArray& writeBuf() { return queue[rear]; }
    // BytesArray& readBuf()  { return readBuf_; }

    void push( BytesArray &arr )
    {
        // DBG("push @%lx", (long)this);
        std::unique_lock<std::mutex> lk(lock);

        while( _full() )
            condWr.wait(lk);
            
        queue[rear].swap( arr );
        rear = (rear + 1) % queSize;

        lk.unlock();
        condRd.notify_one();
    }

    void push()
    { this->push( writeBuf() ); }

    void pop( BytesArray &arr )
    {
        // DBG("pop @%lx", (long)this);
        std::unique_lock<std::mutex> lk(lock);

        while( _empty() )
            condRd.wait(lk);
            
        queue[front].swap( arr );
        front = (front + 1) % queSize;

        lk.unlock();
        condWr.notify_one();
    }

    /*
     * void clear()
     * {
     *     std::unique_lock<std::mutex> lk(lock);
     *     front = rear = 0;
     * }
     */

    // void pop()
    // { this->pop( readBuf_ ); }

protected:
    bool _full()  { return ((rear + 1) % queSize == front); }
    bool _empty() { return (front == rear); }

protected:
    std::size_t                                                             front, rear;
    const std::size_t                                                       queSize;
    std::mutex                                                              lock;
    std::condition_variable                                                 condRd;
    std::condition_variable                                                 condWr;
    std::vector<BytesArray>                                                 queue;
    // BytesArray                                                              readBuf_, writeBuf_;
};


/*
 * 用来快速生成BytesArray，省去反复分配内存，与SharedBuffer无关
 */
template <typename ContainerType>
class BufferMgr {
    typedef std::shared_ptr<ContainerType>          ElemType;
public:
    BufferMgr( std::size_t _ReserveSize, std::size_t _ListSize )
            : RESERVE_SIZE(_ReserveSize), LIST_SIZE(_ListSize) {}

    ElemType get()
    {
        ElemType ret;

        std::unique_lock<std::mutex> lk(lock);
        if( !bufList.empty() ) {
            ret = std::move(bufList.front());
            bufList.pop_front();
            return std::move(ret);
        } // if

        lk.unlock();
        ret.reset( new ContainerType );
        ret->reserve( RESERVE_SIZE );
        return std::move(ret);
    }

    void put( ElemType elem )
    {
        elem->clear();
        std::unique_lock<std::mutex> lk(lock);
        if( bufList.size() < LIST_SIZE )
            bufList.push_back( std::move(elem) ); 
    }

private:
    std::deque<ElemType>         bufList;
    std::mutex                  lock;
    const std::size_t           RESERVE_SIZE, LIST_SIZE;
};


template < typename T >
class SharedQueue : std::deque<T> {
public:
    SharedQueue( std::size_t _MaxSize = UINT_MAX ) 
                : maxSize(_MaxSize) {}

    bool full() const { return this->size() >= maxSize; }

    void push( const T &elem )
    {
        std::unique_lock<std::mutex> lk(lock);

        while( this->full() )
            condWr.wait( lk );

        this->push_back( elem );

        lk.unlock();
        condRd.notify_one();
    }

    T pop()
    {
        std::unique_lock<std::mutex> lk(lock);
        
        while( this->empty() )
            condRd.wait( lk );

        T retval = this->front();
        this->pop_front();

        lk.unlock();
        condWr.notify_one();

        return retval;
    }

protected:
    std::size_t                 maxSize;
    std::mutex                  lock;
    std::condition_variable     condRd;
    std::condition_variable     condWr;
};


typedef std::shared_ptr<std::string>        StringPtr;

#endif
