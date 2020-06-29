/**
 * @file
 * DXGI frame device demuxer
 */

#define COBJMACROS

#include "config.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>

// Driver types supported
D3D_DRIVER_TYPE DriverTypes[] =
{
  D3D_DRIVER_TYPE_HARDWARE,
  D3D_DRIVER_TYPE_WARP,
  D3D_DRIVER_TYPE_REFERENCE,
};
UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

// Feature levels supported
D3D_FEATURE_LEVEL FeatureLevels[] =
{
  D3D_FEATURE_LEVEL_11_0,
  D3D_FEATURE_LEVEL_10_1,
  D3D_FEATURE_LEVEL_10_0,
  D3D_FEATURE_LEVEL_9_1
};
UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

struct pointerinfo  {
    BYTE* PtrShapeBuffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO ShapeInfo;
    POINT Position;
    BOOL Visible;
    UINT BufferSize;
    LARGE_INTEGER LastTimeStamp;
};

/**
 * DXGI Device Demuxer context
 */
struct dxgigrab {
    const AVClass *class;   /**< Class for private options */

    int        frame_size;  /**< Size in bytes of the frame pixel data */
    int        header_size; /**< Size in bytes of the DIB header */
    AVRational time_base;   /**< Time base */
    int64_t    time_frame;  /**< Current time */
    int        draw_mouse;  /**< Draw mouse cursor (private option) */
    AVRational framerate;   /**< Capture framerate (private option) */
    int        width;       /**< Width of the grab frame (private option) */
    int        height;      /**< Height of the grab frame (private option) */
    int        offset_x;    /**< Capture x offset (private option) */
    int        offset_y;    /**< Capture y offset (private option) */
    HDC        source_hdc;  /**< Source device context */
    BITMAPINFO bmi;         /**< Information describing DIB format */
    void      *buffer;      /**< The buffer containing the bitmap image data */
    RECT       clip_rect;   /**< The subarea of the screen or window to clip */

    ID3D11Device              *d3d11_device;
    ID3D11DeviceContext       *d3d11_device_ctx;
    IDXGIDevice               *dxgi_device;
    IDXGIAdapter              *dxgi_adapter;
    IDXGIOutput               *dxgi_output;
    IDXGIOutput1              *dxgi_output1;
    IDXGIOutputDuplication    *dxgi_output_duplication;
    ID3D11Texture2D           *src_resource;
    ID3D11Texture2D           *gdi_resource;
    ID3D11Texture2D           *dst_resource;
    ID3D11ShaderResourceView  *gdi_view;
    DXGI_MODE_ROTATION      rotation;
    struct pointerinfo      pointer_info;
    int cursor_error_printed;
    int first_frames;
    int down_sample_factor;
};

#define WIN32_API_ERROR(str)                                            \
    av_log(s1, AV_LOG_ERROR, str " (error %li)\n", GetLastError())

/**
 * Initializes the dxgi grab device demuxer (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return AVERROR_IO error, 0 success
 */
static int
dxgigrab_read_header(AVFormatContext *s1)
{
    struct dxgigrab *dxgigrab = s1->priv_data;

    HDC source_hdc = NULL;
    BITMAPINFO bmi;
    ID3D11Device *d3d11_device = NULL;
    ID3D11DeviceContext *d3d11_device_ctx = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    IDXGIOutput *dxgi_output = NULL;
    IDXGIOutput1 *dxgi_output1 = NULL;
    IDXGIOutputDuplication *dxgi_output_duplication = NULL;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;

    const char *filename = s1->filename;
    const char *name     = NULL;
    AVStream   *st       = NULL;
    int bpp;

    RECT clip_rect;
    RECT virtual_rect;
    int ret;

    HRESULT hr;
    D3D_FEATURE_LEVEL feature_level;	

    if (!strncmp(filename, "title=", 6)) {
       av_log(s1, AV_LOG_ERROR,
               "DXGI don't support window capture, please use GDI format.\n");
        ret = AVERROR(EIO);
        goto error;
    } else if (strcmp(filename, "desktop")){
        av_log(s1, AV_LOG_ERROR,
               "Please use \"desktop\" or \"title=<windowname>\" to specify your target.\n");
        ret = AVERROR(EIO);
        goto error;
    }

    source_hdc = GetDC(NULL);
    bpp = GetDeviceCaps(source_hdc, BITSPIXEL);

    for (UINT index = 0; index < NumDriverTypes; index++)
    {
      hr = D3D11CreateDevice(
        NULL,                       // Adapter
        DriverTypes[index],         // Driver type
        NULL,                       // Software
        0,                          // Flags
        FeatureLevels,              // Feature level
        NumFeatureLevels,           // Size of feature levels array
        D3D11_SDK_VERSION,          // SDK version
        &d3d11_device,              // Pointer to returned interface
        &feature_level,             // Pointer to returned feature level
        &d3d11_device_ctx);

      if (SUCCEEDED(hr))
      {
        break;
      }
    }

    if (d3d11_device == NULL)
    {
      av_log(s1, AV_LOG_ERROR,
             "D3D11 create device failed!.\n");
      ret = AVERROR(EIO);
      goto error;
    }

    // Get DXGI device
    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_IDXGIDevice, &dxgi_device);
    if (FAILED(hr)) {
      av_log(s1, AV_LOG_ERROR,
        "D3D11 device QueryInterface failed %d!\n", HRESULT_CODE(hr));
      ret = AVERROR(EIO);
      goto error;
    }

    // Get DXGI adapter000002754
    hr = IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter, &dxgi_adapter);
    if (FAILED(hr)) {
      av_log(s1, AV_LOG_ERROR,
        "DXGI device GetParent failed %d!\n", HRESULT_CODE(hr));
      ret = AVERROR(EIO);
      goto error;
    }

    if (IDXGIAdapter_EnumOutputs(dxgi_adapter, 0, &dxgi_output) == DXGI_ERROR_NOT_FOUND) {
      av_log(s1, AV_LOG_ERROR,
        "DXGI adapter enumerate outputs failed\n");
      ret = AVERROR(EIO);
      goto error;
    }

    DXGI_OUTPUT_DESC desktop_desc;
    IDXGIOutput_GetDesc(dxgi_output, &desktop_desc);
    rotation = desktop_desc.Rotation;

    // QueryInterface for Output1
    hr = IDXGIOutput_QueryInterface(dxgi_output, &IID_IDXGIOutput1, &dxgi_output1);
    if (FAILED(hr)) {
      av_log(s1, AV_LOG_ERROR,
        "DXGI output query interface failed %d !\n", HRESULT_CODE(hr));
      ret = AVERROR(EIO);
      goto error;
    }

    // Create desktop duplication
    hr = IDXGIOutput1_DuplicateOutput(dxgi_output1, d3d11_device, &dxgi_output_duplication);
    if (FAILED(hr)) {
      av_log(s1, AV_LOG_ERROR,
        "DXGI duplicate output failed %d!\n", HRESULT_CODE(hr));
      ret = AVERROR(EIO);
      goto error;
    }

    st = avformat_new_stream(s1, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    virtual_rect = desktop_desc.DesktopCoordinates;

    /* If no width or height set, use full screen/window area */
    if (!dxgigrab->width || !dxgigrab->height) {
        clip_rect.left = virtual_rect.left;
        clip_rect.top = virtual_rect.top;
        clip_rect.right = virtual_rect.right;
        clip_rect.bottom = virtual_rect.bottom;
        dxgigrab->width = clip_rect.right - clip_rect.left;
        dxgigrab->height = clip_rect.bottom - clip_rect.top;
    } else {
        clip_rect.left = dxgigrab->offset_x;
        clip_rect.top = dxgigrab->offset_y;
        clip_rect.right = dxgigrab->width + dxgigrab->offset_x;
        clip_rect.bottom = dxgigrab->height + dxgigrab->offset_y;
    }

    if (dxgigrab->down_sample_factor > 0) {
        dxgigrab->down_sample_factor = pow(2, dxgigrab->down_sample_factor);
        dxgigrab->width /= dxgigrab->down_sample_factor;
        dxgigrab->height /= dxgigrab->down_sample_factor;
    }
    else {
        dxgigrab->down_sample_factor = 1;
    }

    /* Create a DIB */
    bmi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth         = dxgigrab->width;
    bmi.bmiHeader.biHeight        = -(dxgigrab->height);
    bmi.bmiHeader.biPlanes        = 1;
    bmi.bmiHeader.biBitCount      = bpp;
    bmi.bmiHeader.biCompression   = BI_RGB;
    bmi.bmiHeader.biSizeImage     = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed       = 0;
    bmi.bmiHeader.biClrImportant  = 0;

    dxgigrab->frame_size = dxgigrab->width * dxgigrab->height * (bpp / 8);
    dxgigrab->header_size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) +
                           (bpp <= 8 ? (1 << bpp) : 0) * sizeof(RGBQUAD) /* palette size */;
    dxgigrab->time_base   = av_inv_q(dxgigrab->framerate);
    dxgigrab->time_frame  = av_gettime() / av_q2d(dxgigrab->time_base);

    dxgigrab->d3d11_device = d3d11_device;
    dxgigrab->d3d11_device_ctx = d3d11_device_ctx;
    dxgigrab->dxgi_device = dxgi_device;
    dxgigrab->dxgi_adapter = dxgi_adapter;
    dxgigrab->dxgi_output = dxgi_output;
    dxgigrab->dxgi_output1 = dxgi_output1;
    dxgigrab->dxgi_output_duplication = dxgi_output_duplication;
    dxgigrab->src_resource = NULL;
    dxgigrab->dst_resource = NULL;
    dxgigrab->gdi_view = NULL;
    dxgigrab->rotation = rotation;
    RtlZeroMemory(&dxgigrab->pointer_info, sizeof(struct pointerinfo));

    dxgigrab->bmi = bmi;
    dxgigrab->source_hdc = source_hdc;
    dxgigrab->clip_rect  = clip_rect;

    dxgigrab->cursor_error_printed = 0;
    dxgigrab->first_frames = 5;

    st->avg_frame_rate = av_inv_q(dxgigrab->time_base);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_BMP;
    st->codecpar->bit_rate   = (dxgigrab->header_size + dxgigrab->frame_size) * 1/av_q2d(dxgigrab->time_base) * 8;
    return 0;

error:
    if (source_hdc)
        ReleaseDC(NULL, source_hdc);

    if (d3d11_device_ctx)
        ID3D11DeviceContext_Release(d3d11_device_ctx); 

    if (dxgi_output_duplication)
        IDXGIOutputDuplication_Release(dxgi_output_duplication);

    if (dxgi_output1)
        IDXGIOutput1_Release(dxgi_output1);

    if (dxgi_output)
        IDXGIOutput_Release(dxgi_output);

    if (d3d11_device)
        ID3D11Device_Release(d3d11_device); 

    if (dxgi_device)
        IDXGIDevice_Release(dxgi_device);

    if (dxgi_adapter)
        IDXGIAdapter_Release(dxgi_adapter); 

    if (source_hdc)
        DeleteDC(source_hdc);
}

//
// Retrieves mouse info and write it into ptr_info 
//
int GetMouse(AVFormatContext *s1, DXGI_OUTDUPL_FRAME_INFO* frame_info) {
    struct dxgigrab *dxgigrab = s1->priv_data;
    struct pointerinfo* ptr_info = &dxgigrab->pointer_info;
    int offset_x = dxgigrab->offset_x;
    int offset_y = dxgigrab->offset_y;
    RECT clip_rect = dxgigrab->clip_rect;
    UINT BufferSizeRequired = 0;
    HRESULT hr;
    IDXGIOutputDuplication *dxgi_output_duplication = dxgigrab->dxgi_output_duplication;
    // A non-zero mouse update timestamp indicates that there is a mouse position update and optionally a shape change
    if (frame_info->LastMouseUpdateTime.QuadPart == 0)
    {
        return 0;
    }

    BOOL update_position = TRUE;

    // If two outputs both say they have a visible, only update if new update has newer timestamp
    if (frame_info->PointerPosition.Visible 
        && ptr_info->Visible 
        && (ptr_info->LastTimeStamp.QuadPart > frame_info->LastMouseUpdateTime.QuadPart))
    {
        update_position = FALSE;
    }

    // Update position
    if (update_position)
    {
        ptr_info->Position.x = frame_info->PointerPosition.Position.x - offset_x;
        ptr_info->Position.y = frame_info->PointerPosition.Position.y - offset_y;
        ptr_info->LastTimeStamp = frame_info->LastMouseUpdateTime;
        ptr_info->Visible = frame_info->PointerPosition.Visible != 0;
    }

    // No new shape
    if (frame_info->PointerShapeBufferSize == 0)
    {
        return 0;
    }

    // Old buffer too small
    if (frame_info->PointerShapeBufferSize > ptr_info->BufferSize)
    {
        if (ptr_info->PtrShapeBuffer)
        {
            free(ptr_info->PtrShapeBuffer);
            ptr_info->PtrShapeBuffer = NULL;
        }
        ptr_info->PtrShapeBuffer = (BYTE*) malloc(frame_info->PointerShapeBufferSize*sizeof(BYTE));
        if (!ptr_info->PtrShapeBuffer) {
            ptr_info->BufferSize = 0;
            av_log(s1, AV_LOG_ERROR,
                "Failed to allocate memory for pointer shape in DUPLICATIONMANAGE!\n");
            return AVERROR(ENOMEM);
        }

        // Update buffer size
        ptr_info->BufferSize = frame_info->PointerShapeBufferSize;
    }

    // Get shape
    hr = IDXGIOutputDuplication_GetFramePointerShape(dxgi_output_duplication,
                                                     frame_info->PointerShapeBufferSize, 
                                                     (VOID*)(ptr_info->PtrShapeBuffer), 
                                                     &BufferSizeRequired, 
                                                     &(ptr_info->ShapeInfo));
    if (FAILED(hr)) {
        free(ptr_info->PtrShapeBuffer);
        RtlZeroMemory(ptr_info, sizeof(struct pointerinfo));
        av_log(s1, AV_LOG_ERROR,
            "Failed to get frame pointer shape in DUPLICATIONMANAGER %d!\n", HRESULT_CODE(hr));
        return AVERROR(EIO);
    }

    return 0;
}

void ProcessColor(struct dxgigrab* dxgigrab, RECT pointer_rect,
                 INT skip_x, INT skip_y) {
    struct pointerinfo* ptr_info = &dxgigrab->pointer_info;
    uint8_t* desktop_data = (uint8_t*)dxgigrab->buffer + dxgigrab->header_size;
    int desktop_pitch = dxgigrab->width << 2;
    uint8_t* cursor_data = ptr_info->PtrShapeBuffer;
    int cursor_pitch = ptr_info->ShapeInfo.Pitch;
    int down_sample_factor = dxgigrab->down_sample_factor;
    int width = pointer_rect.right - pointer_rect.left;
    int height = pointer_rect.bottom - pointer_rect.top;

    desktop_data += pointer_rect.top * desktop_pitch + (pointer_rect.left << 2);
    cursor_data += skip_y * cursor_pitch + (skip_x << 2);


    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int pixel_index = (x << 2);
            uint8_t base_alpha = 255 - cursor_data[pixel_index*down_sample_factor + 3];
            if (base_alpha == 255) {
                continue;
            } else if (base_alpha == 0) {
                *(UINT32*)(desktop_data + pixel_index) = *(UINT32*)(cursor_data + pixel_index*down_sample_factor);
            } else {
                double alpha = base_alpha / 255.0;
                desktop_data[pixel_index] =
                        (uint8_t)(desktop_data[pixel_index] * alpha) +
                        cursor_data[pixel_index*down_sample_factor];
                desktop_data[pixel_index + 1] =
                        (uint8_t)(desktop_data[pixel_index + 1] * alpha) +
                        cursor_data[pixel_index*down_sample_factor + 1];
                desktop_data[pixel_index + 2] =
                        (uint8_t)(desktop_data[pixel_index + 2] * alpha) +
                        cursor_data[pixel_index*down_sample_factor + 2];
            }
        }
        cursor_data += cursor_pitch * down_sample_factor;
        desktop_data += desktop_pitch;
    }
}

void ProcessMonoMask(struct dxgigrab* dxgigrab, 
                     BOOL is_mono, RECT pointer_rect,
                     INT skip_x, INT skip_y) {

    struct pointerinfo* ptr_info = &dxgigrab->pointer_info; 
    UINT* desktop_data32 = (UINT*)((uint8_t*)dxgigrab->buffer + dxgigrab->header_size);
    UINT desktop_pitch_in_pixels = dxgigrab->width;
    BYTE* cursor_data = ptr_info->PtrShapeBuffer;
    INT pitch = ptr_info->ShapeInfo.Pitch;
    INT pointer_left = pointer_rect.left;
    INT pointer_top = pointer_rect.top;
    int down_sample_factor = dxgigrab->down_sample_factor;
    INT pointer_width = pointer_rect.right - pointer_left;
    INT pointer_height = pointer_rect.bottom - pointer_top;

    desktop_data32 += pointer_top * desktop_pitch_in_pixels + pointer_left;

    if (is_mono) {
        for (int row = 0; row < pointer_height; ++row) {
            uint8_t mask = 0x80;
            mask = mask >> (skip_x % 8);
            for (int col = 0; col < pointer_width; col++) {
                uint32_t and_mask_index = ((col * down_sample_factor + skip_x) >> 3) + ((row * down_sample_factor + skip_y) * pitch);
                uint8_t and_mask = cursor_data[and_mask_index] & mask;
                uint8_t xor_mask = cursor_data[and_mask_index  + (ptr_info->ShapeInfo.Height >> 1) * pitch] & mask;
                UINT and_mask32 = (and_mask) ? 0xFFFFFFFF : 0xFF000000;
                UINT xor_mask32 = (xor_mask) ? 0x00FFFFFF : 0x00000000;

                // Set new pixel
                UINT index = (row * desktop_pitch_in_pixels) + col;
                desktop_data32[index] = (desktop_data32[index] & and_mask32) ^ xor_mask32;

                // Adjust mask
                if (mask == 0x01) {
                    mask = 0x80;
                } else {
                    mask = mask >> 1;
                }
            }
        }
    } else {
        UINT* cursor_data32 = (uint32_t*)(ptr_info->PtrShapeBuffer);
        for (int row = 0; row < pointer_height; ++row) {
            for (int col = 0; col < pointer_width; ++col) {
                // Set up mask
                uint32_t MaskVal = 0xFF000000 & cursor_data32[(col * down_sample_factor + skip_x) + 
                                                              ((row * down_sample_factor + skip_y) * 
                                                               (pitch / sizeof(UINT)))];
                UINT index = (row * desktop_pitch_in_pixels) + col;
                if (MaskVal) {
                    // Mask was 0xFF
                    desktop_data32[index] = (desktop_data32[index] 
                                                          ^ cursor_data32[(col * down_sample_factor + skip_x) 
                                                          + ((row * down_sample_factor + skip_y) 
                                                          * (pitch / sizeof(UINT)))]) | 0xFF000000;
                } else {
                    // Mask was 0x00
                    desktop_data32[index] = cursor_data32[(col * down_sample_factor + skip_x) + ((row * down_sample_factor + skip_y) * (pitch >> 2))] | 0xFF000000;
                }
            }
        }
    }
}

void DrawCursor(AVFormatContext *s1) {
    struct dxgigrab *dxgigrab = s1->priv_data;
    struct pointerinfo* ptr_info = &dxgigrab->pointer_info; 
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE type = dxgigrab->pointer_info.ShapeInfo.Type;
    RECT cursor_rect;
    BOOL is_mono = (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME);
    int desktop_width = dxgigrab->width;
    int desktop_height = dxgigrab->height;
    int down_sample_factor = dxgigrab->down_sample_factor;
    int32_t given_left = ptr_info->Position.x / down_sample_factor;
    int32_t given_top = ptr_info->Position.y / down_sample_factor;
    int32_t pointer_width = ptr_info->ShapeInfo.Width / down_sample_factor;
    int32_t pointer_height = ptr_info->ShapeInfo.Height / down_sample_factor;
    int32_t skip_x = 0;
    int32_t skip_y = 0;
    int32_t pointer_left = 0;
    int32_t pointer_top = 0;


    // Figure out if any adjustment is needed for out of bound positions
    if (given_left < 0) {
        pointer_width = given_left + pointer_width;
    }
    else if ((given_left + pointer_width) > desktop_width) {
        pointer_width = desktop_width - given_left;
    }

    if (is_mono) {
        pointer_height /= 2;
    }

    if (given_top < 0) {
        pointer_height = given_top + pointer_height;
    }
    else if ((given_top + pointer_height) > desktop_height) {
        pointer_height = desktop_height - given_top;
    }

    cursor_rect.left = (given_left < 0) ? 0 : given_left;
    cursor_rect.top = (given_top < 0) ? 0 : given_top;
    cursor_rect.right = cursor_rect.left + pointer_width;
    cursor_rect.bottom = cursor_rect.top + pointer_height;

    // What to skip (pixel offset)
    skip_x = (given_left < 0) ? (-1 * given_left) : (0);
    skip_y = (given_top < 0) ? (-1 * given_top) : (0);

    if (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ||
        type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        ProcessMonoMask(dxgigrab, is_mono, cursor_rect, skip_x, skip_y);
    } else {
        ProcessColor(dxgigrab, cursor_rect, skip_x, skip_y);
    }
}

/**
 * Grabs a frame from dxgi (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @param pkt Packet holding the grabbed frame
 * @return frame size in bytes
 */
static int dxgigrab_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    struct dxgigrab *dxgigrab = s1->priv_data;
    IDXGIOutputDuplication *dxgi_output_duplication = dxgigrab->dxgi_output_duplication;

    RECT       clip_rect  = dxgigrab->clip_rect;
    AVRational time_base  = dxgigrab->time_base;
    int64_t    time_frame = dxgigrab->time_frame;

    BITMAPFILEHEADER bfh;
    int file_size = dxgigrab->header_size + dxgigrab->frame_size;
    int pixel_size = 4;
    int line_size = 0;
    int offset_x = dxgigrab->offset_x;
    int offset_y = dxgigrab->offset_y;
    int width = dxgigrab->width;
    int height = dxgigrab->height;
    int header_size = dxgigrab->header_size;
    int buffer_offset  = 0;
    int surface_offset = 0;
    uint32_t* buffer = NULL;

    IDXGIResource *desktop_resource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    HRESULT hr;
    ID3D11Texture2D **src_resource = &dxgigrab->src_resource;
    ID3D11Texture2D **gdi_resource = &dxgigrab->gdi_resource;
    ID3D11Texture2D **dst_resource = &dxgigrab->dst_resource;
    ID3D11ShaderResourceView  **gdi_view = &dxgigrab->gdi_view;
    IDXGISurface *dst_image = NULL;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    D3D11_MAPPED_SUBRESOURCE mapped_resource;
    int down_sample_factor = dxgigrab->down_sample_factor;
    if (down_sample_factor != 1) {
        clip_rect.left /= down_sample_factor;
        clip_rect.right /= down_sample_factor;
        clip_rect.bottom /= down_sample_factor;
        clip_rect.top /= down_sample_factor;
        offset_x /= down_sample_factor;
        offset_y /= down_sample_factor;
    }

    int64_t curtime, delay;
    int ret;
 
    /* Calculate the time of the next frame */
    time_frame += INT64_C(1000000);

    /* wait based on the frame rate */
    for (;;) {
        curtime = av_gettime();
        delay = time_frame * av_q2d(time_base) - curtime;
        if (delay <= 0) {
            if (delay < INT64_C(-1000000) * av_q2d(time_base)) {
                time_frame += INT64_C(1000000);
            }
            break;
        }
        if (s1->flags & AVFMT_FLAG_NONBLOCK) {
            return AVERROR(EAGAIN);
        } else {
            av_usleep(delay);
        }
    }

    hr = IDXGIOutputDuplication_AcquireNextFrame(dxgi_output_duplication, 20, &frame_info, &desktop_resource);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return AVERROR(EAGAIN);
        } else {
            return AVERROR(EIO);
        }
    }

    // Update mouse infomations.
    if (dxgigrab->draw_mouse) {
        ret = GetMouse(s1, &frame_info);
        if (ret != 0) {
            goto error;
        }
    }

    // The IDXGIOutputDuplication::AcquireNextFrame() always capture the update
    // of the screen. i.e. , there is no update comparing to the previous state.
    // So we skipping first frame until a fully captured frame.
    if (dxgigrab->first_frames > 0) {
      IDXGIResource_Release(desktop_resource);
      --dxgigrab->first_frames;
      ret = AVERROR(EAGAIN);
      goto error;
    }

    // If still holding old frame, destroy it
    if (*src_resource) {
        ID3D11Texture2D_Release(*src_resource);
        *src_resource = NULL;
    }

    // QueryInterface for IDXGIResource
    hr = IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D, src_resource);
    IDXGIResource_Release(desktop_resource);
    // failed to QI for ID3D11Texture2D from acquired IDXGIResource
    if (FAILED(hr)) {
        av_log(s1, AV_LOG_ERROR,
            "DXGI resource QueryInterface failed %d!\n", HRESULT_CODE(hr));
        ret = AVERROR(EIO);
        goto error;
    }
    
    if (*dst_resource == NULL) {
        D3D11_TEXTURE2D_DESC desc;
        D3D11_TEXTURE2D_DESC d3d11_texture2d_desc;
        ID3D11Texture2D_GetDesc(*src_resource, &desc);

        // Create CPU access texture
        d3d11_texture2d_desc.Width = desc.Width / down_sample_factor;
        d3d11_texture2d_desc.Height = desc.Height / down_sample_factor;
        d3d11_texture2d_desc.MipLevels = 1;
        d3d11_texture2d_desc.ArraySize = 1;
        d3d11_texture2d_desc.Format = desc.Format;
        d3d11_texture2d_desc.SampleDesc.Count = 1;
        d3d11_texture2d_desc.SampleDesc.Quality = 0;
        d3d11_texture2d_desc.Usage = D3D11_USAGE_STAGING;
        d3d11_texture2d_desc.BindFlags = 0;
        d3d11_texture2d_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d3d11_texture2d_desc.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D(d3d11_device, &d3d11_texture2d_desc, NULL, dst_resource);
        if (FAILED(hr)) {
            av_log(s1, AV_LOG_ERROR,
                "Create CPU texture 2D failed %d!\n", HRESULT_CODE(hr));
            ret = AVERROR(EIO);
            goto error;
        }
    }

    if (*gdi_resource == NULL) {
        D3D11_TEXTURE2D_DESC desc;
        D3D11_TEXTURE2D_DESC d3d11_texture2d_desc;
        ID3D11Texture2D_GetDesc(*src_resource, &desc);

        // Create GDI access texture
        d3d11_texture2d_desc.Width = desc.Width;
        d3d11_texture2d_desc.Height = desc.Height;
        d3d11_texture2d_desc.MipLevels = 4;
        d3d11_texture2d_desc.ArraySize = 1;
        d3d11_texture2d_desc.Format = desc.Format;
        d3d11_texture2d_desc.SampleDesc.Count = 1;
        d3d11_texture2d_desc.SampleDesc.Quality = 0;
        d3d11_texture2d_desc.Usage = D3D11_USAGE_DEFAULT;
        d3d11_texture2d_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d3d11_texture2d_desc.CPUAccessFlags = 0;
        d3d11_texture2d_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        hr = ID3D11Device_CreateTexture2D(d3d11_device, &d3d11_texture2d_desc, NULL, gdi_resource);
        if (FAILED(hr)) {
          av_log(s1, AV_LOG_ERROR, 
              "Create GDI texture 2D failed %d!\n", HRESULT_CODE(hr));
          ret = AVERROR(EIO);
          goto error;
        }
        hr = ID3D11Device_CreateShaderResourceView(d3d11_device, *gdi_resource, NULL, gdi_view);
        if (FAILED(hr)) {
          av_log(s1, AV_LOG_ERROR, 
              "Create GDI view failed %d!\n", HRESULT_CODE(hr));
          ret = AVERROR(EIO);
          goto error;
        }
    }

    // Copy the mipmap of src_resource to the staging texture
    ID3D11DeviceContext_CopySubresourceRegion(d3d11_device_ctx, *gdi_resource, 0, 0, 0, 0, *src_resource, 0, NULL);

    // Generates the mipmap of the screen
    ID3D11DeviceContext_GenerateMips(d3d11_device_ctx, *gdi_view);

    // Copy the mipmap of gdi_rsource to the staging texture
    ID3D11DeviceContext_CopySubresourceRegion(d3d11_device_ctx, *dst_resource, 0, 0, 0, 0, *gdi_resource, log2(down_sample_factor), NULL);

    // Get the desktop capture texture
    hr = ID3D11DeviceContext_Map(d3d11_device_ctx, *dst_resource, 0, D3D11_MAP_READ, 0, &mapped_resource);
    if (FAILED(hr)) {
        av_log(s1, AV_LOG_ERROR,
            "D3D11 context map failed %d!", HRESULT_CODE(hr));
        ret = AVERROR(EIO);
        goto error;
    }

    if (av_new_packet(pkt, file_size) < 0) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    dxgigrab->buffer = pkt->data;
    pkt->pts = curtime;
    buffer = (uint32_t*)(pkt->data + header_size);

    switch (dxgigrab->rotation) {
    case DXGI_MODE_ROTATION_IDENTITY:
      line_size = width * pixel_size;
      for (int i = 0; i < height; ++i) {
        memcpy(pkt->data + header_size + i * line_size,
               (BYTE*)mapped_resource.pData + (i + offset_y)* mapped_resource.RowPitch + (offset_x << 2),
               line_size);
      }
      break;
    case DXGI_MODE_ROTATION_ROTATE90:
      /* Rotate the coordinate of pixel.
       *
       *                             * * X(right, 0)
       *  * * * * * *                P * *
       *  * * * * * *       ==>      * * *
       *  * P * * * *                * * *
       *                             * * *
       *                             * * *
       * 
       * The point P(x, y) map to P(right - 1 - y, x)
       */
      for (int i = 0; i < height; ++i) {
        for(int j = 0; j < width; ++j) {
           *buffer++ = *(uint32_t*)((BYTE*)mapped_resource.pData +
                (clip_rect.right - 1 - offset_x - j) * mapped_resource.RowPitch + ((i + offset_y) << 2));
        }
      }
      break;
    case DXGI_MODE_ROTATION_ROTATE180:
      /* Rotate the coordinate of pixel.
       *                             
       *  * * * * * *               * P * * * *
       *  * * * * * *       ==>     * * * * * * 
       *  * * * * P *               * * * * * X(right, bottom) 
       *                             
       *                             
       * 
       * The point P(x, y) map to P(right - 1 - x, bottom - 1 - y)
       */
      for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
          *buffer++ = *(uint32_t*)((BYTE*)mapped_resource.pData + 
              (clip_rect.bottom - 1 - offset_y - i) * mapped_resource.RowPitch +
              ((clip_rect.right - 1 - offset_x - j) << 2));
        }
      }
      break;
    case DXGI_MODE_ROTATION_ROTATE270:
      /* Rotate the coordinate of pixel.
       *
       *                             * * *
       *  * * * * P *                P * *
       *  * * * * * *       ==>      * * *
       *  * * * * * *                * * *
       *                             * * *
       *                 (0, bottom) X * *
       * 
       * The point P(x, y) map to P(y, bottom - 1 - x)
       */
      for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
          *buffer++ = *(uint32_t*)((BYTE*)mapped_resource.pData +
              (j + offset_x) * mapped_resource.RowPitch +
              ((clip_rect.bottom - 1 - offset_y - i) << 2));
        }
      }
      break;
    };

    if (dxgigrab->draw_mouse && dxgigrab->pointer_info.Visible) {
        DrawCursor(s1);
    }

    /* Copy bits to packet data */
    bfh.bfType = 0x4d42; /* "BM" in little-endian */
    bfh.bfSize = file_size;
    bfh.bfReserved1 = 0;
    bfh.bfReserved2 = 0;
    bfh.bfOffBits = dxgigrab->header_size;

    memcpy(pkt->data, &bfh, sizeof(bfh));
    memcpy(pkt->data + sizeof(bfh), &dxgigrab->bmi.bmiHeader, sizeof(dxgigrab->bmi.bmiHeader));

    dxgigrab->time_frame = time_frame;
    ret = dxgigrab->frame_size;

error:
    if (dst_image) {
        IDXGISurface_Unmap(dst_image);
        IDXGISurface_Release(dst_image);
    }
    IDXGIOutputDuplication_ReleaseFrame(dxgi_output_duplication);
    return ret;
}

/**
 * Closes dxgi frame grabber (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return 0 success, !0 failure
 */
static int dxgigrab_read_close(AVFormatContext *s1)
{
    struct dxgigrab *s = s1->priv_data;
    int ref = 0;
    if (s->source_hdc) {
        ReleaseDC(NULL, s->source_hdc);
    }
    if (s->d3d11_device_ctx) {
      ref = ID3D11DeviceContext_Release(s->d3d11_device_ctx); 
      av_log(s1, AV_LOG_ERROR, "d3d11_device_ctx %d\n", ref);
    }
    if (s->dst_resource) {
      ref = IDXGIResource_Release(s->dst_resource);
      av_log(s1, AV_LOG_ERROR, "dst_resource %d\n", ref);
    }
    if (s->gdi_view) {
      ref = ID3D11ShaderResourceView_Release(s->gdi_view);
      av_log(s1, AV_LOG_ERROR, "dst_resource %d\n", ref);
    }
    if (s->gdi_resource) {
      ref = IDXGIResource_Release(s->gdi_resource);
      av_log(s1, AV_LOG_ERROR, "gdi_resource %d\n", ref);
    }
    if (s->dxgi_output_duplication) { 
      ref = IDXGIOutputDuplication_Release(s->dxgi_output_duplication);
      av_log(s1, AV_LOG_ERROR, "dxgi_output_duplication %d\n", ref);
    }
    if (s->src_resource) {
      ref = IDXGIResource_Release(s->src_resource);
      av_log(s1, AV_LOG_ERROR, "src_resource %d\n", ref);
    }
    if (s->dxgi_output1) { 
      int ref = IDXGIOutput1_Release(s->dxgi_output1);
      av_log(s1, AV_LOG_ERROR, "dxgi_output1 %d\n", ref);
    }
    if (s->dxgi_output) { 
      ref = IDXGIOutput_Release(s->dxgi_output);
      av_log(s1, AV_LOG_ERROR, "dxgi_output %d\n", ref);
    }
    if (s->d3d11_device) {
      ref = ID3D11Device_Release(s->d3d11_device); 
      av_log(s1, AV_LOG_ERROR, "d3d11_device %d\n", ref);
    }
    if (s->dxgi_device) { 
      ref = IDXGIDevice_Release(s->dxgi_device);
      av_log(s1, AV_LOG_ERROR, "dxgi_device %d\n", ref);
    }
    if (s->dxgi_adapter) { 
      ref = IDXGIAdapter_Release(s->dxgi_adapter); 
      av_log(s1, AV_LOG_ERROR, "dxgi_adapter %d\n", ref);
    }
    if (s->source_hdc)
        DeleteDC(s->source_hdc);

    if (s->pointer_info.PtrShapeBuffer) {
        free(s->pointer_info.PtrShapeBuffer);
        s->pointer_info.PtrShapeBuffer = NULL;
    }
    return 0;
}

#define OFFSET(x) offsetof(struct dxgigrab, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "draw_mouse", "draw the mouse pointer", OFFSET(draw_mouse), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "ntsc"}, 0, INT_MAX, DEC },
    { "video_size", "set video frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "offset_y", "capture area y offset", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "down_sample_factor", "Use down sample with specify factor", OFFSET(down_sample_factor), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { NULL },
};

static const AVClass dxgigrab_class = {
    .class_name = "DXGIgrab indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/** gdi grabber device demuxer declaration */
AVInputFormat ff_dxgigrab_demuxer = {
    .name           = "dxgigrab",
    .long_name      = NULL_IF_CONFIG_SMALL("DXGI API Windows frame grabber"),
    .priv_data_size = sizeof(struct dxgigrab),
    .read_header    = dxgigrab_read_header,
    .read_packet    = dxgigrab_read_packet,
    .read_close     = dxgigrab_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &dxgigrab_class,
};

