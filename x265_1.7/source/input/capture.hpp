#ifndef _CAPTURE_HPP
#define _CAPTURE_HPP


#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <cassert>
#include <windows.h>

static
void errhandler(const char *msg)
{
    fprintf(stderr, "ERROR! %s\n", msg);
}

static
void BGRA32ToYUV444(unsigned char * pRgb, unsigned int uRgbStride,
    unsigned char * pY, unsigned char * pU, unsigned char * pV, unsigned int uYuvStride,
    unsigned int uWidth, unsigned int uHeight)
{
    int iR, iG, iB;
    int iRgbOffset = uRgbStride - uWidth * 4;
    int iYuvOffset = uYuvStride - uWidth;

    for (unsigned int i = 0; i < uHeight; i++)
    {
        for (unsigned int j = 0; j < uWidth; j++)
        {
            iB = *pRgb++;
            iG = *pRgb++;
            iR = *pRgb++;
            pRgb++;

            *pY++ = (unsigned char)((19595 * iR + 38470 * iG + 7471 * iB + 32768) >> 16);
            *pU++ = (unsigned char)((-11056 * iR - 21712 * iG + 32768 * iB + 8388608) >> 16);
            *pV++ = (unsigned char)((32768 * iR - 27440 * iG - 5328 * iB + 8388608) >> 16);
        }
        pRgb += iRgbOffset;
        pY += iYuvOffset;
        pU += iYuvOffset;
        pV += iYuvOffset;
    } // for

    return;
}


class YuvFrame {
public:
    YuvFrame(int _Width, int _Height, int _ColorFormat) : width(_Width), height(_Height), colorFormat(_ColorFormat)
    {
        if (colorFormat == X265_CSP_I444)
            totalSize = width * height * 3;
        else if (colorFormat == X265_CSP_I420)
            totalSize = width * height * 3 / 2;

        buffer.resize(totalSize);
        m_pBuf = &buffer[0];

        printf("totalSize = %u\n", totalSize);
    }

    unsigned char* RGBToYUVConversion(const unsigned char *pRGB, size_t nBytes);
    uint32_t size() const { return totalSize; }
protected:
    int width, height, colorFormat;
    uint32_t                    totalSize;
    std::vector<unsigned char>       buffer;
    unsigned char*              m_pBuf;
};

// template <size_t SIZE>
// struct Block {
    // char buf[SIZE];
// };

// 1920*4 bytes in each line, 4 bytes per pixel for 32bit bmp
unsigned char* YuvFrame::RGBToYUVConversion(const unsigned char *pRGB, size_t nBytes)
{
    static std::vector<unsigned char>       reversedRGB;
    unsigned int uRGBStride = width * 4;
    unsigned int uYUVStride = width;
    unsigned char *pY = m_pBuf;
    unsigned char *pU = m_pBuf + width * height;
    unsigned char *pV = pU + width * height;

    assert( nBytes % uRGBStride == 0 );

    // reverse
    reversedRGB.resize(nBytes);
    unsigned char *pReversed = &reversedRGB[0];
    int iStart = nBytes;
    do {
        iStart -= uRGBStride;
        memcpy(pReversed, pRGB+iStart, uRGBStride);
        pReversed += uRGBStride;
    } while( iStart > 0 );

    pRGB = &reversedRGB[0];

    // reverse
    // Block<1920*4> *pblk = (Block<1920*4>*)pRGB;
    // reverse( pblk, pblk + height );

    switch (colorFormat)
    {
    case X265_CSP_I444:
        BGRA32ToYUV444((unsigned char*)pRGB, uRGBStride, pY, pU, pV, uYUVStride, width, height);
        return m_pBuf;
    case X265_CSP_I420:
        return m_pBuf;
    default:
        break;
    }

    return m_pBuf;
}


static
PBITMAPINFO CreateBitmapInfoStruct(HBITMAP hBmp)
{
    BITMAP bmp;
    PBITMAPINFO pbmi;
    WORD    cClrBits;

    // Retrieve the bitmap color format, width, and height.
    if (!GetObject(hBmp, sizeof(BITMAP), (LPSTR)&bmp))
        errhandler("GetObject");

    // Convert the color format to a count of bits.
    cClrBits = (WORD)(bmp.bmPlanes * bmp.bmBitsPixel);
    if (cClrBits == 1)
        cClrBits = 1;
    else if (cClrBits <= 4)
        cClrBits = 4;
    else if (cClrBits <= 8)
        cClrBits = 8;
    else if (cClrBits <= 16)
        cClrBits = 16;
    else if (cClrBits <= 24)
        cClrBits = 24;
    else cClrBits = 32;

    // Allocate memory for the BITMAPINFO structure. (This structure
    // contains a BITMAPINFOHEADER structure and an array of RGBQUAD
    // data structures.)

    if (cClrBits < 24)
        pbmi = (PBITMAPINFO)LocalAlloc(LPTR,
        sizeof(BITMAPINFOHEADER)+
        sizeof(RGBQUAD)* (1 << cClrBits));

    // There is no RGBQUAD array for these formats: 24-bit-per-pixel or 32-bit-per-pixel

    else
        pbmi = (PBITMAPINFO)LocalAlloc(LPTR,
        sizeof(BITMAPINFOHEADER));

    // Initialize the fields in the BITMAPINFO structure.

    pbmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pbmi->bmiHeader.biWidth = bmp.bmWidth;
    pbmi->bmiHeader.biHeight = bmp.bmHeight;
    pbmi->bmiHeader.biPlanes = bmp.bmPlanes;
    pbmi->bmiHeader.biBitCount = bmp.bmBitsPixel;
    if (cClrBits < 24)
        pbmi->bmiHeader.biClrUsed = (1 << cClrBits);

    // If the bitmap is not compressed, set the BI_RGB flag.
    pbmi->bmiHeader.biCompression = BI_RGB;

    // Compute the number of bytes in the array of color
    // indices and store the result in biSizeImage.
    // The width must be DWORD aligned unless the bitmap is RLE
    // compressed.
    pbmi->bmiHeader.biSizeImage = ((pbmi->bmiHeader.biWidth * cClrBits + 31) & ~31) / 8
        * pbmi->bmiHeader.biHeight;
    // Set biClrImportant to 0, indicating that all of the
    // device colors are important.
    pbmi->bmiHeader.biClrImportant = 0;

    return pbmi;
}


static
void CreateBMPFile(LPCTSTR pszFile, PBITMAPINFO pbi,
    HBITMAP hBMP, HDC hDC)
{
    HANDLE hf;                 // file handle
    BITMAPFILEHEADER hdr;       // bitmap file-header
    PBITMAPINFOHEADER pbih;     // bitmap info-header
    LPBYTE lpBits;              // memory pointer
    DWORD dwTotal;              // total count of bytes
    DWORD cb;                   // incremental count of bytes
    BYTE *hp;                   // byte pointer
    DWORD dwTmp;

    pbih = (PBITMAPINFOHEADER)pbi;
    lpBits = (LPBYTE)GlobalAlloc(GMEM_FIXED, pbih->biSizeImage);

    if (!lpBits)
        errhandler("GlobalAlloc");

    // Retrieve the color table (RGBQUAD array) and the bits
    // (array of palette indices) from the DIB.
    if (!GetDIBits(hDC, hBMP, 0, (WORD)pbih->biHeight, lpBits, pbi, DIB_RGB_COLORS))
        errhandler("GetDIBits");

    // Create the .BMP file.
    hf = CreateFile(pszFile,
        GENERIC_READ | GENERIC_WRITE,
        (DWORD)0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        (HANDLE)NULL);
    if (hf == INVALID_HANDLE_VALUE)
        errhandler("CreateFile");
    hdr.bfType = 0x4d42;        // 0x42 = "B" 0x4d = "M"
    // Compute the size of the entire file.
    hdr.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER)+
        pbih->biSize + pbih->biClrUsed
        * sizeof(RGBQUAD)+pbih->biSizeImage);
    hdr.bfReserved1 = 0;
    hdr.bfReserved2 = 0;

    // Compute the offset to the array of color indices.
    hdr.bfOffBits = (DWORD) sizeof(BITMAPFILEHEADER)+
        pbih->biSize + pbih->biClrUsed
        * sizeof (RGBQUAD);

    // Copy the BITMAPFILEHEADER into the .BMP file.
    if (!WriteFile(hf, (LPVOID)&hdr, sizeof(BITMAPFILEHEADER),
        (LPDWORD)&dwTmp, NULL))
    {
        errhandler("WriteFile");
    }

    // Copy the BITMAPINFOHEADER and RGBQUAD array into the file.
    if (!WriteFile(hf, (LPVOID)pbih, sizeof(BITMAPINFOHEADER)
        +pbih->biClrUsed * sizeof (RGBQUAD),
        (LPDWORD)&dwTmp, (NULL)))
        errhandler("WriteFile");

    // Copy the array of color indices into the .BMP file.
    dwTotal = cb = pbih->biSizeImage;
    hp = lpBits;
    if (!WriteFile(hf, (LPSTR)hp, (int)cb, (LPDWORD)&dwTmp, NULL))
        errhandler("WriteFile");

    // Close the .BMP file.
    if (!CloseHandle(hf))
        errhandler("CloseHandle");

    // Free memory.
    GlobalFree((HGLOBAL)lpBits);
}


static
char* AppendToYUV(size_t &len, PBITMAPINFO pbi,
    HBITMAP hBMP, HDC hDC)
{
    static YuvFrame frame(1920, 1080, X265_CSP_I444); // TODO should configurable
    static std::vector<BYTE> byteBuf;
    PBITMAPINFOHEADER pbih;     // bitmap info-header
    LPBYTE lpBits;              // memory pointer
    DWORD dwTotal;              // total count of bytes
    DWORD cb;                   // incremental count of bytes
    BYTE *hp;                   // byte pointer

    pbih = (PBITMAPINFOHEADER)pbi;
    byteBuf.resize(pbih->biSizeImage);
    lpBits = &byteBuf[0];

    // Retrieve the color table (RGBQUAD array) and the bits
    // (array of palette indices) from the DIB.
    if (!GetDIBits(hDC, hBMP, 0, (WORD)pbih->biHeight, lpBits, pbi,
                        DIB_RGB_COLORS))
        return NULL;

    // Copy the array of color indices into the .BMP file.
    dwTotal = cb = pbih->biSizeImage;
    // printf("dwTotal = %lu\n", (unsigned long)dwTotal);           8294400
    hp = lpBits;
    unsigned char *pYuvFrame = frame.RGBToYUVConversion( (unsigned char*)hp, (size_t)dwTotal );
    // os.write((char*)pYuvFrame, frame.size());
    len = frame.size();
    return (char*)pYuvFrame;
}



// void CaptureScreen(const char *filename)
char* CaptureScreenToYuv( size_t &len )
{
    int nScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    int nScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    HWND hDesktopWnd = GetDesktopWindow();
    HDC hDesktopDC = GetDC(hDesktopWnd);
    HDC hCaptureDC = CreateCompatibleDC(hDesktopDC);
    HBITMAP hCaptureBitmap = CreateCompatibleBitmap(hDesktopDC,
        nScreenWidth, nScreenHeight);
    SelectObject(hCaptureDC, hCaptureBitmap);
    BitBlt(hCaptureDC, 0, 0, nScreenWidth, nScreenHeight,
        hDesktopDC, 0, 0, SRCCOPY | CAPTUREBLT);

    // Draw cursor
    CURSORINFO cursor = { sizeof(cursor) };
    ::GetCursorInfo(&cursor);
    if (cursor.flags == CURSOR_SHOWING) {
        RECT rcWnd;
        ::GetWindowRect(hDesktopWnd, &rcWnd);
        ICONINFOEXW info = { sizeof(info) };
        ::GetIconInfoExW(cursor.hCursor, &info);
        const int x = cursor.ptScreenPos.x - rcWnd.left - info.xHotspot;
        const int y = cursor.ptScreenPos.y - rcWnd.top - info.yHotspot;
        BITMAP bmpCursor = { 0 };
        ::GetObject(info.hbmColor, sizeof(bmpCursor), &bmpCursor);
        ::DrawIconEx(hCaptureDC, x, y, cursor.hCursor, bmpCursor.bmWidth, bmpCursor.bmHeight,
            0, NULL, DI_NORMAL);
    } // if

    PBITMAPINFO bmpInfo = CreateBitmapInfoStruct(hCaptureBitmap);
    // CreateBMPFile(filename, bmpInfo, hCaptureBitmap, hDesktopDC);
    char *pFrame = AppendToYUV(len, bmpInfo, hCaptureBitmap, hDesktopDC);

    ReleaseDC(hDesktopWnd, hDesktopDC);
    DeleteDC(hCaptureDC);
    DeleteObject(hCaptureBitmap);

    return pFrame;
}


#endif

/*
 * int main()
 * {
 *      // char buf[64];
 *      // for (int i = 1; i <= 200; ++i) {
 *          // sprintf(buf, "%03d.bmp", i);
 *          // printf("Capturing %d\n", i);
 *          // CaptureScreen(buf);
 *          // ::Sleep(50);
 *      // }
 * 
 *     ofstream yuv("test.yuv", ios::out | ios::binary);
 *     size_t len = 0;
 *     char *pFrame = NULL;
 *     for (int i = 1; i <= 200; ++i) {
 *        printf("Capturing %d\n", i);
 *        pFrame = CaptureScreenToYuv(len);
 *        yuv.write(pFrame, len);
 *        ::Sleep(50);
 *     }
 * 
 *     return 0;
 * }
 */



/*
 * class ScreenCapture {
 * public:
 *     ScreenCapture();
 *     void shot();
 *     virtual ~ScreenCapture();
 * protected:
 *     int         nScreenWidth, nScreenHeight;
 *     HWND        hDesktopWnd;
 *     HDC         hDesktopDC;
 *     HDC         hCaptureDC;
 *     HBITMAP     hCaptureBitmap;
 * };
 * 
 * 
 * void ScreenCapture::shot()
 * {
 *     if( hCaptureBitmap )
 *         DeleteObject(hCaptureBitmap);
 * 
 *     hCaptureBitmap = CreateCompatibleBitmap(hDesktopDC, nScreenWidth, nScreenHeight);
 *     SelectObject(hCaptureDC, hCaptureBitmap);
 *     BitBlt(hCaptureDC, 0, 0, nScreenWidth, nScreenHeight,
 *                 hDesktopDC, 0, 0, SRCCOPY | CAPTUREBLT);
 * 
 *     // Draw cursor
 *     CURSORINFO cursor = { sizeof(cursor) };
 *     ::GetCursorInfo(&cursor);
 *     if (cursor.flags == CURSOR_SHOWING) {
 *         RECT rcWnd;
 *         ::GetWindowRect(hDesktopWnd, &rcWnd);
 *         ICONINFOEXW info = { sizeof(info) };
 *         ::GetIconInfoExW(cursor.hCursor, &info);
 *         const int x = cursor.ptScreenPos.x - rcWnd.left - info.xHotspot;
 *         const int y = cursor.ptScreenPos.y - rcWnd.top - info.yHotspot;
 *         BITMAP bmpCursor = { 0 };
 *         ::GetObject(info.hbmColor, sizeof(bmpCursor), &bmpCursor);
 *         ::DrawIconEx(hCaptureDC, x, y, cursor.hCursor, bmpCursor.bmWidth, bmpCursor.bmHeight,
 *                         0, NULL, DI_NORMAL);
 *     } // if
 * }
 * 
 * 
 * ScreenCapture::ScreenCapture() : hCaptureBitmap(0)
 * {
 *     nScreenWidth = GetSystemMetrics(SM_CXSCREEN);
 *     nScreenHeight = GetSystemMetrics(SM_CYSCREEN);
 *     hDesktopWnd = GetDesktopWindow();
 *     hDesktopDC = GetDC(hDesktopWnd);
 *     hCaptureDC = CreateCompatibleDC(hDesktopDC);
 * }
 * 
 * 
 * ScreenCapture::~ScreenCapture()
 * {
 *     ReleaseDC(hDesktopWnd, hDesktopDC);
 *     DeleteDC(hCaptureDC);
 *     if( hCaptureBitmap )
 *         DeleteObject(hCaptureBitmap);
 * }
 */

