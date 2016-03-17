#ifndef _LOG_H
#define _LOG_H

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <iomanip>
//#include <sys/time.h>


//!! __VA_ARGS__ 只是原样展开...
#define DBG(...) do { \
            fprintf( stderr,  "%s:%d: ", __FILE__, __LINE__ ); \
            fprintf( stderr, __VA_ARGS__ ); \
            fprintf( stderr, "\n" ); \
            fflush( stderr ); \
        } while(0)

//!! args可以是包含空格和其他任意符号如<<的字符串
#define DBG_STREAM(args) do { \
            std::stringstream __dbg_stream_stringstream; \
            __dbg_stream_stringstream << __FILE__ << ":" << __LINE__ << ": " \
                            << args; \
            fprintf(stderr, "%s\n", __dbg_stream_stringstream.str().c_str()); \
            fflush( stderr ); \
        } while(0)

#define DBG_COND(cond, ...) do { \
            if(cond) DBG(__VA_ARGS__);  \
        } while(0)

#define DBG_STREAM_COND(cond, args) do { \
            if(cond) DBG_STREAM(args); \
        } while(0)






#endif
