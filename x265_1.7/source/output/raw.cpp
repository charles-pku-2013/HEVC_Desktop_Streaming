/*****************************************************************************
 * Copyright (C) 2013-2015 x265 project
 *
 * Authors: Steve Borho <steve@borho.org>
 *          Xinyue Lu <i@7086.in>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at license @ x265.com.
 *****************************************************************************/

#include "raw.h"
#include <vector>
#include <chrono>
#include "network/desktop_streaming_service.hpp"

using namespace x265;
using namespace std;

// #define MAX_FRAME_SIZE      (1024*1024)

/*
 * struct ByteArray : std::vector<char> {
 *     ByteArray() {}
 *     ByteArray( size_type n ) { reserve(n); }
 * 
 *     char* ptr() { return &(*this)[0]; }
 * 
 *     void append( const char *src, size_type n )
 *     {
 *         size_type cursize = this->size();
 *         this->resize( cursize + n );
 *         char *dst = ptr() + cursize;
 *         memcpy( dst, src, n );
 *     }
 * };
 */

static uint32_t seqno = 0;

RAWOutput::RAWOutput(const char* fname, InputFileInfo&)
{
    b_fail = false;
    // if (!strcmp(fname, "-"))
    // {
        // ofs = &cout;
        // return;
    // }
    // ofs = new ofstream(fname, ios::binary | ios::out);
    // if (ofs->fail())
        // b_fail = true;
}

void RAWOutput::setParam(x265_param* param)
{
    param->bAnnexB = true;
}

int RAWOutput::writeHeaders(const x265_nal* nal, uint32_t nalcount)
{
    seqno = 0;
    uint32_t bytes = 0;
    BytesArrayPtr pBuf = gServerBufMgr.get();

    pBuf->resize( ENCODED_FRAME_HEADER_LEN );
    for (uint32_t i = 0; i < nalcount; i++)
    {
        pBuf->append((const char*)nal->payload, nal->sizeBytes);
        bytes += nal->sizeBytes;
        nal++;
    }

    DBG_STREAM("Generated encode header size = " << bytes << " at " << gen_timestamp());

    DesktopStreamingService::instance()->SendEncodedFrame( 0, pBuf );

    return bytes;
}

int RAWOutput::writeFrame(const x265_nal* nal, uint32_t nalcount, x265_picture&)
{
    uint32_t bytes = 0;
    BytesArrayPtr pBuf = gServerBufMgr.get();

    pBuf->resize( ENCODED_FRAME_HEADER_LEN );
    for (uint32_t i = 0; i < nalcount; i++)
    {
        pBuf->append((const char*)nal->payload, nal->sizeBytes);
        bytes += nal->sizeBytes;
        nal++;
    }

    ++seqno;
    DBG_STREAM("Encoding " << seqno << " frame size = " << bytes );

    DesktopStreamingService::instance()->SendEncodedFrame( seqno, pBuf );

    return bytes;
}

void RAWOutput::closeFile(int64_t, int64_t)
{
    //if (ofs != &cout)
    //    delete ofs;
}
