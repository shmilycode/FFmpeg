/**
 * @file
 * DXGI frame device demuxer
 */

#define COBJMACROS

#include "config.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavdevice/vertex_shader.h"
#include "libavdevice/pixel_shader_chrominance.h"
#include "libavdevice/pixel_shader_luminance.h"
#include "libavdevice/pixel_shader_sample.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>

#define NUMVERTICES 6
#define BPP 4
struct xmfloat3 
{
  float x;
  float y;
  float z;
};

struct xmfloat2 
{
  float x;
  float y;
};

typedef struct _VERTEX
{
  struct xmfloat3 Pos;
  struct xmfloat2 TexCoord;
} VERTEX;

//
// Holds info about the pointer/cursor
//
typedef struct _PTR_INFO
{
    BYTE* PtrShapeBuffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO ShapeInfo;
    POINT Position;
    BOOL Visible;
    UINT BufferSize;
    UINT WhoUpdatedPositionLast;
    LARGE_INTEGER LastTimeStamp;
} PTR_INFO;

typedef struct _FRAME_DATA
{
    ID3D11Texture2D* Frame;
    DXGI_OUTDUPL_FRAME_INFO FrameInfo;
    BYTE* MetaData;
    UINT DirtyCount;
    UINT MoveCount;
} FRAME_DATA;

// Position will be changed based on mouse position
// One rectangle combine with two triangle.
struct xmfloat3 VF00 = {-1.0f, -1.0f, 0};
struct xmfloat2 VF01 = {0.0f, 1.0f};
struct xmfloat3 VF10 = {-1.0f, 1.0f, 0};
struct xmfloat2 VF11 = {0.0f, 0.0f};
struct xmfloat3 VF20 = {1.0f, -1.0f, 0};
struct xmfloat2 VF21 = {1.0f, 1.0f};
struct xmfloat3 VF30 = {1.0f, -1.0f, 0};
struct xmfloat2 VF31 = {1.0f, 1.0f};
struct xmfloat3 VF40 = {-1.0f, 1.0f, 0};
struct xmfloat2 VF41 = {0.0f, 0.0f};
struct xmfloat3 VF50 = {1.0f, 1.0f, 0};
struct xmfloat2 VF51 = {1.0f, 0.0f};

/**
 * DXGI Device Demuxer context
 */
struct dxgigrab {
    const AVClass *class;   /**< Class for private options */

    int        frame_size;  /**< Size in bytes of the frame pixel data */
    AVRational time_base;   /**< Time base */
    int64_t    time_frame;  /**< Current time */
    int        draw_mouse;  /**< Draw mouse cursor (private option) */
    AVRational framerate;   /**< Capture framerate (private option) */
    int        width;       /**< Width of the grab frame (private option) */
    int        height;      /**< Height of the grab frame (private option) */
    int        scaled_width;
    int        scaled_height;
    int        offset_x;    /**< Capture x offset (private option) */
    int        offset_y;    /**< Capture y offset (private option) */
    RECT       clip_rect;   /**< The subarea of the screen or window to clip */

    ID3D11Device              *d3d11_device;
    ID3D11DeviceContext       *d3d11_device_ctx;
    ID3D11Texture2D           *scale_src_surf;
    ID3D11Texture2D           *luminance_surf;
    ID3D11Texture2D           *chrominance_surf;
    ID3D11Texture2D           *cpu_accessible_luminance_surf;
    ID3D11Texture2D           *cpu_accessible_chrominance_surf;
    D3D11_VIEWPORT            vp_luminance;
    D3D11_VIEWPORT            vp_chrominance;
    ID3D11RenderTargetView    *luminance_RTV;
    ID3D11RenderTargetView    *chrominance_RTV;

    //duplication manager
    IDXGIOutputDuplication    *desktop_dupl;
    ID3D11Texture2D* acquired_desktop_image;
    DXGI_OUTPUT_DESC output_desc;


    // bgra to buffer process
    ID3D11PixelShader         *pixel_shader_luminance;
    ID3D11PixelShader         *pixel_shader_chrominance;

    // output manager
    ID3D11RenderTargetView* shared_RTV;
    ID3D11SamplerState* sampler_linear;
    ID3D11BlendState* blend_state;
    ID3D11VertexShader* vertex_shader;
    ID3D11PixelShader* sampler_pixel_shader;
    ID3D11InputLayout* input_layout;
    ID3D11Texture2D* shared_surf;

    // thread data
    PTR_INFO pointer_info;

    int down_sample_factor;
};

//
// Recreate shared texture
//
int CreateSharedSurf(ID3D11Device *d3d11_device, RECT* desk_bounds, ID3D11Texture2D **surface) 
{
    ID3D11Texture2D **shared_surf = surface;
    IDXGIDevice* dxgi_device = NULL;
    IDXGIAdapter* dxgi_adapter = NULL;
    IDXGIOutput* dxgi_output = NULL;
    DXGI_OUTPUT_DESC desktop_desc;
    D3D11_TEXTURE2D_DESC desk_tex_desc;
    HRESULT hr;

    // Get DXGI resources
    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_IDXGIDevice, &dxgi_device);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    hr = IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter, &dxgi_adapter);
    IDXGIDevice_Release(dxgi_device);
    dxgi_device = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Set initial values so that we always catch the right coordinates
    desk_bounds->left = INT_MAX;
    desk_bounds->right = INT_MIN;
    desk_bounds->top = INT_MAX;
    desk_bounds->bottom = INT_MIN;

    hr = IDXGIAdapter_EnumOutputs(dxgi_adapter, 0, &dxgi_output);
    if (FAILED(hr)) {
        IDXGIAdapter_Release(dxgi_adapter);
        dxgi_adapter = NULL;
        return AVERROR(EIO);
    }

    IDXGIOutput_GetDesc(dxgi_output, &desktop_desc);
    *desk_bounds = desktop_desc.DesktopCoordinates;

    IDXGIOutput_Release(dxgi_output);
    dxgi_output = NULL;

    IDXGIAdapter_Release(dxgi_adapter);
    dxgi_adapter = NULL;

    // Create shared texture for all duplication threads to draw into
    RtlZeroMemory(&desk_tex_desc, sizeof(D3D11_TEXTURE2D_DESC));
    desk_tex_desc.Width = desk_bounds->right - desk_bounds->left;
    desk_tex_desc.Height = desk_bounds->bottom - desk_bounds->top;
    desk_tex_desc.MipLevels = 1;
    desk_tex_desc.ArraySize = 1;
    desk_tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desk_tex_desc.SampleDesc.Count = 1;
    desk_tex_desc.Usage = D3D11_USAGE_DEFAULT;
    desk_tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desk_tex_desc.CPUAccessFlags = 0;
    desk_tex_desc.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, shared_surf);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    return 0;
}

int MakeRTV(ID3D11Device *d3d11_device, ID3D11RenderTargetView** RTV, ID3D11Texture2D* surface)
{
  HRESULT hr;
  if(*RTV) {
    ID3D11RenderTargetView_Release(*RTV);
    *RTV = NULL;
  }
  // Create a render target view
  hr = ID3D11Device_CreateRenderTargetView(d3d11_device, surface, NULL, RTV);

  if (FAILED(hr)) {
    return AVERROR(EIO);
  }

  return 0;
}

int InitShaders(struct dxgigrab* dxgigrab)
{
    HRESULT hr;
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    UINT Size = ARRAYSIZE(g_VS);
    ID3D11VertexShader *vertex_shader = NULL;
    ID3D11PixelShader *sample_pixel_shader = NULL;
    ID3D11PixelShader *pixel_shader_luminance;
    ID3D11PixelShader *pixel_shader_chrominance;
    ID3D11InputLayout *input_layout = NULL;
    D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    UINT NumElements = ARRAYSIZE(Layout);
    int ret = 0;

    hr = ID3D11Device_CreateVertexShader(d3d11_device, 
                                         g_VS,
                                         Size,
                                         NULL, &vertex_shader);

    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    hr = ID3D11Device_CreateInputLayout(d3d11_device, 
                                        Layout, 
                                        NumElements, 
                                        g_VS,
                                        Size,
                                        &input_layout);
    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    ID3D11DeviceContext_IASetInputLayout(d3d11_device_ctx, input_layout);

    Size = ARRAYSIZE(g_PS);
    hr = ID3D11Device_CreatePixelShader(d3d11_device, 
                                        g_PS, 
                                        Size, 
                                        NULL, &sample_pixel_shader);
    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    Size = ARRAYSIZE(g_PS_Y);
    hr = ID3D11Device_CreatePixelShader(d3d11_device, 
                                        g_PS_Y, 
                                        Size, 
                                        NULL, &pixel_shader_luminance);
    if (FAILED(hr)) {
      ret = AVERROR(EIO);
      goto error;
    }

    Size = ARRAYSIZE(g_PS_UV);
    hr = ID3D11Device_CreatePixelShader(d3d11_device,
                                        g_PS_UV,
                                        Size,
                                        NULL, &pixel_shader_chrominance);
    if (FAILED(hr)) {
      ret = AVERROR(EIO);
      goto error;
    }

    dxgigrab->vertex_shader = vertex_shader;
    dxgigrab->sampler_pixel_shader = sample_pixel_shader;
    dxgigrab->input_layout = input_layout;
    dxgigrab->pixel_shader_chrominance = pixel_shader_chrominance;
    dxgigrab->pixel_shader_luminance = pixel_shader_luminance;

    return 0;

error:
    if (vertex_shader) {
        ID3D11VertexShader_Release(vertex_shader);
    }
    if (sample_pixel_shader) {
        ID3D11PixelShader_Release(sample_pixel_shader);
    }
    if (input_layout) {
        ID3D11InputLayout_Release(input_layout);
    }
    if (pixel_shader_chrominance) {
        ID3D11PixelShader_Release(pixel_shader_chrominance);
    }
    if (pixel_shader_luminance) {
        ID3D11PixelShader_Release(pixel_shader_luminance);
    }
    return ret;
}

int InitOutput(struct dxgigrab* dxgigrab) 
{
    HRESULT hr;
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
    D3D_FEATURE_LEVEL feature_level;
    ID3D11Device *d3d11_device = NULL;
    ID3D11DeviceContext *d3d11_device_ctx = NULL;
    ID3D11SamplerState *sampler_linear = NULL;
    D3D11_SAMPLER_DESC samp_desc;
    D3D11_BLEND_DESC blend_state_desc;
    ID3D11BlendState* blend_state = NULL;
    ID3D11Texture2D *shared_surface = NULL;
    ID3D11RenderTargetView *RTV = NULL;;
    RECT desk_bounds;
    int ret = 0;
    
    // Create device
    for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex) {
        hr = D3D11CreateDevice(NULL, 
                               DriverTypes[DriverTypeIndex], 
                               NULL, 
                               0, 
                               FeatureLevels, 
                               NumFeatureLevels,
                               D3D11_SDK_VERSION, 
                               &d3d11_device, 
                               &feature_level, 
                               &d3d11_device_ctx);
        if (SUCCEEDED(hr)) {
            // Device creation succeeded, no need to loop anymore
            break;
        }
    }
    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    // Create shared texture
    ret = CreateSharedSurf(d3d11_device, &desk_bounds, &shared_surface);
    if (ret != 0) {
        ret = AVERROR(EIO);
        goto error;
    }

    // Make new render target view
    ret = MakeRTV(d3d11_device, &RTV, shared_surface);
    if (ret != 0) {
        ret = AVERROR(EIO);
        goto error;
    }

    // Create the sample state
    RtlZeroMemory(&samp_desc, sizeof(samp_desc));
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MinLOD = 0;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(d3d11_device, &samp_desc, &sampler_linear);
    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    // Create the blend state
    blend_state_desc.AlphaToCoverageEnable = FALSE;
    blend_state_desc.IndependentBlendEnable = FALSE;
    blend_state_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_state_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_state_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_state_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_state_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_state_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_state_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_state_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(d3d11_device, &blend_state_desc, &blend_state);
    if (FAILED(hr)) {
        ret = AVERROR(EIO);
        goto error;
    }

    dxgigrab->d3d11_device = d3d11_device;
    dxgigrab->d3d11_device_ctx = d3d11_device_ctx;
    dxgigrab->sampler_linear = sampler_linear;
    dxgigrab->blend_state = blend_state;
    dxgigrab->shared_surf = shared_surface;
    dxgigrab->shared_RTV = RTV;
    dxgigrab->clip_rect = desk_bounds;
    dxgigrab->width = dxgigrab->clip_rect.right - dxgigrab->clip_rect.left;
    dxgigrab->height = dxgigrab->clip_rect.bottom - dxgigrab->clip_rect.top;
    // Align to 2x2 pixel boundaries
    dxgigrab->scaled_width = (dxgigrab->width / dxgigrab->down_sample_factor) & ~1;
    dxgigrab->scaled_height = (dxgigrab->height / dxgigrab->down_sample_factor) & ~1;
    dxgigrab->frame_size =dxgigrab->scaled_width * dxgigrab->scaled_height * 3 / 2; 
    

    // Initialize shaders
    return InitShaders(dxgigrab);

error:
    if (d3d11_device) {
        ID3D11Device_Release(d3d11_device);
    }
    if (d3d11_device_ctx) {
        ID3D11DeviceContext_Release(d3d11_device_ctx);
    }
    if (sampler_linear) {
        ID3D11SamplerState_Release(sampler_linear);
    }
    if (blend_state) {
        ID3D11BlendState_Release(blend_state);
    }
    if (shared_surface) {
        ID3D11Texture2D_Release(shared_surface);
    }
    if (RTV) {
        ID3D11RenderTargetView_Release(RTV);
    }
    return ret;
}

int InitDupl(struct dxgigrab* dxgigrab)
{
    HRESULT hr;
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    IDXGIDevice* dxgi_device = NULL;
    IDXGIAdapter* dxgi_adapter = NULL;
    IDXGIOutput* dxgi_output = NULL;
    DXGI_OUTPUT_DESC *output_desc = &dxgigrab->output_desc;
    IDXGIOutput1* dxgi_output1 = NULL;
    IDXGIOutputDuplication *desk_dupl = NULL;
    int ret = 0;
    INT output = 0;

    // Get DXGI device
    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_IDXGIDevice, &dxgi_device);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Get DXGI adapter
    hr = IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter, &dxgi_adapter);
    IDXGIDevice_Release(dxgi_device);
    dxgi_device = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Get output
    hr = IDXGIAdapter_EnumOutputs(dxgi_adapter, output, &dxgi_output);
    IDXGIAdapter_Release(dxgi_adapter);
    dxgi_adapter = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    IDXGIOutput_GetDesc(dxgi_output, output_desc);

    // QI for output 1
    hr = IDXGIOutput_QueryInterface(dxgi_output, &IID_IDXGIOutput1, &dxgi_output1);
    IDXGIOutput_Release(dxgi_output);
    dxgi_output = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Create desktop duplication
    hr = IDXGIOutput1_DuplicateOutput(dxgi_output1, d3d11_device, &desk_dupl);
    IDXGIOutput1_Release(dxgi_output1);
    dxgi_output1 = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    dxgigrab->desktop_dupl =  desk_dupl;
    return 0;
}

int GetFrame(struct dxgigrab* dxgigrab, FRAME_DATA* Data, BOOL* Timeout)
{
    IDXGIOutputDuplication *desk_dupl = dxgigrab->desktop_dupl;
    ID3D11Texture2D **AcquiredDesktopImage = &dxgigrab->acquired_desktop_image;
    IDXGIResource* DesktopResource = NULL;
    DXGI_OUTDUPL_FRAME_INFO FrameInfo;
    HRESULT hr;

    // Get new frame
    hr = IDXGIOutputDuplication_AcquireNextFrame(desk_dupl, 20, &FrameInfo, &DesktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        *Timeout = TRUE;
        return 0;
    }
    *Timeout = FALSE;

    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // If still holding old frame, destroy it
    if (*AcquiredDesktopImage)
    {
        ID3D11Texture2D_Release(*AcquiredDesktopImage);
        *AcquiredDesktopImage = NULL;
    }

    // QI for IDXGIResource
    hr = IDXGIResource_QueryInterface(DesktopResource, &IID_ID3D11Texture2D, AcquiredDesktopImage);
    IDXGIResource_Release(DesktopResource);
    DesktopResource = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }
    Data->Frame = *AcquiredDesktopImage;
    Data->FrameInfo = FrameInfo;

    return 0;
}

int GetMouse(struct dxgigrab* dxgigrab, PTR_INFO* pointer_info, DXGI_OUTDUPL_FRAME_INFO* FrameInfo, INT OffsetX, INT OffsetY)
{
    INT OutputNumber = 0;
    DXGI_OUTPUT_DESC *output_desc = &dxgigrab->output_desc;
    IDXGIOutputDuplication *desk_dupl = dxgigrab->desktop_dupl;
    HRESULT hr;
    BOOL update_position = TRUE;
    UINT buffer_size_required;

    // A non-zero mouse update timestamp indicates that there is a mouse position update and optionally a shape change
    if (FrameInfo->LastMouseUpdateTime.QuadPart == 0) {
        return 0;
    }

    // Make sure we don't update pointer position wrongly
    // If pointer is invisible, make sure we did not get an update from another output that the last time that said pointer
    // was visible, if so, don't set it to invisible or update.
    if (!FrameInfo->PointerPosition.Visible && (pointer_info->WhoUpdatedPositionLast != OutputNumber)) {
        update_position = FALSE;
    }

    // If two outputs both say they have a visible, only update if new update has newer timestamp
    if (FrameInfo->PointerPosition.Visible 
        && pointer_info->Visible 
        && (pointer_info->WhoUpdatedPositionLast != OutputNumber) 
        && (pointer_info->LastTimeStamp.QuadPart > FrameInfo->LastMouseUpdateTime.QuadPart)) {
        update_position = FALSE;
    }

    // Update position
    if (update_position) {
        pointer_info->Position.x = FrameInfo->PointerPosition.Position.x + output_desc->DesktopCoordinates.left - OffsetX;
        pointer_info->Position.y = FrameInfo->PointerPosition.Position.y + output_desc->DesktopCoordinates.top - OffsetY;
        pointer_info->WhoUpdatedPositionLast = OutputNumber;
        pointer_info->LastTimeStamp = FrameInfo->LastMouseUpdateTime;
        pointer_info->Visible = FrameInfo->PointerPosition.Visible != 0;
    }

    // No new shape
    if (FrameInfo->PointerShapeBufferSize == 0) {
        return 0;
    }

    // Old buffer too small
    if (FrameInfo->PointerShapeBufferSize > pointer_info->BufferSize) {
        if (pointer_info->PtrShapeBuffer) {
            free(pointer_info->PtrShapeBuffer);
            pointer_info->PtrShapeBuffer = NULL;
        }
        pointer_info->PtrShapeBuffer = (BYTE*)malloc(FrameInfo->PointerShapeBufferSize * sizeof(BYTE));
        if (!pointer_info->PtrShapeBuffer) {
            pointer_info->BufferSize = 0;
            return AVERROR(ENOMEM);
        }

        // Update buffer size
        pointer_info->BufferSize = FrameInfo->PointerShapeBufferSize;
    }

    // Get shape
    hr = IDXGIOutputDuplication_GetFramePointerShape(desk_dupl,
                                                     FrameInfo->PointerShapeBufferSize,
                                                     (VOID*)pointer_info->PtrShapeBuffer,
                                                     &buffer_size_required,
                                                     &(pointer_info->ShapeInfo));
    if (FAILED(hr)) {
        free(pointer_info->PtrShapeBuffer);
        pointer_info->PtrShapeBuffer = NULL;
        pointer_info->BufferSize = 0;
        return AVERROR(EIO);
    }

    return 0;
}

void SetDrawVert(VERTEX* vertices, RECT* clip_rect, 
                  INT OffsetX, INT OffsetY, DXGI_OUTPUT_DESC* desk_desc, 
                  D3D11_TEXTURE2D_DESC* full_desc, D3D11_TEXTURE2D_DESC* this_desc)
{
    INT center_X = full_desc->Width / 2;
    INT center_Y = full_desc->Height / 2;

    INT Width = desk_desc->DesktopCoordinates.right - desk_desc->DesktopCoordinates.left;
    INT Height = desk_desc->DesktopCoordinates.bottom - desk_desc->DesktopCoordinates.top;

    // Rotation compensated destination rect
    RECT dest_rect = *clip_rect;

    // Set appropriate coordinates compensated for rotation
    switch (desk_desc->Rotation)
    {
        case DXGI_MODE_ROTATION_ROTATE90:
        {
            dest_rect.left = Width - clip_rect->bottom;
            dest_rect.top = clip_rect->left;
            dest_rect.right = Width - clip_rect->top;
            dest_rect.bottom = clip_rect->right;

            struct xmfloat2 tex_coord0 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord1 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord2 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord5 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            vertices[0].TexCoord = tex_coord0;
            vertices[1].TexCoord = tex_coord1;
            vertices[2].TexCoord = tex_coord2;
            vertices[5].TexCoord = tex_coord5;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180:
        {
            dest_rect.left = Width - clip_rect->right;
            dest_rect.top = Height - clip_rect->bottom;
            dest_rect.right = Width - clip_rect->left;
            dest_rect.bottom = Height - clip_rect->top;

            struct xmfloat2 tex_coord0 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord1 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord2 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord5 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            vertices[0].TexCoord = tex_coord0;
            vertices[1].TexCoord = tex_coord1;
            vertices[2].TexCoord = tex_coord2;
            vertices[5].TexCoord = tex_coord5;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270:
        {
            dest_rect.left = clip_rect->top;
            dest_rect.top = Height - clip_rect->right;
            dest_rect.right = clip_rect->bottom;
            dest_rect.bottom = Height - clip_rect->left;

            struct xmfloat2 tex_coord0 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord1 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord2 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord5 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            vertices[0].TexCoord = tex_coord0;
            vertices[1].TexCoord = tex_coord1;
            vertices[2].TexCoord = tex_coord2;
            vertices[5].TexCoord = tex_coord5;
            break;
        }
        default:
            assert(FALSE); // drop through
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        {
            struct xmfloat2 tex_coord0 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord1 = {clip_rect->left / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord2 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->bottom / (FLOAT)(this_desc->Height)};
            struct xmfloat2 tex_coord5 = {clip_rect->right / (FLOAT)(this_desc->Width), clip_rect->top / (FLOAT)(this_desc->Height)};
            vertices[0].TexCoord = tex_coord0;
            vertices[1].TexCoord = tex_coord1;
            vertices[2].TexCoord = tex_coord2;
            vertices[5].TexCoord = tex_coord5;
            break;
        }
    }

    // Set positions
    struct xmfloat3 pos0 = {(dest_rect.left + desk_desc->DesktopCoordinates.left - OffsetX - center_X) / (FLOAT)(center_X),
                             -1 * (dest_rect.bottom + desk_desc->DesktopCoordinates.top - OffsetY - center_Y) / (FLOAT)(center_Y),
                             0.0f};
    struct xmfloat3 pos1 = {(dest_rect.left + desk_desc->DesktopCoordinates.left - OffsetX - center_X) / (FLOAT)(center_X),
                             -1 * (dest_rect.top + desk_desc->DesktopCoordinates.top - OffsetY - center_Y) / (FLOAT)(center_Y),
                             0.0f};
    struct xmfloat3 pos2 = {(dest_rect.right + desk_desc->DesktopCoordinates.left - OffsetX - center_X) / (FLOAT)(center_X),
                             -1 * (dest_rect.bottom + desk_desc->DesktopCoordinates.top - OffsetY - center_Y) / (FLOAT)(center_Y),
                             0.0f};
    struct xmfloat3 pos5 = {(dest_rect.right + desk_desc->DesktopCoordinates.left - OffsetX - center_X) / (FLOAT)(center_X),
                             -1 * (dest_rect.top + desk_desc->DesktopCoordinates.top - OffsetY - center_Y) / (FLOAT)(center_Y),
                             0.0f};
    vertices[0].Pos = pos0;
    vertices[1].Pos = pos1;
    vertices[2].Pos = pos2;
    vertices[3].Pos = vertices[2].Pos;
    vertices[4].Pos = vertices[1].Pos;
    vertices[5].Pos = pos5;
    vertices[3].TexCoord = vertices[2].TexCoord;
    vertices[4].TexCoord = vertices[1].TexCoord;
}

int DrawFrame(struct dxgigrab* dxgigrab, ID3D11Texture2D* SrcSurface,
              RECT clip_rect, INT OffsetX, INT OffsetY)
{
    ID3D11Texture2D* shared_surf = dxgigrab->shared_surf;
    DXGI_OUTPUT_DESC* desk_desc = &dxgigrab->output_desc;
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    ID3D11RenderTargetView **RTV = &dxgigrab->shared_RTV;

    HRESULT hr;
    D3D11_TEXTURE2D_DESC full_desc;
    D3D11_TEXTURE2D_DESC this_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC shader_desc;
    ID3D11ShaderResourceView* shader_resource = NULL;
    FLOAT blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_SUBRESOURCE_DATA init_data;
    ID3D11Buffer* vert_buf = NULL;
    VERTEX vertices[NUMVERTICES];
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    D3D11_VIEWPORT VP;

    ID3D11Texture2D_GetDesc(shared_surf, &full_desc);

    ID3D11Texture2D_GetDesc(SrcSurface, &this_desc);

    if (!*RTV) {
        hr = ID3D11Device_CreateRenderTargetView(d3d11_device, shared_surf, NULL, RTV);
        if (FAILED(hr)) {
            return AVERROR(EIO);
        }
    }

    shader_desc.Format = this_desc.Format;
    shader_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shader_desc.Texture2D.MostDetailedMip = this_desc.MipLevels - 1;
    shader_desc.Texture2D.MipLevels = this_desc.MipLevels;

    // Create new shader resource view
    hr = ID3D11Device_CreateShaderResourceView(d3d11_device, SrcSurface, &shader_desc, &shader_resource);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    ID3D11DeviceContext_OMSetBlendState(d3d11_device_ctx, NULL, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetRenderTargets(d3d11_device_ctx, 1, RTV, NULL);
    ID3D11DeviceContext_VSSetShader(d3d11_device_ctx, dxgigrab->vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d3d11_device_ctx, dxgigrab->sampler_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d3d11_device_ctx, 0, 1, &shader_resource);
    ID3D11DeviceContext_PSSetSamplers(d3d11_device_ctx, 0, 1, &dxgigrab->sampler_linear);
    ID3D11DeviceContext_IASetPrimitiveTopology(d3d11_device_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    SetDrawVert(vertices, &clip_rect, OffsetX, OffsetY, desk_desc, &full_desc, &this_desc);

    // Create vertex buffer
    RtlZeroMemory(&buffer_desc, sizeof(buffer_desc));
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = 0;
    RtlZeroMemory(&init_data, sizeof(init_data));
    init_data.pSysMem = vertices;

    hr = ID3D11Device_CreateBuffer(d3d11_device, &buffer_desc, &init_data, &vert_buf);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    ID3D11DeviceContext_IASetVertexBuffers(d3d11_device_ctx, 0, 1, &vert_buf, &stride, &offset);

    VP.Width = (FLOAT)(full_desc.Width);
    VP.Height = (FLOAT)(full_desc.Height);
    VP.MinDepth = 0.0f;
    VP.MaxDepth = 1.0f;
    VP.TopLeftX = 0.0f;
    VP.TopLeftY = 0.0f;
    ID3D11DeviceContext_RSSetViewports(d3d11_device_ctx, 1, &VP);

    // apply to pipeline
    ID3D11DeviceContext_Draw(d3d11_device_ctx, NUMVERTICES, 0);

    ID3D11Buffer_Release(vert_buf);
    vert_buf = NULL;

    ID3D11ShaderResourceView_Release(shader_resource);
    shader_resource = NULL;

    return 0;
}

//
// Process both masked and monochrome pointers
//
int ProcessMonoMask(struct dxgigrab* dxgigrab, BOOL is_mono, PTR_INFO* pointer_info, 
                    INT* ptr_width, INT* ptr_height, INT* ptr_left, INT* ptr_top, 
                    BYTE** init_buffer, D3D11_BOX* box)
{
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    D3D11_TEXTURE2D_DESC full_desc;
    INT desktop_width = 0;
    INT desktop_height = 0;
    INT given_left = 0;
    INT given_top = 0;
    D3D11_TEXTURE2D_DESC copy_buffer_desc;
    ID3D11Texture2D* copy_buffer = NULL;
    IDXGISurface* copy_surface = NULL;
    DXGI_MAPPED_RECT mapped_surface;
    HRESULT hr;

    UINT* init_buffer32 = NULL;
    UINT* desktop32 = NULL;
    UINT  desktop_pitch_in_pixels = 0;

    // What to skip (pixel offset)
    UINT skipX = 0;
    UINT skipY = 0;


    // Desktop dimensions
    ID3D11Texture2D_GetDesc(dxgigrab->shared_surf, &full_desc);
    desktop_width = full_desc.Width;
    desktop_height = full_desc.Height;

    // Pointer position
    given_left = pointer_info->Position.x;
    given_top = pointer_info->Position.y;

    // Figure out if any adjustment is needed for out of bound positions
    if (given_left < 0) {
        *ptr_width = given_left + (INT)(pointer_info->ShapeInfo.Width);
    }
    else if ((given_left + (INT)(pointer_info->ShapeInfo.Width)) > desktop_width) {
        *ptr_width = desktop_width - given_left;
    }
    else {
        *ptr_width = (INT)(pointer_info->ShapeInfo.Width);
    }

    if (is_mono) {
        pointer_info->ShapeInfo.Height = pointer_info->ShapeInfo.Height / 2;
    }

    if (given_top < 0) {
        *ptr_height = given_top + (INT)(pointer_info->ShapeInfo.Height);
    }
    else if ((given_top + (INT)(pointer_info->ShapeInfo.Height)) > desktop_height) {
        *ptr_height = desktop_height - given_top;
    }
    else {
        *ptr_height = (INT)(pointer_info->ShapeInfo.Height);
    }

    if (is_mono) {
        pointer_info->ShapeInfo.Height = pointer_info->ShapeInfo.Height * 2;
    }

    *ptr_left = (given_left < 0) ? 0 : given_left;
    *ptr_top = (given_top < 0) ? 0 : given_top;

    // Staging buffer/texture
    copy_buffer_desc.Width = *ptr_width;
    copy_buffer_desc.Height = *ptr_height;
    copy_buffer_desc.MipLevels = 1;
    copy_buffer_desc.ArraySize = 1;
    copy_buffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    copy_buffer_desc.SampleDesc.Count = 1;
    copy_buffer_desc.SampleDesc.Quality = 0;
    copy_buffer_desc.Usage = D3D11_USAGE_STAGING;
    copy_buffer_desc.BindFlags = 0;
    copy_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    copy_buffer_desc.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(d3d11_device, &copy_buffer_desc, NULL, &copy_buffer);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Copy needed part of desktop image
    box->left = *ptr_left;
    box->top = *ptr_top;
    box->right = *ptr_left + *ptr_width;
    box->bottom = *ptr_top + *ptr_height;

    ID3D11DeviceContext_CopySubresourceRegion(d3d11_device_ctx, 
                                              copy_buffer,
                                              0, 0, 0, 0, 
                                              dxgigrab->shared_surf,
                                              0,
                                              box);

    // QI for IDXGISurface
    hr = ID3D11Texture2D_QueryInterface(copy_buffer, &IID_IDXGISurface, &copy_surface);
    ID3D11Texture2D_Release(copy_buffer);
    copy_buffer = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Map pixels
    hr = IDXGISurface_Map(copy_surface, &mapped_surface, DXGI_MAP_READ);
    if (FAILED(hr)) {
        IDXGISurface_Release(copy_surface);
        copy_surface = NULL;
        return AVERROR(EIO);
    }

    // New mouseshape buffer
    *init_buffer = (BYTE*)malloc(*ptr_width * *ptr_height * BPP * sizeof(BYTE));
    if (!(*init_buffer)) {
        return AVERROR(ENOMEM);
    }

    init_buffer32 = (UINT*)(*init_buffer);
    desktop32 = (UINT*)(mapped_surface.pBits);
    desktop_pitch_in_pixels = mapped_surface.Pitch / sizeof(UINT);

    // What to skip (pixel offset)
    skipX = (given_left < 0) ? (-1 * given_left) : (0);
    skipY = (given_top < 0) ? (-1 * given_top) : (0);

    if (is_mono) {
        for (INT row = 0; row < *ptr_height; ++row) {
            // Set mask
            BYTE mask = 0x80;
            mask = mask >> (skipX % 8);
            for (INT col = 0; col < *ptr_width; ++col) {
                // Get masks using appropriate offsets
                BYTE and_mask = pointer_info->PtrShapeBuffer[((col + skipX) / 8) + ((row + skipY) * (pointer_info->ShapeInfo.Pitch))] & mask;
                BYTE xor_mask = pointer_info->PtrShapeBuffer[((col + skipX) / 8) + ((row + skipY + (pointer_info->ShapeInfo.Height / 2)) * (pointer_info->ShapeInfo.Pitch))] & mask;
                UINT and_mask32 = (and_mask) ? 0xFFFFFFFF : 0xFF000000;
                UINT XorMask32 = (xor_mask) ? 0x00FFFFFF : 0x00000000;

                // Set new pixel
                init_buffer32[(row * *ptr_width) + col] = (desktop32[(row * desktop_pitch_in_pixels) + col] & and_mask32) ^ XorMask32;

                // Adjust mask
                if (mask == 0x01) {
                    mask = 0x80;
                }
                else {
                    mask = mask >> 1;
                }
            }
        }
    }
    else
    {
        UINT* buffer32 = (UINT*)(pointer_info->PtrShapeBuffer);

        // Iterate through pixels
        for (INT row = 0; row < *ptr_height; ++row) {
            for (INT col = 0; col < *ptr_width; ++col) {
                // Set up mask
                UINT mask_val = 0xFF000000 & buffer32[(col + skipX) + ((row + skipY) * (pointer_info->ShapeInfo.Pitch / sizeof(UINT)))];
                if (mask_val) {
                    // mask was 0xFF
                    init_buffer32[(row * *ptr_width) + col] = (desktop32[(row * desktop_pitch_in_pixels) + col] ^ buffer32[(col + skipX) + ((row + skipY) * (pointer_info->ShapeInfo.Pitch / sizeof(UINT)))]) | 0xFF000000;
                }
                else {
                    // mask was 0x00
                    init_buffer32[(row * *ptr_width) + col] = buffer32[(col + skipX) + ((row + skipY) * (pointer_info->ShapeInfo.Pitch / sizeof(UINT)))] | 0xFF000000;
                }
            }
        }
    }

    // Done with resource
    hr = IDXGISurface_Unmap(copy_surface);
    IDXGISurface_Release(copy_surface);
    copy_surface = NULL;
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    return 0;
}

//
// Draw mouse provided in buffer to backbuffer
//
int DrawMouse(struct dxgigrab* dxgigrab, PTR_INFO* pointer_info)
{
    ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    ID3D11Texture2D* mouse_tex = NULL;
    ID3D11ShaderResourceView* shader_res = NULL;
    ID3D11Buffer* vertex_buffer_mouse = NULL;
    D3D11_SUBRESOURCE_DATA init_data;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC sdesc;
    D3D11_TEXTURE2D_DESC full_desc;
    D3D11_BUFFER_DESC bdesc;
    HRESULT hr;
    FLOAT blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;

    VERTEX vertices[NUMVERTICES] = {
        {VF00, VF01},
        {VF10, VF11},
        {VF20, VF21},
        {VF30, VF31},
        {VF40, VF41},
        {VF50, VF51}
    };

    ID3D11Texture2D_GetDesc(dxgigrab->shared_surf, &full_desc);
    INT desktop_width = full_desc.Width;
    INT desktop_height = full_desc.Height;

    // Center of desktop dimensions
    INT center_X = (desktop_width / 2);
    INT center_Y = (desktop_height / 2);

    // Clipping adjusted coordinates / dimensions
    INT ptr_width = 0;
    INT ptr_height = 0;
    INT ptr_left = 0;
    INT ptr_top = 0;

    // Buffer used if necessary (in case of monochrome or masked pointer)
    BYTE* init_buffer = NULL;

    // Used for copying pixels
    D3D11_BOX box;
    box.front = 0;
    box.back = 1;

    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    // Set shader resource properties
    sdesc.Format = desc.Format;
    sdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sdesc.Texture2D.MostDetailedMip = desc.MipLevels - 1;
    sdesc.Texture2D.MipLevels = desc.MipLevels;

    switch (pointer_info->ShapeInfo.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        {
            ptr_left = pointer_info->Position.x;
            ptr_top = pointer_info->Position.y;

            ptr_width = (INT)(pointer_info->ShapeInfo.Width);
            ptr_height = (INT)(pointer_info->ShapeInfo.Height);

            break;
        }

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        {
            ProcessMonoMask(dxgigrab, TRUE, pointer_info, &ptr_width, &ptr_height, &ptr_left, &ptr_top, &init_buffer, &box);
            break;
        }

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
            ProcessMonoMask(dxgigrab, FALSE, pointer_info, &ptr_width, &ptr_height, &ptr_left, &ptr_top, &init_buffer, &box);
            break;
        }

        default:
            break;
    }

    // VERTEX creation
    vertices[0].Pos.x = (ptr_left - center_X) / (FLOAT)center_X;
    vertices[0].Pos.y = -1 * ((ptr_top + ptr_height) - center_Y) / (FLOAT)center_Y;
    vertices[1].Pos.x = (ptr_left - center_X) / (FLOAT)center_X;
    vertices[1].Pos.y = -1 * (ptr_top - center_Y) / (FLOAT)center_Y;
    vertices[2].Pos.x = ((ptr_left + ptr_width) - center_X) / (FLOAT)center_X;
    vertices[2].Pos.y = -1 * ((ptr_top + ptr_height) - center_Y) / (FLOAT)center_Y;
    vertices[3].Pos.x = vertices[2].Pos.x;
    vertices[3].Pos.y = vertices[2].Pos.y;
    vertices[4].Pos.x = vertices[1].Pos.x;
    vertices[4].Pos.y = vertices[1].Pos.y;
    vertices[5].Pos.x = ((ptr_left + ptr_width) - center_X) / (FLOAT)center_X;
    vertices[5].Pos.y = -1 * (ptr_top - center_Y) / (FLOAT)center_Y;

    // Set texture properties
    desc.Width = ptr_width;
    desc.Height = ptr_height;

    // Set up init data
    init_data.pSysMem = (pointer_info->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? pointer_info->PtrShapeBuffer : init_buffer;
    init_data.SysMemPitch = (pointer_info->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? pointer_info->ShapeInfo.Pitch : ptr_width * BPP;
    init_data.SysMemSlicePitch = 0;

    // Create mouseshape as texture
    hr = ID3D11Device_CreateTexture2D(d3d11_device, &desc, &init_data, &mouse_tex);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    // Create shader resource from texture
    hr = ID3D11Device_CreateShaderResourceView(d3d11_device, mouse_tex, &sdesc, &shader_res);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(mouse_tex);
        mouse_tex = NULL;
        return AVERROR(EIO);
    }

    ZeroMemory(&bdesc, sizeof(D3D11_BUFFER_DESC));
    bdesc.Usage = D3D11_USAGE_DEFAULT;
    bdesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
    bdesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bdesc.CPUAccessFlags = 0;

    ZeroMemory(&init_data, sizeof(D3D11_SUBRESOURCE_DATA));
    init_data.pSysMem = vertices;

    // Create vertex buffer
    hr = ID3D11Device_CreateBuffer(d3d11_device, &bdesc, &init_data, &vertex_buffer_mouse);
    if (FAILED(hr)) {
        ID3D11ShaderResourceView_Release(shader_res);
        shader_res = NULL;
        ID3D11Texture2D_Release(mouse_tex);
        mouse_tex = NULL;
        return AVERROR(EIO);
    }

    // Set resources
    ID3D11DeviceContext_IASetVertexBuffers(d3d11_device_ctx, 0, 1, &vertex_buffer_mouse, &stride, &offset);
    ID3D11DeviceContext_OMSetBlendState(d3d11_device_ctx, dxgigrab->blend_state, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetRenderTargets(d3d11_device_ctx, 1, &dxgigrab->shared_RTV, NULL);
    ID3D11DeviceContext_VSSetShader(d3d11_device_ctx, dxgigrab->vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d3d11_device_ctx, dxgigrab->sampler_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d3d11_device_ctx, 0, 1, &shader_res);
    ID3D11DeviceContext_PSSetSamplers(d3d11_device_ctx, 0, 1, &dxgigrab->sampler_linear);

    // Draw
    ID3D11DeviceContext_Draw(d3d11_device_ctx, NUMVERTICES, 0);

    // Clean
    if (vertex_buffer_mouse) {
        ID3D11Buffer_Release(vertex_buffer_mouse);
        vertex_buffer_mouse = NULL;
    }
    if (shader_res) {
        ID3D11ShaderResourceView_Release(shader_res);
        shader_res = NULL;
    }
    if (mouse_tex) {
        ID3D11Texture2D_Release(mouse_tex);
        mouse_tex = NULL;
    }
    if (init_buffer) {
        free(init_buffer);
        init_buffer = NULL;
    }

    return 0;
}

int DoneWithFrame(struct dxgigrab* dxgigrab) {
    HRESULT hr;
    hr = IDXGIOutputDuplication_ReleaseFrame(dxgigrab->desktop_dupl);
    if (FAILED(hr)) {
        return AVERROR(EIO);
    }

    if (dxgigrab->acquired_desktop_image)
    {
        ID3D11Texture2D_Release(dxgigrab->acquired_desktop_image);
        dxgigrab->acquired_desktop_image = NULL;
    }
    return 0; 
}

int InitNV12Surfaces(struct dxgigrab *dxgigrab) {
  ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
  DXGI_OUTPUT_DESC output_desc = dxgigrab->output_desc;
  ID3D11Texture2D *scale_src_surf = NULL;
  ID3D11Texture2D *luminance_surf = NULL;
  ID3D11Texture2D *cpu_accessible_luminance_surf = NULL;
  ID3D11Texture2D *cpu_accessible_chrominance_surf = NULL;
  ID3D11Texture2D *chrominance_surf = NULL;
  D3D11_VIEWPORT  vp_luminance;
  D3D11_VIEWPORT  vp_chrominance;
  ID3D11RenderTargetView  *luminance_RTV = NULL;
  ID3D11RenderTargetView  *chrominance_RTV = NULL;
  D3D11_TEXTURE2D_DESC desk_tex_desc;
  HRESULT hr;
  int ret = 0;


  RtlZeroMemory(&desk_tex_desc, sizeof(D3D11_TEXTURE2D_DESC));
  desk_tex_desc.Width = dxgigrab->width;
  desk_tex_desc.Height = dxgigrab->height;
  desk_tex_desc.MipLevels = 1;
  desk_tex_desc.ArraySize = 1;
  desk_tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desk_tex_desc.SampleDesc.Count = 1;
  desk_tex_desc.Usage = D3D11_USAGE_DEFAULT;
  desk_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, &scale_src_surf);
  if (FAILED(hr) || !scale_src_surf) {
      return AVERROR(EIO);
  }

  // Destination texture may be scaled.
  desk_tex_desc.Width = dxgigrab->scaled_width;
  desk_tex_desc.Height = dxgigrab->scaled_height;
  desk_tex_desc.MipLevels = 1;
  desk_tex_desc.Format = DXGI_FORMAT_R8_UNORM; 
  desk_tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, &luminance_surf);
  if (FAILED(hr) || !luminance_surf) {
      ret = AVERROR(EIO);
      goto error;
  }

  desk_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desk_tex_desc.Usage = D3D11_USAGE_STAGING;
  desk_tex_desc.BindFlags = 0;

  hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, &cpu_accessible_luminance_surf);
  if (FAILED(hr) || cpu_accessible_luminance_surf == NULL) {
      ret = AVERROR(EIO);
      goto error;
  }

  // set view port
  vp_luminance.Width = (FLOAT)(desk_tex_desc.Width);
  vp_luminance.Height = (FLOAT)(desk_tex_desc.Height);
  vp_luminance.MinDepth = 0.0f;
  vp_luminance.MaxDepth = 1.0f;
  vp_luminance.TopLeftX = 0;
  vp_luminance.TopLeftY = 0;

  ret = MakeRTV(d3d11_device, &luminance_RTV, luminance_surf);
  if(ret != 0) {
      goto error;
  }

  desk_tex_desc.Width = desk_tex_desc.Width / 2;
  desk_tex_desc.Height = desk_tex_desc.Height / 2;
  desk_tex_desc.Format = DXGI_FORMAT_R8G8_UNORM; 
  
  desk_tex_desc.Usage = D3D11_USAGE_DEFAULT;
  desk_tex_desc.CPUAccessFlags = 0;
  desk_tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, &chrominance_surf);
  if (FAILED(hr) || !chrominance_surf) {
      ret = AVERROR(EIO);
      goto error;
  }

  desk_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desk_tex_desc.Usage = D3D11_USAGE_STAGING;
  desk_tex_desc.BindFlags = 0;

  hr = ID3D11Device_CreateTexture2D(d3d11_device, &desk_tex_desc, NULL, &cpu_accessible_chrominance_surf);
  if (FAILED(hr) || cpu_accessible_chrominance_surf == NULL) {
      ret = AVERROR(EIO);
      goto error;
  }

  // set view port
  vp_chrominance.Width = (FLOAT)(desk_tex_desc.Width);
  vp_chrominance.Height = (FLOAT)(desk_tex_desc.Height);
  vp_chrominance.MinDepth = 0.0f;
  vp_chrominance.MaxDepth = 1.0f;
  vp_chrominance.TopLeftX = 0;
  vp_chrominance.TopLeftY = 0;

  ret = MakeRTV(d3d11_device, &chrominance_RTV, chrominance_surf);
  if (ret != 0) {
      goto error;
  }

  dxgigrab->scale_src_surf = scale_src_surf;
  dxgigrab->luminance_surf = luminance_surf;
  dxgigrab->cpu_accessible_luminance_surf = cpu_accessible_luminance_surf;
  dxgigrab->cpu_accessible_chrominance_surf = cpu_accessible_chrominance_surf;
  dxgigrab->chrominance_surf = chrominance_surf;
  dxgigrab->vp_luminance = vp_luminance;
  dxgigrab->vp_chrominance = vp_chrominance;
  dxgigrab->luminance_RTV = luminance_RTV;
  dxgigrab->chrominance_RTV = chrominance_RTV;
  return 0;

error:
  if (scale_src_surf)
      ID3D11Texture2D_Release(scale_src_surf);
  if (luminance_surf)
      ID3D11Texture2D_Release(luminance_surf);
  if (cpu_accessible_luminance_surf)
      ID3D11Texture2D_Release(cpu_accessible_luminance_surf);
  if (cpu_accessible_chrominance_surf)
      ID3D11Texture2D_Release(cpu_accessible_chrominance_surf);
  if (chrominance_surf)
      ID3D11Texture2D_Release(chrominance_surf);
  if (luminance_RTV)
    ID3D11RenderTargetView_Release(luminance_RTV);
  if (chrominance_RTV)
    ID3D11RenderTargetView_Release(chrominance_RTV);
    return ret;
}

//
// Draw frame for NV12 texture
//
int DrawNV12Frame(struct dxgigrab *dxgigrab) {
  D3D11_TEXTURE2D_DESC frame_desc;
  D3D11_SHADER_RESOURCE_VIEW_DESC shader_desc;
  ID3D11ShaderResourceView* shader_resource = NULL;
  ID3D11Texture2D *scale_src_surf = dxgigrab->scale_src_surf;
  ID3D11Device *d3d11_device = dxgigrab->d3d11_device;
  ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
  ID3D11RenderTargetView  **luminance_RTV = &dxgigrab->luminance_RTV;
  ID3D11RenderTargetView  **chrominance_RTV = &dxgigrab->chrominance_RTV;
  ID3D11PixelShader *pixel_shader_luminance = dxgigrab->pixel_shader_luminance;
  ID3D11PixelShader *pixel_shader_chrominance = dxgigrab->pixel_shader_chrominance;
  D3D11_VIEWPORT  *vp_luminance = &dxgigrab->vp_luminance;
  D3D11_VIEWPORT  *vp_chrominance = &dxgigrab->vp_chrominance;

  D3D11_BUFFER_DESC bdesc;
  D3D11_SUBRESOURCE_DATA init_data;
  ID3D11Buffer* VertexBuffer = NULL;
  UINT stride = sizeof(VERTEX);
  FLOAT blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
  UINT offset = 0;

  HRESULT hr;
  int ret = 0;

  ID3D11Texture2D_GetDesc(scale_src_surf, &frame_desc);

  shader_desc.Format = frame_desc.Format;
  shader_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  shader_desc.Texture2D.MostDetailedMip = frame_desc.MipLevels - 1;
  shader_desc.Texture2D.MipLevels = frame_desc.MipLevels;

  // Create new shader resource view
  hr = ID3D11Device_CreateShaderResourceView(d3d11_device, scale_src_surf, &shader_desc, &shader_resource);
  if (FAILED(hr)) {
      return AVERROR(EIO);
  }

  ID3D11DeviceContext_PSSetShaderResources(d3d11_device_ctx, 0, 1, &shader_resource);

  VERTEX vertices[NUMVERTICES] = {
      {VF00, VF01},
      {VF10, VF11},
      {VF20, VF21},
      {VF30, VF31},
      {VF40, VF41},
      {VF50, VF51}
  };

  ZeroMemory(&bdesc, sizeof(D3D11_BUFFER_DESC));
  bdesc.Usage = D3D11_USAGE_DEFAULT;
  bdesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
  bdesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bdesc.CPUAccessFlags = 0;

  ZeroMemory(&init_data, sizeof(D3D11_SUBRESOURCE_DATA));
  init_data.pSysMem = vertices;

  // Create vertex buffer
  hr = ID3D11Device_CreateBuffer(d3d11_device, &bdesc, &init_data, &VertexBuffer);
  if (FAILED(hr)) {
      return AVERROR(EIO);
  }

  // Set resources
  ID3D11DeviceContext_IASetVertexBuffers(d3d11_device_ctx, 0, 1, &VertexBuffer, &stride, &offset);
  ID3D11DeviceContext_OMSetBlendState(d3d11_device_ctx, NULL, blend_factor, 0xFFFFFFFF);

  // Set resources
  ID3D11DeviceContext_OMSetRenderTargets(d3d11_device_ctx, 1, luminance_RTV, NULL);
  ID3D11DeviceContext_PSSetShader(d3d11_device_ctx, pixel_shader_luminance, NULL, 0);
  ID3D11DeviceContext_RSSetViewports(d3d11_device_ctx, 1, vp_luminance);

  // Draw textured quad onto render target
  ID3D11DeviceContext_Draw(d3d11_device_ctx, NUMVERTICES, 0);

  ID3D11DeviceContext_OMSetRenderTargets(d3d11_device_ctx, 1, chrominance_RTV, NULL);
  ID3D11DeviceContext_PSSetShader(d3d11_device_ctx, pixel_shader_chrominance, NULL, 0);
  ID3D11DeviceContext_RSSetViewports(d3d11_device_ctx, 1, vp_chrominance);

  // Draw textured quad onto render target
  ID3D11DeviceContext_Draw(d3d11_device_ctx, NUMVERTICES, 0);

  // Clean
  if (VertexBuffer) {
      ID3D11Buffer_Release(VertexBuffer);
      VertexBuffer = NULL;
  }

  // Release shader resource
  ID3D11ShaderResourceView_Release(shader_resource);
  shader_resource = NULL;

  return 0;
}

UINT D3D11CalcSubresource( UINT MipSlice, UINT ArraySlice, UINT MipLevels )
{ return MipSlice + ArraySlice * MipLevels; }

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
    const char *filename = s1->filename;
    AVStream   *st       = NULL;
    int ret = 0;

    if (!strncmp(filename, "title=", 6)) {
       av_log(s1, AV_LOG_ERROR,
               "DXGI don't support window capture, please use GDI format.\n");
        return AVERROR(EIO);
    } else if (strcmp(filename, "desktop")){
        av_log(s1, AV_LOG_ERROR,
               "Please use \"desktop\" or \"title=<windowname>\" to specify your target.\n");
        return AVERROR(EIO);
    }

    if (dxgigrab->down_sample_factor < 1 || dxgigrab->down_sample_factor > 10) {
        dxgigrab->down_sample_factor = 1;
    }

    if ((ret = InitOutput(dxgigrab)) != 0) {
       av_log(s1, AV_LOG_ERROR,
               "Initialize output content failed.\n");
       return ret;
    }

    if ((ret = InitDupl(dxgigrab)) != 0) {
       av_log(s1, AV_LOG_ERROR,
               "Initialize duplication failed.\n");
       return ret;
    }

    if (ret = InitNV12Surfaces(dxgigrab) != 0) {
        av_log(s1, AV_LOG_ERROR,
            "Initialize NV12 surfaces failed!");
        return ret;
    }

    st = avformat_new_stream(s1, NULL);
    if (!st) {
        return AVERROR(ENOMEM);
    }
    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    dxgigrab->time_base   = av_inv_q(dxgigrab->framerate);
    dxgigrab->time_frame  = av_gettime() / av_q2d(dxgigrab->time_base);

    st->avg_frame_rate = av_inv_q(dxgigrab->time_base);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->bit_rate   = dxgigrab->frame_size * 1/av_q2d(dxgigrab->time_base) * 8;
    st->codecpar->format = AV_PIX_FMT_NV12;
    st->codecpar->width = dxgigrab->scaled_width;
    st->codecpar->height = dxgigrab->scaled_height;
    return 0;
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
    ID3D11DeviceContext *d3d11_device_ctx = dxgigrab->d3d11_device_ctx;
    int ret = 0;
    BOOL time_out = FALSE;
    FRAME_DATA current_data;
    HRESULT hr;
    D3D11_MAPPED_SUBRESOURCE resource;
    UINT subresource = D3D11CalcSubresource(0, 0, 0);

    RECT clip_rect = dxgigrab->clip_rect;
    int width = dxgigrab->width;
    int height = dxgigrab->height;
    int64_t curtime, delay;
    AVRational time_base  = dxgigrab->time_base;
    int64_t    time_frame = dxgigrab->time_frame;
    RECT draw_rect;

    BYTE* sptr = NULL;
    BYTE* dptr = NULL;

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

    ret = GetFrame(dxgigrab, &current_data, &time_out);
    if (ret != 0) {
        av_log(s1, AV_LOG_ERROR, "Read frame failed.\n");
        return ret;
    }

    if (time_out) {
        return AVERROR(EAGAIN);
    }

    if (dxgigrab->draw_mouse) {
        ret = GetMouse(dxgigrab, &dxgigrab->pointer_info, &(current_data.FrameInfo), clip_rect.left, clip_rect.top);
        if (ret != 0) {
            av_log(s1, AV_LOG_ERROR, "Get mouse infomation failed.\n");
            DoneWithFrame(dxgigrab);
            return ret;
        }
    }

    // Rotate draw retangle.
    switch(dxgigrab->output_desc.Rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
        {
            draw_rect.left = clip_rect.top;
            draw_rect.top = width  - clip_rect.right;
            draw_rect.right = clip_rect.bottom;
            draw_rect.bottom = width - clip_rect.left;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180:
        {
            draw_rect.left = width - clip_rect.right;
            draw_rect.top = height - clip_rect.bottom;
            draw_rect.right = width - clip_rect.left;
            draw_rect.bottom = height - clip_rect.top;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270:
        {
            draw_rect.left = height - clip_rect.bottom;
            draw_rect.top = clip_rect.left;
            draw_rect.right = height - clip_rect.top;
            draw_rect.bottom = clip_rect.right;
            break;
        }
        default:
            assert(FALSE); // drop through
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        {
            draw_rect.left = clip_rect.left;
            draw_rect.right = clip_rect.right;
            draw_rect.top = clip_rect.top;
            draw_rect.bottom = clip_rect.bottom;
        }
    };

    ret = DrawFrame(dxgigrab, 
      current_data.Frame,
      draw_rect,
      clip_rect.left, 
      clip_rect.top);

    DoneWithFrame(dxgigrab);

    if (ret != 0) {
        av_log(s1, AV_LOG_ERROR, "Draw frame failed.\n");
        return ret;
    }

    if (dxgigrab->pointer_info.Visible) {
      if ((ret = DrawMouse(dxgigrab, &dxgigrab->pointer_info)) != 0) {
          av_log(s1, AV_LOG_ERROR, "Draw mouse failed.\n");
          return ret;
      }
    }

    ID3D11DeviceContext_CopyResource(d3d11_device_ctx, dxgigrab->scale_src_surf, dxgigrab->shared_surf);

    if ((ret = DrawNV12Frame(dxgigrab)) != 0) {
        av_log(s1, AV_LOG_ERROR, "Draw NV12 failed.\n");
        return ret;
    }

    ID3D11DeviceContext_CopyResource(d3d11_device_ctx, dxgigrab->cpu_accessible_luminance_surf, dxgigrab->luminance_surf);

    hr = ID3D11DeviceContext_Map(d3d11_device_ctx,  
                                 dxgigrab->cpu_accessible_luminance_surf, 
                                 subresource, 
                                 D3D11_MAP_READ, 
                                 0, &resource);

    width = dxgigrab->scaled_width;
    height = dxgigrab->scaled_height;
    if (av_new_packet(pkt, 
                      resource.RowPitch * height + resource.RowPitch * height / 2) < 0)
        return AVERROR(ENOMEM);

    sptr = (BYTE*)(resource.pData);
    dptr = pkt->data;

    for (int i = 0; i < height; i++)
    {
      memcpy_s(dptr, width, sptr, width);
      sptr += resource.RowPitch;
      dptr += width;
    }
    ID3D11DeviceContext_Unmap(d3d11_device_ctx, dxgigrab->cpu_accessible_luminance_surf, subresource);

    ID3D11DeviceContext_CopyResource(d3d11_device_ctx, 
                                     dxgigrab->cpu_accessible_chrominance_surf,
                                     dxgigrab->chrominance_surf);

    hr = ID3D11DeviceContext_Map(d3d11_device_ctx, 
                                 dxgigrab->cpu_accessible_chrominance_surf, 
                                 subresource, 
                                 D3D11_MAP_READ, 
                                 0, &resource);

    sptr = (BYTE*)(resource.pData);

    for (int i = 0; i < height/2; i++)
    {
      memcpy_s(dptr, width, sptr, width);
      sptr += resource.RowPitch;
      dptr += width;
    }

    ID3D11DeviceContext_Unmap(d3d11_device_ctx, dxgigrab->cpu_accessible_chrominance_surf, subresource);

    dxgigrab->time_frame = time_frame;
    ret = dxgigrab->frame_size;
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
    if (s->d3d11_device_ctx) {
      ref = ID3D11DeviceContext_Release(s->d3d11_device_ctx); 
      av_log(s1, AV_LOG_DEBUG, "d3d11_device_ctx %d\n", ref);
    }
    if (s->desktop_dupl) { 
      ref = IDXGIOutputDuplication_Release(s->desktop_dupl);
      av_log(s1, AV_LOG_DEBUG, "dxgi_output_duplication %d\n", ref);
    }
    if (s->shared_surf) {
      ref = IDXGIResource_Release(s->shared_surf);
      av_log(s1, AV_LOG_DEBUG, "shared_surf %d\n", ref);
    }
    if (s->scale_src_surf) {
      ref = IDXGIResource_Release(s->scale_src_surf);
      av_log(s1, AV_LOG_DEBUG, "scale_src_surf %d\n", ref);
    }
    if (s->luminance_surf) {
      ref = IDXGIResource_Release(s->luminance_surf);
      av_log(s1, AV_LOG_DEBUG, "luminance_surf %d\n", ref);
    }
    if (s->chrominance_surf) {
      ref = IDXGIResource_Release(s->chrominance_surf);
      av_log(s1, AV_LOG_DEBUG, "chrominance_surf %d\n", ref);
    }
    if (s->cpu_accessible_luminance_surf) {
      ref = IDXGIResource_Release(s->cpu_accessible_luminance_surf);
      av_log(s1, AV_LOG_DEBUG, "cpu_accessible_luminance_surf %d\n", ref);
    }
    if (s->cpu_accessible_chrominance_surf) {
      ref = IDXGIResource_Release(s->cpu_accessible_chrominance_surf);
      av_log(s1, AV_LOG_DEBUG, "cpu_accessible_chrominance_surf %d\n", ref);
    }
    if (s->luminance_RTV) {
      ref = ID3D11RenderTargetView_Release(s->luminance_RTV);
      av_log(s1, AV_LOG_DEBUG, "luminance_RTV %d\n", ref);
    }
    if (s->chrominance_RTV) {
      ref = ID3D11RenderTargetView_Release(s->chrominance_RTV);
      av_log(s1, AV_LOG_DEBUG, "chrominance_RTV %d\n", ref);
    }
    if (s->acquired_desktop_image) {
        ref = ID3D11Texture2D_Release(s->acquired_desktop_image);
        av_log(s1, AV_LOG_DEBUG, "acquired_desktop_image %d\n", ref);
    }
    if (s->pointer_info.PtrShapeBuffer) {
        free(s->pointer_info.PtrShapeBuffer);
        s->pointer_info.PtrShapeBuffer = NULL;
    }
    if (s->pixel_shader_luminance) {
      ref = ID3D11PixelShader_Release(s->pixel_shader_luminance);
      av_log(s1, AV_LOG_DEBUG, "pixel_shader_luminance %d\n", ref);
    }
    if (s->pixel_shader_chrominance) {
      ref = ID3D11PixelShader_Release(s->pixel_shader_chrominance);
      av_log(s1, AV_LOG_DEBUG, "pixel_shader_chrominance %d\n", ref);
    }
    if (s->shared_RTV) {
      ref = ID3D11RenderTargetView_Release(s->shared_RTV);
      av_log(s1, AV_LOG_DEBUG, "shared_RTV %d\n", ref);
    }
    if (s->sampler_linear) {
        ref = ID3D11SamplerState_Release(s->sampler_linear);
      av_log(s1, AV_LOG_DEBUG, "sampler_linear %d\n", ref);
    }
    if (s->blend_state) {
        ref = ID3D11BlendState_Release(s->blend_state);
        av_log(s1, AV_LOG_DEBUG, "blend_state %d\n", ref);
    }
    if (s->vertex_shader) {
        ref = ID3D11VertexShader_Release(s->vertex_shader);
        av_log(s1, AV_LOG_DEBUG, "vertex_shader %d\n", ref);
    }
    if (s->sampler_pixel_shader) {
        ref = ID3D11VertexShader_Release(s->sampler_pixel_shader);
        av_log(s1, AV_LOG_DEBUG, "sampler_pixel_shader %d\n", ref);
    }
    if (s->input_layout) {
        ref = ID3D11InputLayout_Release(s->input_layout);
        av_log(s1, AV_LOG_DEBUG, "input_layout %d\n", ref);
    }
    if (s->d3d11_device) {
      ref = ID3D11Device_Release(s->d3d11_device); 
      av_log(s1, AV_LOG_DEBUG, "d3d11_device %d\n", ref);
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
    { "down_sample_factor", "Use down sample with specify factor", OFFSET(down_sample_factor), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 10, DEC },
    { NULL },
};

static const AVClass dxgigrab_class = {
    .class_name = "DXGIgrab indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/** DXGI grabber device demuxer declaration */
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