#include "desktop_streaming_request.hpp"
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <SDL.h>
#include <SDL_thread.h>
} // extern "C"

BufferMgr<BytesArray>    DesktopStreamingRequest::gRecvBufMgr(INIT_FRAME_SIZE, 10);


static
DesktopStreamingRequest *pInstance = NULL;
static
BufferMgr<BytesArray>   *pBufMgr = NULL;

static
int WriteJPEG( const AVCodecContext *pCodecCtx, AVFrame *pFrame, const char *filename );

static 
int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    int             nread;
    RecvdFrame      frame;

    frame = pInstance->FrameQueue().pop();
    nread = frame.pData->size();
    assert( nread <= buf_size );            //!!
    memcpy( buf, frame.pData->ptr(), nread );
    DBG_STREAM("Player required a frame: " << frame << " when " << gen_timestamp());
    pBufMgr->put( frame.pData );

    // DEBUG
    // static std::ofstream ofs( "client.h265", std::ios::out | std::ios::binary );
    // ofs.write( frame.pData->ptr(), nread );
    // ofs.flush();

    return nread;
}

int DesktopStreamingRequest::PlayerRoutine()
{
    AVFormatContext*                    fmt_ctx = NULL;
    AVIOContext*                        avio_ctx = NULL;
    AVCodecContext*                     pCodecCtx = NULL;
    unsigned char*                      readBuf = NULL;
    int                     i, ret = 0, videoStreamIndex;
    AVCodec         *pCodec = NULL;
    AVFrame         *pFrame = NULL; 
    AVPacket        packet;
    int             frameFinished;

    AVDictionary    *optionsDict = NULL;
    struct SwsContext *sws_ctx = NULL;

    SDL_Overlay     *bmp = NULL;
    SDL_Surface     *screen = NULL;
    SDL_Rect        rect;
    SDL_Event       event;

    // JPG_Converter   jpg_converter;
    uint32_t        decodeSeqNO = 0;
    char            jpgFileName[64];

    pInstance = this;
    pBufMgr = &gRecvBufMgr;

    /* register codecs and formats and other lavf/lavc components*/
    av_register_all();

    if (!(fmt_ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        DBG("avformat_alloc_context fail!");
        goto end;
    }

    readBuf = (unsigned char*)av_malloc(PLAYER_BUFSIZE);
    if (!readBuf) {
        ret = AVERROR(ENOMEM);
        DBG("alloc buffer fail!");
        goto end;
    }

    //!! readBuf 必须动态分配，在open_input内部释放
    avio_ctx = avio_alloc_context(readBuf, PLAYER_BUFSIZE,
                                  0, NULL, &read_packet, NULL, NULL);
    if (!avio_ctx) {
        ret = AVERROR(ENOMEM);
        DBG("avio_alloc_context fail!");
        goto end;
    }
    fmt_ctx->pb = avio_ctx;

    ret = avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        DBG("Could not open input!");
        goto end;
    }

    //!! 这里需要预读取
    //fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto end;
    }
    DBG("avformat_find_stream_info Done!"); 
    /* sleep(10); */

    av_dump_format(fmt_ctx, 0, "input_video", 0);

    videoStreamIndex = -1;
    for(i = 0; i < fmt_ctx->nb_streams; i++) {
        if(fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        } // if
    } // for 

    if(videoStreamIndex == -1) {
        DBG("Cannot find a valid video stream!");
        goto end;
    }
 
    pCodecCtx = fmt_ctx->streams[videoStreamIndex]->codec;

    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec == NULL) {
        DBG("Unsupported codec!");
        goto end;
    } // if 
    DBG("Decoder name = %s, long_name = %s", pCodec->name, pCodec->long_name);

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, &optionsDict) < 0) {
        DBG("open codec error!");
        goto end;
    }

    // jpg_converter.init( pCodecCtx );

    // Allocate video frame
    pFrame = av_frame_alloc();
    if( !pFrame ) {
        DBG("alloc frame fail!");
        goto end;
    }

    // useless for sync
    /*
     * for( i = 1; i <= 500; ++i ) {
     *     av_read_frame(fmt_ctx, &packet);
     *     printf("skip %d frame\n", i);
     * }
     */

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
        DBG( "Could not initialize SDL - %s", SDL_GetError());
        goto end;
    }

    screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);   //!! original 24
    if(!screen) {
        DBG("SDL: could not set video mode - exiting");
        goto end;
    }

    // Allocate a place to put our YUV image on that screen
    bmp = SDL_CreateYUVOverlay(pCodecCtx->width, pCodecCtx->height,
                    SDL_YV12_OVERLAY, screen);

    sws_ctx = sws_getContext( pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
                    pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, // original 420p
                    NULL, NULL, NULL );    //!! SWS_BILINEAR
    if( !sws_ctx ) {
        DBG("sws_getContext error!");
        goto end;
    }

    while(av_read_frame(fmt_ctx, &packet) >= 0) {
        // Is this a packet from the video stream?
        if(packet.stream_index==videoStreamIndex) {
            DBG_STREAM( "going to decode packet size = " << packet.size
                   << " cksum = " << crc_checksum(packet.data, packet.size)
                   << " at time: " << gen_timestamp() );
            // Decode video frame 解码结果存放在pFrame中，frameFinished表示成功与否
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
            if( frameFinished ) {
                /* frame->pts = av_frame_get_best_effort_timestamp(frame); */ //!!
                // DBG_STREAM( "Decoded frame format = " << pFrame->format << " pict_type = " 
                       // << pFrame->pict_type << " coded_picture_number = " << pFrame->coded_picture_number
                       // << " display_picture_number = " << pFrame->display_picture_number
                       // << " pkt_pos = " << pFrame->pkt_pos
                       // << " pkt_size = " << pFrame->pkt_size
                       // << " pts = " << pFrame->pts
                       // << " pkt_pts = " << pFrame->pkt_pts );

                sprintf( jpgFileName, "%u.jpg", ++decodeSeqNO );
                WriteJPEG( pCodecCtx, pFrame, jpgFileName );
                // jpg_converter.convert( pFrame, jpgFileName );

                SDL_LockYUVOverlay(bmp);

                // 将yuv444转为yuv420以便用SDL显示，转换后的图像存储在pict中，其指针指向内存为bmp
                AVFrame pict;
                pict.data[0] = bmp->pixels[0];
                pict.data[1] = bmp->pixels[2];
                pict.data[2] = bmp->pixels[1];

                pict.linesize[0] = bmp->pitches[0];
                pict.linesize[1] = bmp->pitches[2];
                pict.linesize[2] = bmp->pitches[1];

                // Convert the image into YUV format that SDL uses
                sws_scale( sws_ctx, (uint8_t const * const *)pFrame->data, 
                        pFrame->linesize, 0, pCodecCtx->height, pict.data, pict.linesize );

                SDL_UnlockYUVOverlay(bmp);

                rect.x = 0;
                rect.y = 0;
                rect.w = pCodecCtx->width;
                rect.h = pCodecCtx->height;
                SDL_DisplayYUVOverlay(bmp, &rect);
                DBG_STREAM( "Finish show packet size = " << packet.size
                        << " cksum = " << crc_checksum(packet.data, packet.size)
                        << " at time: " << gen_timestamp() );
            } else {
                DBG_STREAM("frameFinished is not true!");
            } // if frameFinished
        } else {
            DBG_STREAM("NOT A VIDEO PACKET!");
        } // if videoStreamIndex

        // av_free_packet(&packet);
        av_packet_unref(&packet);
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                SDL_Quit();
                /* exit(0); */
                goto end;
                break;
            default:
                break;
        } // switch

    } // while

end:
    if( pFrame )
        av_free(pFrame);
    // Close the codec
    if( pCodecCtx )
        avcodec_close(pCodecCtx);
    // Close the video file
    if( fmt_ctx )
        avformat_close_input(&fmt_ctx);
    /* note: the internal buffer could have changed, and be != avio_ctx_buffer */
    if (avio_ctx)
        av_freep(&avio_ctx);

    if (ret < 0) {
        // fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        fprintf(stderr, "Error occurred\n");
        return 1;
    }

    return 0;
}


static
int WriteJPEG( const AVCodecContext *pCodecCtx, AVFrame *pFrame, const char *filename )
{
    AVCodecContext         *pOCodecCtx;
    AVCodec                *pOCodec;
    // uint8_t                *Buffer;
    // int                     BufSiz;
    // int                     BufSizActual;
    int                     got_output;
    int                     ret = 0;
    AVPixelFormat           ImgFmt = AV_PIX_FMT_YUVJ444P; //!!??!!
    FILE                   *JPEGFile;
    char                    JPEGFName[256];
    AVDictionary           *optionsDict = NULL;
    AVPacket                pkt;

    // BufSiz = avpicture_get_size( ImgFmt, pCodecCtx->width, pCodecCtx->height );
    // BufSiz = av_image_get_buffer_size( ImgFmt, pCodecCtx->width, pCodecCtx->height, 1 );

    // Buffer = (uint8_t *) malloc ( BufSiz );
    // if ( Buffer == NULL )
        // return ( 0 );
    // memset ( Buffer, 0, BufSiz );

    DBG_STREAM("Writing jpg file: " << filename);

    pOCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!pOCodec) {
        fprintf(stderr, "Cannot find MJPEG codec!\n");
        return -1;
    }

    pOCodecCtx = avcodec_alloc_context3(pOCodec);
    if ( !pOCodecCtx ) {
        fprintf(stderr, "Allocate CodecCtx error!\n");
        return -1;
    }

    pOCodecCtx->bit_rate      = pCodecCtx->bit_rate;
    pOCodecCtx->width         = pCodecCtx->width;
    pOCodecCtx->height        = pCodecCtx->height;
    pOCodecCtx->pix_fmt       = ImgFmt;
    pOCodecCtx->time_base.num = pCodecCtx->time_base.num;
    pOCodecCtx->time_base.den = pCodecCtx->time_base.den;
    pOCodecCtx->framerate = pCodecCtx->framerate;

    if ( avcodec_open2(pOCodecCtx, pOCodec, &optionsDict) < 0 ) {
        fprintf(stderr, "open codec context error!\n");
        return -1;
    }

    // pOCodecCtx->mb_lmin        = pOCodecCtx->lmin = pOCodecCtx->qmin * FF_QP2LAMBDA;
    // pOCodecCtx->mb_lmax        = pOCodecCtx->lmax = pOCodecCtx->qmax * FF_QP2LAMBDA;
    pOCodecCtx->flags          = CODEC_FLAG_QSCALE;
    pOCodecCtx->global_quality = pOCodecCtx->qmin * FF_QP2LAMBDA;

    pFrame->pts     = 1;
    pFrame->quality = pOCodecCtx->global_quality;
    // int avcodec_encode_video2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet_ptr);
    av_init_packet(&pkt);
    pkt.data = NULL;    // packet data will be allocated by the encoder
    pkt.size = 0;
    ret = avcodec_encode_video2(pOCodecCtx, &pkt, pFrame, &got_output);
    if (ret < 0) {
        fprintf(stderr, "Error encoding frame\n");
        return -1;
    }
    // BufSizActual = avcodec_encode_video( pOCodecCtx, Buffer, BufSiz, pFrame );
    if (got_output) {
        JPEGFile = fopen ( filename, "wb" );
        if( !JPEGFile ) {
            std::cerr << "Cannot open file " << filename << " for writing!" << std::endl;
            return -1;
        }
        fwrite(pkt.data, 1, pkt.size, JPEGFile);
        av_packet_unref(&pkt);
        fclose ( JPEGFile );
    }

    // fwrite ( Buffer, 1, BufSizActual, JPEGFile );

    avcodec_close ( pOCodecCtx );
    av_free( pOCodecCtx );
    // free ( Buffer );

    return 0;
}

// FOR TEST
/*
 * void DesktopStreamingRequest::PlayerRoutine()
 * {
 *     using namespace std;
 * 
 *     RecvdFrame      frame;
 * 
 *     ofstream ofs( "test.dat", ios::out | ios::binary );
 * 
 *     while( true ) {
 *         frame = frameQueue.pop();
 *         ofs.write( frame.pData->ptr(), frame.pData->size() );
 *         ofs.flush();
 *         gRecvBufMgr.put( frame.pData );
 *     } // while 
 * 
 *     return;
 * }
 */

/*
 * struct JPG_Converter {
 *     JPG_Converter() : pOCodecCtx(NULL), pOCodec(NULL) {}
 * 
 *     ~JPG_Converter()
 *     {
 *         if( pOCodecCtx ) {
 *             avcodec_close ( pOCodecCtx );
 *             av_free( pOCodecCtx );
 *         } // if
 *     }
 * 
 *     int init( AVCodecContext *pCodecCtx )
 *     {
 *         AVDictionary           *optionsDict = NULL;
 * 
 *         pOCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
 *         if (!pOCodec) {
 *             fprintf(stderr, "Cannot find MJPEG codec!\n");
 *             return -1;
 *         }
 * 
 *         pOCodecCtx = avcodec_alloc_context3(pOCodec);
 *         if ( !pOCodecCtx ) {
 *             fprintf(stderr, "Allocate CodecCtx error!\n");
 *             return -1;
 *         }
 * 
 *         pOCodecCtx->bit_rate      = pCodecCtx->bit_rate;
 *         pOCodecCtx->width         = pCodecCtx->width;
 *         pOCodecCtx->height        = pCodecCtx->height;
 *         pOCodecCtx->pix_fmt       = AV_PIX_FMT_YUVJ420P;
 *         pOCodecCtx->time_base.num = pCodecCtx->time_base.num;
 *         pOCodecCtx->time_base.den = pCodecCtx->time_base.den;
 *         pOCodecCtx->framerate = pCodecCtx->framerate;
 * 
 *         if ( avcodec_open2(pOCodecCtx, pOCodec, &optionsDict) < 0 ) {
 *             fprintf(stderr, "open codec context error!\n");
 *             return -1;
 *         } // if
 * 
 *         pOCodecCtx->flags          = CODEC_FLAG_QSCALE;
 *         pOCodecCtx->global_quality = pOCodecCtx->qmin * FF_QP2LAMBDA;
 * 
 *         return 0;
 *     }
 * 
 *     int convert( AVFrame *pFrame, const char *filename )
 *     {
 *         int             ret = 0;
 *         FILE            *fp;
 * 
 *         pFrame->pts     = 1;
 *         pFrame->quality = pOCodecCtx->global_quality;
 *         // int avcodec_encode_video2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet_ptr);
 *         av_init_packet(&pkt);
 *         pkt.data = NULL;    // packet data will be allocated by the encoder
 *         pkt.size = 0;
 *         ret = avcodec_encode_video2(pOCodecCtx, &pkt, pFrame, &got_output);
 *         if (ret < 0) {
 *             fprintf(stderr, "Error encoding frame\n");
 *             return -1;
 *         }
 * 
 *         if( (fp = fopen(filename, "wb")) == NULL ) {
 *             fprintf(stderr, "cannot open file %s for writing!\n", filename);
 *             return -1;
 *         }
 *         // BufSizActual = avcodec_encode_video( pOCodecCtx, Buffer, BufSiz, pFrame );
 *         if (got_output) {
 *             fwrite(pkt.data, 1, pkt.size, fp);
 *             av_packet_unref(&pkt);
 *         } // if
 * 
 *         fclose ( fp );
 *         return 0;
 *     }
 * 
 *     AVCodecContext         *pOCodecCtx;
 *     AVCodec                *pOCodec;
 *     AVPacket                pkt;
 *     int                     got_output;
 * };
 */


