#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float Pi = 3.1415926535f;

    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT4 color;
    };

    struct GrassVertex
    {
            XMFLOAT3 shape;
    };

    struct GrassInstance
    {
        XMFLOAT3 root;
        XMFLOAT3 groundNormal;
        XMFLOAT2 dimensions;
        float phase;
        XMFLOAT4 color;
    };

    struct Mesh
    {
        ComPtr<ID3D11Buffer> vertices;
        ComPtr<ID3D11Buffer> indices;
        UINT indexCount = 0;
    };

    struct SceneConstants
    {
        XMMATRIX viewProjection;
        XMFLOAT3 cameraPosition;
        float time;
    };

    struct ObjectConstants
    {
        XMMATRIX world;
        XMFLOAT4 tint;
    };

    struct GrassConstants
    {
        XMMATRIX viewProjection;
        XMFLOAT3 cameraPosition;
        float time;
    };

    struct SkyConstants
    {
        XMFLOAT3 forward;
        float tanHalfFov;
        XMFLOAT3 right;
        float aspectRatio;
        XMFLOAT3 up;
        float padding;
    };

    const char* SceneShader = R"(
cbuffer Scene : register(b0)
{
    row_major float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
};
cbuffer Object : register(b1)
{
    row_major float4x4 World;
    float4 Tint;
};
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float4 color : COLOR; };
struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};
VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0), World);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0), World).xyz);
    output.color = input.color * Tint;
    return output;
}
float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 sunDirection = normalize(float3(-0.63, 0.32, 0.71));
    float diffuse = saturate(dot(normal, sunDirection));
    float skyFill = 0.36 + saturate(normal.y) * 0.24;
    float sunsetRim = pow(saturate(dot(normal, -sunDirection)), 2.0) * 0.34;
    float variation = 0.94 + 0.06 * sin(input.worldPosition.x * 0.39 + input.worldPosition.z * 0.27);
    float lighting = (skyFill + diffuse * 0.80) * variation;
    float3 color = input.color.rgb * lighting + float3(1.0, 0.34, 0.12) * sunsetRim;
    float distanceFog = saturate((distance(input.worldPosition, CameraPosition) - 140.0) / 1850.0);
    color = lerp(color, float3(0.57, 0.57, 0.55), distanceFog * 0.68);
    float portalGlow = saturate(input.color.r - input.color.g * 1.15);
    color += float3(1.0, 0.38, 0.12) * portalGlow * 0.42;
    return float4(color, input.color.a);
}
)";

    const char* GrassShader = R"(
cbuffer GrassScene : register(b0)
{
    row_major float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
};
struct VSInput
{
    float3 shape : TEXCOORD0;
    float3 root : POSITION1;
    float3 groundNormal : NORMAL1;
    float2 dimensions : TEXCOORD1;
    float phase : TEXCOORD2;
    float4 color : COLOR1;
};
struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : TEXCOORD0;
    float4 color : COLOR;
    float3 worldPosition : TEXCOORD1;
};
VSOutput VSGrass(VSInput input)
{
    VSOutput output;
    float height = input.shape.y;
    float sway = sin(Time * 1.15 + input.phase + input.root.x * 0.11 + input.root.z * 0.17) * 0.08;
    float angle = input.shape.z + input.phase;
    float2 side = float2(cos(angle), sin(angle));
    float2 radial = float2(cos(angle * 2.0), sin(angle * 2.0));
    float3 position = input.root;
    float2 baseOffset = side * input.shape.x * input.dimensions.y * 1.15 +
                        radial * 0.30 * (1.0 - height * 0.86);
    position.xz += baseOffset;
    position.y -= (input.groundNormal.x * baseOffset.x +
                   input.groundNormal.z * baseOffset.y) / max(input.groundNormal.y, 0.45);
    position.x += sway * height * height;
    position.z += sin(input.phase) * sway * 0.18 * height * height;
    position.y += height * clamp(input.dimensions.x, 0.75, 1.35);
    float4 worldPosition = float4(position, 1.0);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = position;
    output.normal = normalize(float3(-side.x * 0.45 - sway * 0.3, 0.60, -side.y * 0.45));
    output.color = input.color;
    return output;
}
float4 PSGrass(VSOutput input) : SV_TARGET
{
    float3 sunDirection = normalize(float3(-0.63, 0.32, 0.71));
    float3 normal = normalize(input.normal);
    float backLight = pow(saturate(dot(normal, -sunDirection)), 1.6);
    float light = 0.58 + saturate(dot(normal, sunDirection)) * 0.52 + backLight * 0.40;
    float tipGlow = smoothstep(0.0, 1.4, input.worldPosition.y) * 0.08;
    float distanceToCamera = distance(input.worldPosition, CameraPosition);
    float distanceFade = saturate((900.0 - distanceToCamera) / 240.0);
    clip(distanceFade - 0.01);
    float3 color = input.color.rgb * (light + tipGlow);
    float fog = saturate((distanceToCamera - 180.0) / 1100.0) * 0.58;
    color = lerp(color, float3(0.57, 0.57, 0.55), fog);
    return float4(color, 1.0);
}
)";

    const char* BubblePixelShader = R"(
cbuffer Scene : register(b0)
{
    row_major float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
};
struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};
float4 PSBubble(VSOutput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(CameraPosition - input.worldPosition);
    float facing = saturate(abs(dot(normal, viewDirection)));
    float rim = pow(1.0 - facing, 2.8);
    float film = 0.5 + 0.5 * sin((facing * 7.0 + input.worldPosition.y * 0.12 + Time * 0.08) * 3.14159);
    float highlight = pow(saturate(dot(normal, normalize(float3(-0.45, 0.72, -0.55)))), 42.0);
    float3 rainbow = lerp(float3(0.62, 0.82, 0.83), float3(0.83, 0.72, 0.84), film);
    float3 color = lerp(float3(0.45, 0.62, 0.69), rainbow, 0.38);
    color += float3(0.30, 0.34, 0.29) * (rim + highlight);
    return float4(color, saturate(0.035 + rim * 0.64 + highlight * 0.8) * input.color.a);
}
)";

    const char* SkyShader = R"(
cbuffer SkyScene : register(b0)
{
    float3 CameraForward;
    float TanHalfFov;
    float3 CameraRight;
    float AspectRatio;
    float3 CameraUp;
    float SkyPadding;
};
struct VSOutput { float4 position : SV_POSITION; float3 direction : TEXCOORD0; float2 uv : TEXCOORD1; };
VSOutput VSSky(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    float2 screen = float2(uv.x - 0.5, 0.5 - uv.y);
    output.direction = CameraForward +
        CameraRight * (screen.x * 2.0 * AspectRatio * TanHalfFov) +
        CameraUp * (screen.y * 2.0 * TanHalfFov);
    output.uv = uv;
    return output;
}
float4 PSSky(VSOutput input) : SV_TARGET
{
    float3 direction = normalize(input.direction);
    float horizon = saturate(direction.y * 1.9 + 0.12);
    float3 bottom = float3(0.72, 0.58, 0.52);
    float3 middle = float3(0.40, 0.52, 0.62);
    float3 zenith = float3(0.27, 0.38, 0.51);
    float3 sky = lerp(bottom, middle, smoothstep(0.0, 0.65, horizon));
    sky = lerp(sky, zenith, smoothstep(0.48, 1.0, horizon));
    float3 sunDirection = normalize(float3(0.0, 0.22, 1.0));
    float sunDot = saturate(dot(direction, sunDirection));
    float sun = pow(sunDot, 115.0);
    float halo = pow(sunDot, 12.0);
    float sunsetGlow = pow(saturate(1.0 - abs(direction.y - 0.07) * 3.0), 2.0);
    sky += float3(0.64, 0.56, 0.48) * (sun * 0.62 + halo * 0.035 + sunsetGlow * 0.035);
    float cloudX = direction.x / max(direction.y + 0.36, 0.12);
    float cloudY = direction.z / max(direction.y + 0.36, 0.12);
    float cloudWarp = sin(cloudY * 1.15 + cloudX * 0.7) * 0.55;
    float billows = sin((cloudX + cloudWarp) * 1.8 + sin(cloudY * 1.4)) *
                    cos(cloudY * 1.25 + sin(cloudX * 0.75));
    float wisps = sin(cloudX * 3.2 + sin(cloudY * 2.4)) *
                  cos(cloudY * 2.8 + cloudX * 0.6);
    float cloudField = billows * 0.72 + wisps * 0.28;
    float clouds = smoothstep(-0.12, 0.42, cloudField) *
                   smoothstep(-0.10, 0.16, direction.y) *
                   (1.0 - smoothstep(0.62, 0.91, direction.y));
    float warmClouds = saturate(1.0 - abs(direction.y - 0.22) * 2.8);
    float3 cloudColor = lerp(float3(0.62, 0.67, 0.69),
                             float3(0.69, 0.57, 0.52), warmClouds * 0.62 + halo * 0.12);
    sky = lerp(sky, cloudColor, clouds * 0.82);
    float sunScreen = acos(sunDot);
    float sunDisc = 1.0 - smoothstep(0.020, 0.028, sunScreen);
    float sunBloom = exp(-sunScreen * sunScreen * 28.0);
    sky = lerp(sky, float3(0.76, 0.68, 0.58), sunDisc);
    sky += float3(0.62, 0.51, 0.42) * sunBloom * 0.045;
    float haze = (1.0 - smoothstep(0.02, 0.42, direction.y)) * 0.22;
    sky = lerp(sky, float3(0.56, 0.56, 0.54), haze);
    return float4(sky, 1.0);
}
)";

    const char* GrassLayoutShader = R"(
struct VSInput { float3 shape : TEXCOORD0; float3 root : POSITION1; float2 dimensions : TEXCOORD1; float phase : TEXCOORD2; float4 color : COLOR1; };
float4 VSLayout(VSInput input) : SV_POSITION { return float4(input.shape, input.phase, 1.0); }
)";

    struct Turbine
    {
        XMFLOAT3 position;
        float scale;
        float phase;
    };

    struct Portal
    {
        XMFLOAT3 position;
        float scale;
    };

    struct Clock
    {
        XMFLOAT3 position;
        float scale;
        float phase;
    };

    struct Bubble
    {
        XMFLOAT2 position;
        float ground;
        float speed;
        float scale;
        float phase;
    };

    class BackgroundMusic
    {
    public:
        void start(const std::wstring& path)
        {
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                throw std::runtime_error("Could not find music\\music.mp3 beside the application.");

            const std::wstring command = L"open \"" + path + L"\" type mpegvideo alias LiminalMeadowMusic";
            send(command.c_str(), "Could not open music\\music.mp3.");
            opened_ = true;
            send(L"play LiminalMeadowMusic repeat", "Could not start the background music.");
            wchar_t mode[64]{};
            const MCIERROR result = mciSendStringW(L"status LiminalMeadowMusic mode",
                                                   mode, ARRAYSIZE(mode), nullptr);
            if (result != 0 || std::wcscmp(mode, L"playing") != 0)
            {
                mciSendStringW(L"close LiminalMeadowMusic", nullptr, 0, nullptr);
                opened_ = false;
                throw std::runtime_error("Background music did not enter the playing state.");
            }
        }

        ~BackgroundMusic()
        {
            if (opened_)
                mciSendStringW(L"close LiminalMeadowMusic", nullptr, 0, nullptr);
        }

    private:
        static void send(const wchar_t* command, const char* message)
        {
            const MCIERROR result = mciSendStringW(command, nullptr, 0, nullptr);
            if (result != 0)
                throw std::runtime_error(message);
        }

        bool opened_ = false;
    };

    class Meadow
    {
    public:
        void initialize(HWND window)
        {
            RECT client{};
            GetClientRect(window, &client);
            width_ = std::max<LONG>(client.right, 1);
            height_ = std::max<LONG>(client.bottom, 1);

            DXGI_SWAP_CHAIN_DESC swapDescription{};
            swapDescription.BufferCount = 2;
            swapDescription.BufferDesc.Width = width_;
            swapDescription.BufferDesc.Height = height_;
            swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapDescription.BufferDesc.RefreshRate.Numerator = 60;
            swapDescription.BufferDesc.RefreshRate.Denominator = 1;
            swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDescription.OutputWindow = window;
            swapDescription.SampleDesc.Count = 1;
            swapDescription.Windowed = TRUE;
            swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            D3D_FEATURE_LEVEL featureLevel{};
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
            const HRESULT result = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                D3D11_SDK_VERSION, &swapDescription, &swapChain_, &device_, &featureLevel, &context_);
            if (FAILED(result))
                throw std::runtime_error("Direct3D 11 device creation failed.");

            createShaders();
            createWorld();
            createBuffers();
            resize(width_, height_);
        }

        void resize(UINT width, UINT height)
        {
            if (!device_ || width == 0 || height == 0)
                return;
            width_ = width;
            height_ = height;
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            renderTarget_.Reset();
            depthView_.Reset();
            depthBuffer_.Reset();
            const HRESULT result = swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(result))
                throw std::runtime_error("Could not resize the Direct3D back buffer.");

            ComPtr<ID3D11Texture2D> backBuffer;
            check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "Could not access the Direct3D back buffer.");
            check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_), "Could not create the back-buffer view.");
            D3D11_TEXTURE2D_DESC depthDescription{};
            depthDescription.Width = width_;
            depthDescription.Height = height_;
            depthDescription.MipLevels = 1;
            depthDescription.ArraySize = 1;
            depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDescription.SampleDesc.Count = 1;
            depthDescription.Usage = D3D11_USAGE_DEFAULT;
            depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            check(device_->CreateTexture2D(&depthDescription, nullptr, &depthBuffer_), "Could not create the depth buffer.");
            check(device_->CreateDepthStencilView(depthBuffer_.Get(), nullptr, &depthView_), "Could not create the depth view.");
            context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), depthView_.Get());
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(width_);
            viewport.Height = static_cast<float>(height_);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            context_->RSSetViewports(1, &viewport);
        }

        void update(float deltaTime)
        {
            const float forward = ((GetAsyncKeyState('W') & 0x8000) ? 1.0f : 0.0f) -
                                  ((GetAsyncKeyState('S') & 0x8000) ? 1.0f : 0.0f);
            const float strafe = ((GetAsyncKeyState('D') & 0x8000) ? 1.0f : 0.0f) -
                                 ((GetAsyncKeyState('A') & 0x8000) ? 1.0f : 0.0f);
            const float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 17.0f : 8.0f;
            const float distance = speed * deltaTime;
            cameraX_ += (std::sin(yaw_) * forward + std::cos(yaw_) * strafe) * distance;
            cameraZ_ += (std::cos(yaw_) * forward - std::sin(yaw_) * strafe) * distance;
            cameraX_ = std::clamp(cameraX_, -400.0f, 400.0f);
            cameraZ_ = std::clamp(cameraZ_, -90.0f, 3850.0f);
            if (!grounded_)
            {
                verticalVelocity_ -= 19.0f * deltaTime;
                jumpHeight_ += verticalVelocity_ * deltaTime;
                if (jumpHeight_ <= 0.0f)
                {
                    jumpHeight_ = 0.0f;
                    verticalVelocity_ = 0.0f;
                    grounded_ = true;
                }
            }
            time_ += deltaTime;
        }

        void jump()
        {
            if (!grounded_)
                return;
            verticalVelocity_ = 7.5f;
            grounded_ = false;
        }

        void toggleFullscreen(HWND window)
        {
            if (!fullscreen_)
            {
                windowedStyle_ = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
                if (!GetWindowRect(window, &windowedBounds_))
                    throw std::runtime_error("Could not save the window size before entering fullscreen.");

                MONITORINFO monitor{ sizeof(monitor) };
                const HMONITOR display = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
                if (!GetMonitorInfoW(display, &monitor))
                    throw std::runtime_error("Could not determine the display bounds for fullscreen.");

                SetLastError(ERROR_SUCCESS);
                if (SetWindowLongPtrW(window, GWL_STYLE,
                    static_cast<LONG_PTR>((windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP)) == 0 &&
                    GetLastError() != ERROR_SUCCESS)
                    throw std::runtime_error("Could not switch the window to fullscreen style.");

                if (!SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                    monitor.rcMonitor.right - monitor.rcMonitor.left,
                    monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                    SWP_FRAMECHANGED | SWP_NOOWNERZORDER))
                {
                    SetWindowLongPtrW(window, GWL_STYLE, windowedStyle_);
                    throw std::runtime_error("Could not resize the window to fullscreen.");
                }
                fullscreen_ = true;
                return;
            }

            SetLastError(ERROR_SUCCESS);
            if (SetWindowLongPtrW(window, GWL_STYLE, windowedStyle_) == 0 &&
                GetLastError() != ERROR_SUCCESS)
                throw std::runtime_error("Could not restore the windowed style.");

            if (!SetWindowPos(window, HWND_NOTOPMOST, windowedBounds_.left, windowedBounds_.top,
                windowedBounds_.right - windowedBounds_.left,
                windowedBounds_.bottom - windowedBounds_.top,
                SWP_FRAMECHANGED | SWP_NOOWNERZORDER))
                throw std::runtime_error("Could not restore the windowed size.");
            fullscreen_ = false;
        }

        void handleMouseFocus(HWND window, bool focused)
        {
            if (mouseCaptured_ == focused)
                return;
            mouseCaptured_ = focused;
            if (focused)
            {
                RAWINPUTDEVICE mouse{};
                mouse.usUsagePage = 0x01;
                mouse.usUsage = 0x02;
                mouse.dwFlags = 0;
                mouse.hwndTarget = window;
                if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
                {
                    mouseCaptured_ = false;
                    throw std::runtime_error("Could not enable raw mouse input.");
                }
                ignoreNextMouseInput_ = true;
                SetCapture(window);
                ShowCursor(FALSE);
            }
            else
            {
                if (GetCapture() == window)
                    ReleaseCapture();
                ShowCursor(TRUE);
            }
        }

        void handleRawMouse(LPARAM inputHandle)
        {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(inputHandle), RID_INPUT, &input,
                                &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
                throw std::runtime_error("Could not read mouse input.");
            if (input.header.dwType != RIM_TYPEMOUSE)
                return;
            if (ignoreNextMouseInput_)
            {
                ignoreNextMouseInput_ = false;
                return;
            }
            yaw_ += input.data.mouse.lLastX * mouseSensitivity_;
            pitch_ = std::clamp(pitch_ - input.data.mouse.lLastY * mouseSensitivity_, -0.65f, 0.85f);
        }

        void resetView()
        {
            cameraX_ = 0.0f;
            cameraZ_ = -12.0f;
            yaw_ = 0.0f;
            pitch_ = -0.035f;
        }

        void render()
        {
            const float clear[] = { 0.13f, 0.20f, 0.31f, 1.0f };
            context_->ClearRenderTargetView(renderTarget_.Get(), clear);
            context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), depthView_.Get());

            const float cameraHeight = groundHeight(cameraX_, cameraZ_) + 1.75f + jumpHeight_;
            const XMVECTOR eye = XMVectorSet(cameraX_, cameraHeight, cameraZ_, 1.0f);
            const float cosPitch = std::cos(pitch_);
            const XMVECTOR forward = XMVectorSet(std::sin(yaw_) * cosPitch, std::sin(pitch_),
                                                  std::cos(yaw_) * cosPitch, 0.0f);
            const XMMATRIX view = XMMatrixLookToLH(eye, forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
            const XMMATRIX projection = XMMatrixPerspectiveFovLH(Pi / 3.0f,
                static_cast<float>(width_) / static_cast<float>(height_), 0.08f, 3200.0f);
            const XMMATRIX viewProjection = view * projection;

            const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
            const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
            SkyConstants sky{};
            XMStoreFloat3(&sky.forward, forward);
            XMStoreFloat3(&sky.right, right);
            XMStoreFloat3(&sky.up, up);
            sky.tanHalfFov = std::tan(Pi / 6.0f);
            sky.aspectRatio = static_cast<float>(width_) / static_cast<float>(height_);
            context_->UpdateSubresource(skyBuffer_.Get(), 0, nullptr, &sky, 0, 0);

            drawSky();
            SceneConstants scene{};
            scene.viewProjection = viewProjection;
            scene.cameraPosition = XMFLOAT3(cameraX_, cameraHeight, cameraZ_);
            scene.time = time_;
            context_->UpdateSubresource(sceneBuffer_.Get(), 0, nullptr, &scene, 0, 0);
            ID3D11Buffer* sceneBuffers[] = { sceneBuffer_.Get() };
            context_->VSSetConstantBuffers(0, 1, sceneBuffers);
            context_->PSSetConstantBuffers(0, 1, sceneBuffers);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->IASetInputLayout(sceneLayout_.Get());
            context_->VSSetShader(sceneVertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(scenePixelShader_.Get(), nullptr, 0);
            drawMesh(ground_, XMMatrixIdentity(), XMFLOAT4(1, 1, 1, 1));

            for (const Portal& portal : portals_)
            {
                const float halfWidth = 5.5f * portal.scale;
                const float leftGround = groundHeight(portal.position.x - halfWidth, portal.position.z);
                const float rightGround = groundHeight(portal.position.x + halfWidth, portal.position.z);
                const float ground = std::min({ leftGround, rightGround,
                    groundHeight(portal.position.x, portal.position.z) }) - 0.02f;
                const XMMATRIX transform = XMMatrixScaling(portal.scale, portal.scale, portal.scale) *
                    XMMatrixTranslation(portal.position.x, ground, portal.position.z);
                drawMesh(arch_, transform, XMFLOAT4(1, 1, 1, 1));
                drawMesh(portalLeg_, XMMatrixTranslation(-5.5f, 5.5f, 0.0f) * transform, XMFLOAT4(1, 1, 1, 1));
                drawMesh(portalLeg_, XMMatrixTranslation(5.5f, 5.5f, 0.0f) * transform, XMFLOAT4(1, 1, 1, 1));
                drawMesh(portalGlow_, XMMatrixTranslation(0, 0, -1.05f) * transform,
                         XMFLOAT4(1, 0.68f, 0.36f, 1));
                drawMesh(portalGlowLeg_, XMMatrixTranslation(-4.84f, 5.2f, -1.35f) * transform,
                         XMFLOAT4(1, 0.70f, 0.37f, 1));
                drawMesh(portalGlowLeg_, XMMatrixTranslation(4.84f, 5.2f, -1.35f) * transform,
                         XMFLOAT4(1, 0.70f, 0.37f, 1));
            }

            for (const Clock& clock : clocks_)
            {
                const float ground = groundHeight(clock.position.x, clock.position.z);
                const XMMATRIX base = XMMatrixScaling(clock.scale, clock.scale, clock.scale) *
                    XMMatrixTranslation(clock.position.x, ground, clock.position.z);
                drawMesh(clockTower_, base, XMFLOAT4(1, 1, 1, 1));
                const XMMATRIX dial = XMMatrixScaling(5.15f, 5.15f, 0.42f) *
                    XMMatrixTranslation(0, 30.0f, -0.58f) * base;
                drawMesh(clockRim_, dial, XMFLOAT4(1, 1, 1, 1));
                drawMesh(clockFace_, XMMatrixTranslation(0, 30.0f, -0.58f) * base, XMFLOAT4(1, 1, 1, 1));
                drawMesh(clockHourHand_,
                    XMMatrixRotationZ(-time_ * 0.055f - clock.phase) *
                    XMMatrixTranslation(0, 30.0f, -1.16f) * base, XMFLOAT4(1, 1, 1, 1));
                drawMesh(clockMinuteHand_,
                    XMMatrixRotationZ(-time_ * 0.66f - clock.phase * 4.0f) *
                    XMMatrixTranslation(0, 30.0f, -1.18f) * base, XMFLOAT4(1, 1, 1, 1));
                drawMesh(clockHub_,
                    XMMatrixScaling(0.52f, 0.52f, 0.60f) *
                    XMMatrixTranslation(0, 30.0f, -1.30f) * base, XMFLOAT4(1, 1, 1, 1));
            }

            for (const Turbine& turbine : turbines_)
            {
                const float ground = groundHeight(turbine.position.x, turbine.position.z);
                const XMMATRIX base = XMMatrixScaling(turbine.scale, turbine.scale, turbine.scale) *
                    XMMatrixTranslation(turbine.position.x, ground, turbine.position.z);
                drawMesh(fanFrame_, base, XMFLOAT4(1, 1, 1, 1));
                drawMesh(fanGrille_, XMMatrixTranslation(0.0f, 0.0f, -0.83f) * base, XMFLOAT4(1, 1, 1, 1));
                const XMMATRIX rotor = XMMatrixRotationZ(time_ * 0.78f + turbine.phase) *
                    XMMatrixTranslation(0.0f, 7.2f, -1.0f) * base;
                drawMesh(turbineBlade_, rotor, XMFLOAT4(1, 1, 1, 1));
                drawMesh(hub_, XMMatrixScaling(0.48f, 0.48f, 0.48f) *
                    XMMatrixTranslation(0.0f, 7.2f, -1.18f) * base, XMFLOAT4(1, 1, 1, 1));
            }

            drawGrass(viewProjection);
            drawMesh(flowers_, XMMatrixIdentity(), XMFLOAT4(1, 1, 1, 1));
            for (const XMFLOAT4& rock : rocks_)
            {
                const XMMATRIX world = XMMatrixScaling(rock.w, rock.w * 0.64f, rock.w * 0.82f) *
                    XMMatrixTranslation(rock.x, groundHeight(rock.x, rock.z) + rock.w * 0.63f, rock.z);
                drawMesh(rock_, world, XMFLOAT4(1, 1, 1, 1));
            }
            drawBubbles();
            swapChain_->Present(1, 0);
        }

    private:
        static void check(HRESULT result, const char* message)
        {
            if (FAILED(result))
                throw std::runtime_error(message);
        }

        ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target)
        {
            ComPtr<ID3DBlob> bytecode;
            ComPtr<ID3DBlob> errors;
            const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &bytecode, &errors);
            if (FAILED(result))
            {
                const char* text = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Unknown shader compilation error.";
                OutputDebugStringA(text);
                throw std::runtime_error(text);
            }
            return bytecode;
        }

        void createShaders()
        {
            auto vertex = compile(SceneShader, "VSMain", "vs_4_0");
            auto pixel = compile(SceneShader, "PSMain", "ps_4_0");
            check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &sceneVertexShader_),
                  "Could not create the scene vertex shader.");
            check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &scenePixelShader_),
                  "Could not create the scene pixel shader.");
            auto bubblePixel = compile(BubblePixelShader, "PSBubble", "ps_4_0");
            check(device_->CreatePixelShader(bubblePixel->GetBufferPointer(), bubblePixel->GetBufferSize(), nullptr, &bubblePixelShader_),
                  "Could not create the bubble pixel shader.");
            const D3D11_INPUT_ELEMENT_DESC sceneElements[] =
            {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
            };
            check(device_->CreateInputLayout(sceneElements, ARRAYSIZE(sceneElements), vertex->GetBufferPointer(),
                vertex->GetBufferSize(), &sceneLayout_), "Could not create the scene input layout.");

            auto grassVertex = compile(GrassShader, "VSGrass", "vs_4_0");
            auto grassPixel = compile(GrassShader, "PSGrass", "ps_4_0");
            check(device_->CreateVertexShader(grassVertex->GetBufferPointer(), grassVertex->GetBufferSize(), nullptr, &grassVertexShader_),
                  "Could not create the grass vertex shader.");
            check(device_->CreatePixelShader(grassPixel->GetBufferPointer(), grassPixel->GetBufferSize(), nullptr, &grassPixelShader_),
                  "Could not create the grass pixel shader.");
            const D3D11_INPUT_ELEMENT_DESC grassElements[] =
            {
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "NORMAL", 1, DXGI_FORMAT_R32G32B32_FLOAT, 1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 1, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1 }
            };
            check(device_->CreateInputLayout(grassElements, ARRAYSIZE(grassElements), grassVertex->GetBufferPointer(),
                grassVertex->GetBufferSize(), &grassLayout_), "Could not create the grass input layout.");
            auto skyVertex = compile(SkyShader, "VSSky", "vs_4_0");
            auto skyPixel = compile(SkyShader, "PSSky", "ps_4_0");
            check(device_->CreateVertexShader(skyVertex->GetBufferPointer(), skyVertex->GetBufferSize(), nullptr, &skyVertexShader_),
                  "Could not create the sky vertex shader.");
            check(device_->CreatePixelShader(skyPixel->GetBufferPointer(), skyPixel->GetBufferSize(), nullptr, &skyPixelShader_),
                  "Could not create the sky pixel shader.");

            D3D11_BUFFER_DESC constantDescription{};
            constantDescription.ByteWidth = sizeof(SceneConstants);
            constantDescription.Usage = D3D11_USAGE_DEFAULT;
            constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check(device_->CreateBuffer(&constantDescription, nullptr, &sceneBuffer_), "Could not create the scene constants buffer.");
            constantDescription.ByteWidth = sizeof(ObjectConstants);
            check(device_->CreateBuffer(&constantDescription, nullptr, &objectBuffer_), "Could not create the object constants buffer.");
            constantDescription.ByteWidth = sizeof(SkyConstants);
            check(device_->CreateBuffer(&constantDescription, nullptr, &skyBuffer_), "Could not create the sky constants buffer.");

            D3D11_DEPTH_STENCIL_DESC depthState{};
            depthState.DepthEnable = FALSE;
            depthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depthState.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            ComPtr<ID3D11DepthStencilState> skyDepthState;
            check(device_->CreateDepthStencilState(&depthState, &skyDepthState), "Could not create the sky depth state.");
            skyDepthState_ = skyDepthState;
            depthState.DepthEnable = TRUE;
            depthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depthState.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            ComPtr<ID3D11DepthStencilState> transparentDepthState;
            check(device_->CreateDepthStencilState(&depthState, &transparentDepthState),
                  "Could not create the transparent depth state.");
            transparentDepthState_ = transparentDepthState;

            D3D11_BLEND_DESC blendDescription{};
            blendDescription.RenderTarget[0].BlendEnable = TRUE;
            blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ComPtr<ID3D11BlendState> bubbleBlendState;
            check(device_->CreateBlendState(&blendDescription, &bubbleBlendState),
                  "Could not create the bubble blend state.");
            bubbleBlendState_ = bubbleBlendState;
            D3D11_RASTERIZER_DESC rasterizerDescription{};
            rasterizerDescription.FillMode = D3D11_FILL_SOLID;
            rasterizerDescription.CullMode = D3D11_CULL_NONE;
            rasterizerDescription.DepthClipEnable = TRUE;
            check(device_->CreateRasterizerState(&rasterizerDescription, &rasterizerState_),
                  "Could not create the scene rasterizer.");
            context_->RSSetState(rasterizerState_.Get());
        }

        Mesh createMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
        {
            Mesh mesh;
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = vertices.data();
            check(device_->CreateBuffer(&description, &data, &mesh.vertices), "Could not create a mesh vertex buffer.");
            description.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
            description.BindFlags = D3D11_BIND_INDEX_BUFFER;
            data.pSysMem = indices.data();
            check(device_->CreateBuffer(&description, &data, &mesh.indices), "Could not create a mesh index buffer.");
            mesh.indexCount = static_cast<UINT>(indices.size());
            return mesh;
        }

        static float terrainHeight(float x, float z)
        {
            const auto hill = [x, z](float centerX, float centerZ, float radiusX, float radiusZ, float height)
            {
                const float dx = (x - centerX) / radiusX;
                const float dz = (z - centerZ) / radiusZ;
                return height * std::exp(-(dx * dx + dz * dz) * 1.7f);
            };
            const float broadRolls =
                2.2f * std::sin(x * 0.013f + z * 0.003f) *
                std::cos(z * 0.005f - x * 0.008f) +
                1.3f * std::sin(z * 0.007f + std::sin(x * 0.012f) * 1.8f) +
                0.48f * std::sin(x * 0.087f + z * 0.036f) *
                std::sin(z * 0.063f - x * 0.041f);
            return broadRolls +
                hill(-55.0f, 70.0f, 52.0f, 66.0f, 12.0f) +
                hill(71.0f, 128.0f, 58.0f, 73.0f, 14.0f) +
                hill(-116.0f, 311.0f, 88.0f, 109.0f, 18.0f) +
                hill(142.0f, 466.0f, 103.0f, 136.0f, 21.0f) +
                hill(-222.0f, 823.0f, 148.0f, 196.0f, 28.0f) +
                hill(258.0f, 1112.0f, 168.0f, 215.0f, 33.0f) +
                hill(-88.0f, 1571.0f, 205.0f, 263.0f, 37.0f) +
                hill(204.0f, 2146.0f, 238.0f, 314.0f, 47.0f) +
                hill(-289.0f, 2718.0f, 255.0f, 344.0f, 56.0f) +
                hill(56.0f, 3360.0f, 310.0f, 395.0f, 62.0f);
        }

        float groundHeight(float x, float z) const
        {
            constexpr int sideX = 401;
            constexpr int sideZ = 1101;
            constexpr float extentX = 420.0f;
            constexpr float minZ = -100.0f;
            constexpr float maxZ = 4000.0f;
            const float gx = std::clamp((x + extentX) / (extentX * 2.0f) * (sideX - 1),
                                        0.0f, static_cast<float>(sideX - 1) - 0.0001f);
            const float gz = std::clamp((z - minZ) / (maxZ - minZ) * (sideZ - 1),
                                        0.0f, static_cast<float>(sideZ - 1) - 0.0001f);
            const int cellX = static_cast<int>(gx);
            const int cellZ = static_cast<int>(gz);
            const float u = gx - cellX;
            const float v = gz - cellZ;
            const auto sample = [&](int sx, int sz)
            {
                return terrainSamples_[static_cast<size_t>(sz) * sideX + sx];
            };
            const float a = sample(cellX, cellZ);
            const float b = sample(cellX + 1, cellZ);
            const float c = sample(cellX, cellZ + 1);
            const float d = sample(cellX + 1, cellZ + 1);
            return u + v <= 1.0f ? a * (1.0f - u - v) + b * u + c * v :
                b * (1.0f - v) + c * (1.0f - u) + d * (u + v - 1.0f);
        }

        XMFLOAT3 groundNormal(float x, float z) const
        {
            constexpr float sample = 0.18f;
            XMFLOAT3 normal(
                groundHeight(x - sample, z) - groundHeight(x + sample, z),
                sample * 2.0f,
                groundHeight(x, z - sample) - groundHeight(x, z + sample));
            XMStoreFloat3(&normal, XMVector3Normalize(XMLoadFloat3(&normal)));
            return normal;
        }

        static XMFLOAT3 terrainNormal(float x, float z)
        {
            constexpr float sample = 0.8f;
            XMFLOAT3 normal(
                terrainHeight(x - sample, z) - terrainHeight(x + sample, z),
                2.0f * sample,
                terrainHeight(x, z - sample) - terrainHeight(x, z + sample));
            XMStoreFloat3(&normal, XMVector3Normalize(XMLoadFloat3(&normal)));
            return normal;
        }

        void createBuffers()
        {
            const GrassVertex singleBlade[] =
            {
                {{-1.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f}}, {{0.68f, 0.42f, 0.0f}},
                {{-1.0f, 0.0f, 0.0f}}, {{0.68f, 0.42f, 0.0f}}, {{0.0f, 1.0f, 0.0f}}
            };
            std::vector<GrassVertex> blades;
            blades.reserve(ARRAYSIZE(singleBlade) * 6);
            for (int direction = 0; direction < 6; ++direction)
            {
                const float angle = Pi * static_cast<float>(direction) / 6.0f;
                for (GrassVertex vertex : singleBlade)
                {
                    vertex.shape.z = angle;
                    blades.push_back(vertex);
                }
            }
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = static_cast<UINT>(blades.size() * sizeof(GrassVertex));
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = blades.data();
            check(device_->CreateBuffer(&description, &data, &grassShape_), "Could not create the grass blade mesh.");

            std::mt19937 random(17);
            std::uniform_real_distribution<float> jitter(-0.18f, 0.18f);
            std::uniform_real_distribution<float> height(0.72f, 1.38f);
            std::uniform_real_distribution<float> width(0.028f, 0.045f);
            std::uniform_real_distribution<float> hue(-0.07f, 0.10f);
            std::uniform_real_distribution<float> phase(0.0f, 6.28f);
            std::vector<GrassInstance> grass;
            constexpr size_t nearCount = 2400000;
            constexpr size_t midCount = 2200000;
            constexpr size_t farCount = 1600000;
            grass.reserve(nearCount + midCount + farCount);
            for (size_t i = 0; i < nearCount + midCount + farCount; ++i)
            {
                const float variation = hue(random);
                float x;
                float z;
                if (i < nearCount)
                {
                    constexpr int columns = 1500;
                    const int column = static_cast<int>(i % columns);
                    const int row = static_cast<int>(i / columns);
                    x = -400.0f + (column + 0.5f + jitter(random)) * (800.0f / columns);
                    z = -100.0f + (row + 0.5f + jitter(random)) * (950.0f / 1600.0f);
                }
                else if (i < nearCount + midCount)
                {
                    constexpr int columns = 1100;
                    const size_t localIndex = i - nearCount;
                    const int column = static_cast<int>(localIndex % columns);
                    const int row = static_cast<int>(localIndex / columns);
                    x = -400.0f + (column + 0.5f + jitter(random)) * (800.0f / columns);
                    z = 800.0f + (row + 0.5f + jitter(random)) * (1650.0f / 2000.0f);
                }
                else
                {
                    constexpr int columns = 900;
                    const size_t localIndex = i - nearCount - midCount;
                    const int column = static_cast<int>(localIndex % columns);
                    const int row = static_cast<int>(localIndex / columns);
                    x = -420.0f + (column + 0.5f + jitter(random)) * (840.0f / columns);
                    z = 2400.0f + (row + 0.5f + jitter(random)) * (1600.0f / 1778.0f);
                }
                grass.push_back({ XMFLOAT3(x, groundHeight(x, z) + 0.003f, z),
                groundNormal(x, z),
                XMFLOAT2(height(random), width(random)), phase(random),
                    XMFLOAT4(0.28f + variation, 0.49f + variation, 0.16f + variation * 0.58f, 1.0f) });
            }
            description.ByteWidth = static_cast<UINT>(grass.size() * sizeof(GrassInstance));
            data.pSysMem = grass.data();
            check(device_->CreateBuffer(&description, &data, &grassInstances_), "Could not create the meadow grass instances.");
            grassCount_ = static_cast<UINT>(grass.size());
            nearGrassCount_ = static_cast<UINT>(nearCount);
            midGrassCount_ = static_cast<UINT>(midCount);
        }

        void createWorld()
        {
            ground_ = makeGround();
            const XMFLOAT4 stone(0.82f, 0.77f, 0.68f, 1);
            arch_ = makeArch(5.5f, 0.92f, 11.0f, stone);
            portalLeg_ = makeBox(XMFLOAT3(0, 0, 0), XMFLOAT3(1.86f, 11.0f, 2.45f), stone);
            portalGlow_ = makeArch(4.58f, 0.11f, 10.96f, XMFLOAT4(1.0f, 0.56f, 0.30f, 1));
            portalGlowLeg_ = makeBox(XMFLOAT3(0, 0, 0), XMFLOAT3(0.18f, 10.85f, 0.22f),
                                     XMFLOAT4(1.0f, 0.62f, 0.33f, 1));
            turbineBlade_ = makeTurbineRotor();
            hub_ = makeSphere(1.0f, 16, 10, XMFLOAT4(0.86f, 0.78f, 0.58f, 1));
            fanFrame_ = makeFanFrame();
            fanGrille_ = makeFanGrille();
            clockTower_ = makeClockTower();
            clockRim_ = makeSphere(1.0f, 32, 20, XMFLOAT4(0.34f, 0.35f, 0.33f, 1));
            clockFace_ = makeClockFace();
            clockHourHand_ = makeBox(XMFLOAT3(0, 1.05f, 0), XMFLOAT3(0.23f, 2.25f, 0.12f),
                                     XMFLOAT4(0.22f, 0.24f, 0.23f, 1));
            clockMinuteHand_ = makeBox(XMFLOAT3(0, 1.55f, 0), XMFLOAT3(0.15f, 3.25f, 0.14f),
                                       XMFLOAT4(0.24f, 0.25f, 0.23f, 1));
            clockHub_ = makeSphere(0.24f, 12, 8, XMFLOAT4(0.46f, 0.35f, 0.25f, 1));
            portals_.push_back({ XMFLOAT3(0, 0, 72), 1.50f });
            portals_.push_back({ XMFLOAT3(170, 0, 530), 8.5f });
            std::mt19937 portalRandom(2026);
            std::uniform_real_distribution<float> portalX(-360.0f, 360.0f);
            std::uniform_real_distribution<float> smallScale(0.34f, 0.78f);
            std::uniform_real_distribution<float> mediumScale(0.82f, 1.45f);
            for (int row = 0; row < 76; ++row)
            {
                const float z = 135.0f + row * 49.0f;
                const float scale = row % 43 == 24 ? 13.0f :
                                    row % 29 == 12 ? 9.0f :
                                    row % 19 == 7 ? 3.25f :
                                    row % 11 == 4 ? 2.25f :
                                    row % 3 == 0 ? smallScale(portalRandom) : mediumScale(portalRandom);
                const float x = std::clamp(row % 5 == 0 ? portalX(portalRandom) * 0.45f : portalX(portalRandom),
                                           -400.0f + 5.5f * scale, 400.0f - 5.5f * scale);
                portals_.push_back({ XMFLOAT3(x, 0, z), scale });
                if (row % 13 == 6)
                {
                    const float secondaryScale = row % 26 == 6 ? 1.9f : smallScale(portalRandom);
                    const float secondaryX = std::clamp(-x * 0.62f + portalX(portalRandom) * 0.12f,
                        -400.0f + 5.5f * secondaryScale, 400.0f - 5.5f * secondaryScale);
                    portals_.push_back({ XMFLOAT3(secondaryX, 0, z + 24.0f), secondaryScale });
                }
            }
            std::mt19937 clockRandom(4031);
            std::uniform_real_distribution<float> clockX(-355.0f, 355.0f);
            std::uniform_real_distribution<float> clockScale(0.8f, 1.65f);
            std::uniform_real_distribution<float> clockPhase(0.0f, 6.28f);
            clocks_.push_back({ XMFLOAT3(105, 0, 320), 2.2f, 0.0f });
            for (int i = 0; i < 17; ++i)
            {
                const float z = 160.0f + static_cast<float>(i) * 235.0f;
                const float scale = i % 6 == 3 ? 3.8f : clockScale(clockRandom);
                clocks_.push_back({ XMFLOAT3(clockX(clockRandom), 0, z), scale, clockPhase(clockRandom) });
            }
            std::mt19937 bubbleRandom(9137);
            std::uniform_real_distribution<float> bubbleX(-390.0f, 390.0f);
            std::uniform_real_distribution<float> bubbleZ(-70.0f, 3940.0f);
            std::uniform_real_distribution<float> bubbleSpeed(0.55f, 1.05f);
            std::uniform_real_distribution<float> bubbleScale(0.45f, 1.1f);
            std::uniform_real_distribution<float> bubblePhase(0.0f, 1.0f);
            for (int i = 0; i < 120; ++i)
            {
                const float x = i < 14 ? -130.0f + static_cast<float>(i) * 20.0f : bubbleX(bubbleRandom);
                const float z = i < 14 ? 35.0f + static_cast<float>(i) * 24.0f :
                                i < 32 ? -70.0f + static_cast<float>(i) * 21.0f : bubbleZ(bubbleRandom);
                bubbles_.push_back({ XMFLOAT2(x, z), groundHeight(x, z),
                    bubbleSpeed(bubbleRandom), i < 14 ? 1.7f + (i % 3) * 0.22f : bubbleScale(bubbleRandom),
                    bubblePhase(bubbleRandom) });
            }
            turbines_ = {
                { XMFLOAT3(-22, 0, 18), 1.12f, 0.1f },
                { XMFLOAT3(24, 0, 51), 0.88f, 1.4f },
                { XMFLOAT3(-38, 0, 89), 0.77f, 2.7f },
                { XMFLOAT3(42, 0, 139), 0.67f, 0.7f },
                { XMFLOAT3(-76, 0, 382), 1.25f, 1.1f },
                { XMFLOAT3(131, 0, 728), 1.55f, 0.3f },
                { XMFLOAT3(-198, 0, 1285), 1.3f, 1.8f },
                { XMFLOAT3(245, 0, 1990), 1.7f, 0.9f },
                { XMFLOAT3(-281, 0, 2765), 1.45f, 2.5f }
            };
            flowers_ = makeFlowerPatch();
            rock_ = makeSphere(1.0f, 12, 8, XMFLOAT4(0.39f, 0.40f, 0.36f, 1));
            bubbleMesh_ = makeSphere(1.0f, 24, 16, XMFLOAT4(0.8f, 0.9f, 0.9f, 1));
            rocks_ = {
                XMFLOAT4(-12, 0, 13, 1.8f), XMFLOAT4(18, 0, 25, 1.15f),
                XMFLOAT4(-34, 0, 47, 2.4f), XMFLOAT4(39, 0, 73, 2.0f),
                XMFLOAT4(-56, 0, 118, 2.8f), XMFLOAT4(67, 0, 164, 2.5f),
                XMFLOAT4(-98, 0, 264, 3.2f), XMFLOAT4(121, 0, 392, 3.4f),
                XMFLOAT4(-154, 0, 681, 4.0f), XMFLOAT4(206, 0, 945, 3.0f),
                XMFLOAT4(-238, 0, 1572, 4.5f), XMFLOAT4(295, 0, 2280, 3.8f)
            };
        }

        Mesh makeGround()
        {
            constexpr int sideX = 401;
            constexpr int sideZ = 1101;
            constexpr float extent = 420.0f;
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            vertices.reserve(sideX * sideZ);
            terrainSamples_.resize(sideX * sideZ);
            for (int z = 0; z < sideZ; ++z)
            {
                for (int x = 0; x < sideX; ++x)
                {
                    const float px = (static_cast<float>(x) / (sideX - 1) * 2.0f - 1.0f) * extent;
                    const float pz = -100.0f + static_cast<float>(z) / (sideZ - 1) * 4100.0f;
                    const float py = terrainHeight(px, pz);
                    terrainSamples_[static_cast<size_t>(z) * sideX + x] = py;
                    const float mix = 0.025f * std::sin(px * 0.13f + pz * 0.09f);
                    const XMFLOAT3 normal = terrainNormal(px, pz);
                    const float sunlitSlope = std::max(normal.x * -0.63f + normal.z * 0.71f, 0.0f);
                    const float gold = sunlitSlope * 0.11f;
                    vertices.push_back({ XMFLOAT3(px, py, pz), normal,
                        XMFLOAT4(0.20f + mix + gold, 0.36f + mix + gold * 0.72f,
                                 0.16f + mix * 0.55f + gold * 0.36f, 1) });
                }
            }
            for (int z = 0; z < sideZ - 1; ++z)
            {
                for (int x = 0; x < sideX - 1; ++x)
                {
                    const uint32_t a = static_cast<uint32_t>(z * sideX + x);
                    const uint32_t b = a + 1;
                    const uint32_t c = a + sideX;
                    const uint32_t d = c + 1;
                    indices.insert(indices.end(), { a, c, b, b, c, d });
                }
            }
            return createMesh(vertices, indices);
        }

        static void addTriangle(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                               XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT4 color)
        {
            XMVECTOR va = XMLoadFloat3(&a);
            XMVECTOR vb = XMLoadFloat3(&b);
            XMVECTOR vc = XMLoadFloat3(&c);
            XMFLOAT3 normal;
            XMStoreFloat3(&normal, XMVector3Normalize(XMVector3Cross(vb - va, vc - va)));
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.push_back({ a, normal, color });
            vertices.push_back({ b, normal, color });
            vertices.push_back({ c, normal, color });
            indices.insert(indices.end(), { base, base + 1, base + 2 });
        }

        static void addBox(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                           XMFLOAT3 center, XMFLOAT3 dimensions, XMFLOAT4 color, float rotation = 0.0f)
        {
            const float cosine = std::cos(rotation);
            const float sine = std::sin(rotation);
            const auto corner = [&](float x, float y, float z)
            {
                return XMFLOAT3(center.x + x * cosine - y * sine,
                                center.y + x * sine + y * cosine,
                                center.z + z);
            };
            const float x = dimensions.x * 0.5f;
            const float y = dimensions.y * 0.5f;
            const float z = dimensions.z * 0.5f;
            const auto face = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d)
            {
                addTriangle(vertices, indices, a, b, c, color);
                addTriangle(vertices, indices, a, c, d, color);
            };
            face(corner(-x, -y, -z), corner(-x, y, -z), corner(x, y, -z), corner(x, -y, -z));
            face(corner(-x, -y, z), corner(x, -y, z), corner(x, y, z), corner(-x, y, z));
            face(corner(-x, y, -z), corner(-x, y, z), corner(x, y, z), corner(x, y, -z));
            face(corner(-x, -y, -z), corner(x, -y, -z), corner(x, -y, z), corner(-x, -y, z));
            face(corner(-x, -y, -z), corner(-x, -y, z), corner(-x, y, z), corner(-x, y, -z));
            face(corner(x, -y, -z), corner(x, y, -z), corner(x, y, z), corner(x, -y, z));
        }

        Mesh makeBox(XMFLOAT3 center, XMFLOAT3 dimensions, XMFLOAT4 color)
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            addBox(vertices, indices, center, dimensions, color);
            return createMesh(vertices, indices);
        }

        Mesh makeClockTower()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const XMFLOAT4 stone(0.61f, 0.59f, 0.54f, 1);
            addBox(vertices, indices, XMFLOAT3(-4.2f, 13.0f, 0), XMFLOAT3(1.0f, 26.0f, 1.5f), stone);
            addBox(vertices, indices, XMFLOAT3(4.2f, 13.0f, 0), XMFLOAT3(1.0f, 26.0f, 1.5f), stone);
            addBox(vertices, indices, XMFLOAT3(0, 25.7f, 0), XMFLOAT3(10.0f, 1.2f, 1.8f), stone);
            addBox(vertices, indices, XMFLOAT3(0, 0.45f, 0), XMFLOAT3(10.0f, 0.9f, 2.0f),
                   XMFLOAT4(0.43f, 0.43f, 0.40f, 1));
            return createMesh(vertices, indices);
        }

        Mesh makeClockFace()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            constexpr int segments = 48;
            const XMFLOAT3 center(0, 0, -0.75f);
            const XMFLOAT4 faceColor(0.76f, 0.72f, 0.64f, 1);
            for (int i = 0; i < segments; ++i)
            {
                const float a = 2.0f * Pi * static_cast<float>(i) / segments;
                const float b = 2.0f * Pi * static_cast<float>(i + 1) / segments;
                addTriangle(vertices, indices, center,
                    XMFLOAT3(std::sin(a) * 4.65f, std::cos(a) * 4.65f, -0.75f),
                    XMFLOAT3(std::sin(b) * 4.65f, std::cos(b) * 4.65f, -0.75f), faceColor);
            }
            const XMFLOAT4 tickColor(0.25f, 0.27f, 0.25f, 1);
            for (int i = 0; i < 12; ++i)
            {
                const float angle = 2.0f * Pi * static_cast<float>(i) / 12.0f;
                addBox(vertices, indices,
                    XMFLOAT3(std::sin(angle) * 3.82f, std::cos(angle) * 3.82f, -0.80f),
                    XMFLOAT3(0.16f, 0.62f, 0.08f), tickColor, -angle);
            }
            return createMesh(vertices, indices);
        }

        Mesh makeFanFrame()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const XMFLOAT4 concrete(0.78f, 0.75f, 0.68f, 1);
            addBox(vertices, indices, XMFLOAT3(-5.45f, 7.15f, 0),
                   XMFLOAT3(1.5f, 14.3f, 1.55f), concrete);
            addBox(vertices, indices, XMFLOAT3(5.45f, 7.15f, 0),
                   XMFLOAT3(1.5f, 14.3f, 1.55f), concrete);
            addBox(vertices, indices, XMFLOAT3(0, 13.55f, 0),
                   XMFLOAT3(12.4f, 1.55f, 1.55f), concrete);
            addBox(vertices, indices, XMFLOAT3(0, 0.78f, 0),
                   XMFLOAT3(12.4f, 1.55f, 1.55f), concrete);
            const XMFLOAT4 trim(0.79f, 0.73f, 0.59f, 1);
            addBox(vertices, indices, XMFLOAT3(0, 12.62f, -0.82f),
                   XMFLOAT3(9.5f, 0.10f, 0.12f), trim);
            return createMesh(vertices, indices);
        }

        Mesh makeFanGrille()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const XMFLOAT4 metal(0.36f, 0.36f, 0.34f, 1);
            constexpr float radius = 4.65f;
            constexpr int segments = 112;
            constexpr int sides = 8;
            for (int i = 0; i <= segments; ++i)
            {
                const float angle = 2.0f * Pi * static_cast<float>(i) / segments;
                for (int j = 0; j < sides; ++j)
                {
                    const float around = 2.0f * Pi * static_cast<float>(j) / sides;
                    const float radial = radius + 0.08f * std::cos(around);
                    const XMFLOAT3 normal(std::cos(angle) * std::cos(around),
                                          std::sin(angle) * std::cos(around), std::sin(around));
                    vertices.push_back({ XMFLOAT3(radial * std::cos(angle),
                        7.2f + radial * std::sin(angle), -0.86f + 0.08f * std::sin(around)), normal, metal });
                }
            }
            for (int i = 0; i < segments; ++i)
            {
                for (int j = 0; j < sides; ++j)
                {
                    const uint32_t a = static_cast<uint32_t>(i * sides + j);
                    const uint32_t b = static_cast<uint32_t>(i * sides + (j + 1) % sides);
                    const uint32_t c = a + sides;
                    const uint32_t d = static_cast<uint32_t>(i * sides + (j + 1) % sides + sides);
                    indices.insert(indices.end(), { a, b, c, b, d, c });
                }
            }
            for (int spoke = 0; spoke < 8; ++spoke)
            {
                const float angle = Pi * static_cast<float>(spoke) / 8.0f;
                addBox(vertices, indices, XMFLOAT3(0, 7.2f, -0.88f),
                       XMFLOAT3(0.055f, radius * 2.0f, 0.055f), metal, angle);
            }
            return createMesh(vertices, indices);
        }

        Mesh makeFlowerPatch()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            std::mt19937 random(431);
            std::uniform_real_distribution<float> across(-350.0f, 350.0f);
            std::uniform_real_distribution<float> downwind(-70.0f, 3500.0f);
            std::uniform_real_distribution<float> height(0.78f, 1.35f);
            std::uniform_real_distribution<float> size(0.12f, 0.22f);
            std::uniform_real_distribution<float> rotation(0.0f, 2.0f * Pi);
            std::uniform_int_distribution<int> petalColor(0, 3);
            const XMFLOAT4 petalColors[] = {
                XMFLOAT4(1.00f, 0.88f, 0.57f, 1), XMFLOAT4(0.98f, 0.73f, 0.78f, 1),
                XMFLOAT4(0.91f, 0.92f, 0.82f, 1), XMFLOAT4(0.83f, 0.78f, 0.96f, 1)
            };
            for (int i = 0; i < 4200; ++i)
            {
                const float x = across(random);
                const float z = downwind(random);
                const float ground = groundHeight(x, z);
                const float flowerHeight = height(random);
                const float radius = size(random);
                const float angle = rotation(random);
                const XMFLOAT4 petal = petalColors[petalColor(random)];
                addBox(vertices, indices, XMFLOAT3(x, ground + flowerHeight * 0.45f, z),
                       XMFLOAT3(0.018f, flowerHeight * 0.9f, 0.018f), XMFLOAT4(0.23f, 0.42f, 0.17f, 1));
                const XMFLOAT3 center(x, ground + flowerHeight, z);
                for (int leaf = 0; leaf < 5; ++leaf)
                {
                    const float petalAngle = angle + leaf * 2.0f * Pi / 5.0f;
                    const XMFLOAT3 outerA(x + std::cos(petalAngle - 0.70f) * radius,
                                          center.y, z + std::sin(petalAngle - 0.70f) * radius);
                    const XMFLOAT3 outerB(x + std::cos(petalAngle + 0.70f) * radius,
                                          center.y, z + std::sin(petalAngle + 0.70f) * radius);
                    const XMFLOAT3 inner(x + std::cos(petalAngle) * radius * 0.28f,
                                         center.y + 0.015f, z + std::sin(petalAngle) * radius * 0.28f);
                    addTriangle(vertices, indices, inner, outerA, outerB, petal);
                }
                const XMFLOAT3 centerTop(x, center.y + 0.02f, z);
                const float centerSize = radius * 0.22f;
                addTriangle(vertices, indices, centerTop,
                    XMFLOAT3(x - centerSize, center.y + 0.02f, z),
                    XMFLOAT3(x, center.y + 0.02f, z + centerSize), XMFLOAT4(1.0f, 0.76f, 0.28f, 1));
                addTriangle(vertices, indices, centerTop,
                    XMFLOAT3(x, center.y + 0.02f, z + centerSize),
                    XMFLOAT3(x + centerSize, center.y + 0.02f, z), XMFLOAT4(1.0f, 0.76f, 0.28f, 1));
                addTriangle(vertices, indices, centerTop,
                    XMFLOAT3(x + centerSize, center.y + 0.02f, z),
                    XMFLOAT3(x, center.y + 0.02f, z - centerSize), XMFLOAT4(1.0f, 0.76f, 0.28f, 1));
                addTriangle(vertices, indices, centerTop,
                    XMFLOAT3(x, center.y + 0.02f, z - centerSize),
                    XMFLOAT3(x - centerSize, center.y + 0.02f, z), XMFLOAT4(1.0f, 0.76f, 0.28f, 1));
            }
            return createMesh(vertices, indices);
        }

        Mesh makeArch(float radius, float tube, float springHeight, XMFLOAT4 color)
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            constexpr int segments = 64;
            const float depth = tube * 1.8f;
            for (int i = 0; i <= segments; ++i)
            {
                const float angle = Pi * static_cast<float>(i) / segments;
                const XMFLOAT3 center(std::cos(angle) * radius, springHeight + std::sin(angle) * radius, 0);
                const XMFLOAT3 radial(std::cos(angle), std::sin(angle), 0);
                const float front = -depth * 0.5f;
                const float back = depth * 0.5f;
                const auto point = [&](float offset, float z)
                {
                    return XMFLOAT3(center.x + radial.x * offset, center.y + radial.y * offset, z);
                };
                const XMFLOAT3 outward(radial.x, radial.y, 0);
                const XMFLOAT3 inward(-radial.x, -radial.y, 0);
                vertices.push_back({ point(tube, front), XMFLOAT3(0, 0, -1), color });
                vertices.push_back({ point(-tube, front), XMFLOAT3(0, 0, -1), color });
                vertices.push_back({ point(tube, back), XMFLOAT3(0, 0, 1), color });
                vertices.push_back({ point(-tube, back), XMFLOAT3(0, 0, 1), color });
                vertices.push_back({ point(tube, front), outward, color });
                vertices.push_back({ point(tube, back), outward, color });
                vertices.push_back({ point(-tube, front), inward, color });
                vertices.push_back({ point(-tube, back), inward, color });
            }
            for (int i = 0; i < segments; ++i)
            {
                const uint32_t a = static_cast<uint32_t>(i * 8);
                const uint32_t b = a + 8;
                indices.insert(indices.end(), { a, a + 1, b, a + 1, b + 1, b });
                indices.insert(indices.end(), { a + 2, b + 2, a + 3, a + 3, b + 2, b + 3 });
                indices.insert(indices.end(), { a + 4, b + 4, a + 5, a + 5, b + 4, b + 5 });
                indices.insert(indices.end(), { a + 6, a + 7, b + 6, a + 7, b + 7, b + 6 });
            }
            return createMesh(vertices, indices);
        }

        Mesh makeCylinder(float radius, float height, int sides, XMFLOAT4 color)
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            for (int y = 0; y <= 1; ++y)
            {
                for (int i = 0; i <= sides; ++i)
                {
                    const float angle = 2.0f * Pi * static_cast<float>(i) / sides;
                    const XMFLOAT3 normal(std::cos(angle), 0, std::sin(angle));
                    vertices.push_back({ XMFLOAT3(normal.x * radius, y * height, normal.z * radius), normal, color });
                }
            }
            for (int i = 0; i < sides; ++i)
            {
                const uint32_t a = static_cast<uint32_t>(i);
                indices.insert(indices.end(), { a, a + 1, a + sides + 1, a + 1, a + sides + 2, a + sides + 1 });
            }
            return createMesh(vertices, indices);
        }

        Mesh makeTurbineRotor()
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const XMFLOAT4 bronze(0.48f, 0.38f, 0.25f, 1);
            const XMFLOAT4 highlight(0.72f, 0.55f, 0.32f, 1);
            for (int blade = 0; blade < 5; ++blade)
            {
                const float angle = static_cast<float>(blade) * 2.0f * Pi / 5.0f;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                auto rotate = [c, s](float x, float y, float z)
                {
                    return XMFLOAT3(x * c - y * s, x * s + y * c, z);
                };
                const XMFLOAT3 rootLeft = rotate(-0.25f, 0.28f, 0.0f);
                const XMFLOAT3 rootRight = rotate(0.25f, 0.28f, 0.0f);
                const XMFLOAT3 innerLeft = rotate(-0.72f, 1.55f, 0.0f);
                const XMFLOAT3 innerRight = rotate(0.76f, 1.85f, 0.0f);
                const XMFLOAT3 outerLeft = rotate(-0.60f, 3.45f, 0.0f);
                const XMFLOAT3 outerRight = rotate(0.12f, 4.65f, 0.0f);
                const XMFLOAT3 tip = rotate(0.56f, 4.18f, 0.0f);
                addTriangle(vertices, indices, rootLeft, rootRight, innerRight, bronze);
                addTriangle(vertices, indices, rootLeft, innerRight, innerLeft, highlight);
                addTriangle(vertices, indices, innerLeft, innerRight, tip, bronze);
                addTriangle(vertices, indices, innerLeft, tip, outerLeft, highlight);
                addTriangle(vertices, indices, outerLeft, tip, outerRight, bronze);
                const XMFLOAT3 backLeft(rootLeft.x, rootLeft.y, 0.12f);
                const XMFLOAT3 backRight(rootRight.x, rootRight.y, 0.12f);
                const XMFLOAT3 backInnerLeft(innerLeft.x, innerLeft.y, 0.12f);
                const XMFLOAT3 backInnerRight(innerRight.x, innerRight.y, 0.12f);
                const XMFLOAT3 backOuterLeft(outerLeft.x, outerLeft.y, 0.12f);
                const XMFLOAT3 backOuterRight(outerRight.x, outerRight.y, 0.12f);
                const XMFLOAT3 backTip(tip.x, tip.y, 0.12f);
                addTriangle(vertices, indices, backLeft, backRight, backInnerRight, bronze);
                addTriangle(vertices, indices, backLeft, backInnerRight, backInnerLeft, highlight);
                addTriangle(vertices, indices, backInnerLeft, backInnerRight, backTip, bronze);
                addTriangle(vertices, indices, backInnerLeft, backTip, backOuterLeft, highlight);
                addTriangle(vertices, indices, backOuterLeft, backTip, backOuterRight, bronze);
            }
            return createMesh(vertices, indices);
        }

        Mesh makeSphere(float radius, int slices, int stacks, XMFLOAT4 color)
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            for (int stack = 0; stack <= stacks; ++stack)
            {
                const float phi = Pi * static_cast<float>(stack) / stacks;
                for (int slice = 0; slice <= slices; ++slice)
                {
                    const float theta = 2.0f * Pi * static_cast<float>(slice) / slices;
                    const XMFLOAT3 normal(std::sin(phi) * std::cos(theta), std::cos(phi),
                                          std::sin(phi) * std::sin(theta));
                    vertices.push_back({ XMFLOAT3(normal.x * radius, normal.y * radius, normal.z * radius), normal, color });
                }
            }
            for (int stack = 0; stack < stacks; ++stack)
            {
                for (int slice = 0; slice < slices; ++slice)
                {
                    const uint32_t a = static_cast<uint32_t>(stack * (slices + 1) + slice);
                    const uint32_t b = a + slices + 1;
                    indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            }
            return createMesh(vertices, indices);
        }

        void drawSky()
        {
            context_->OMSetDepthStencilState(skyDepthState_.Get(), 0);
            context_->IASetInputLayout(nullptr);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* constants = skyBuffer_.Get();
            context_->VSSetConstantBuffers(0, 1, &constants);
            context_->VSSetShader(skyVertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(skyPixelShader_.Get(), nullptr, 0);
            context_->Draw(3, 0);
            context_->OMSetDepthStencilState(nullptr, 0);
        }

        void drawMesh(const Mesh& mesh, XMMATRIX world, XMFLOAT4 tint)
        {
            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;
            ID3D11Buffer* vertexBuffer = mesh.vertices.Get();
            context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
            context_->IASetIndexBuffer(mesh.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ObjectConstants object{ world, tint };
            context_->UpdateSubresource(objectBuffer_.Get(), 0, nullptr, &object, 0, 0);
            ID3D11Buffer* objectBuffer = objectBuffer_.Get();
            context_->VSSetConstantBuffers(1, 1, &objectBuffer);
            context_->DrawIndexed(mesh.indexCount, 0, 0);
        }

        void drawGrass(const XMMATRIX& viewProjection)
        {
            GrassConstants scene{};
            scene.viewProjection = viewProjection;
            scene.cameraPosition = XMFLOAT3(cameraX_,
                groundHeight(cameraX_, cameraZ_) + 1.75f + jumpHeight_, cameraZ_);
            scene.time = time_;
            context_->UpdateSubresource(sceneBuffer_.Get(), 0, nullptr, &scene, 0, 0);
            ID3D11Buffer* constants = sceneBuffer_.Get();
            context_->VSSetConstantBuffers(0, 1, &constants);
            const UINT strides[] = { sizeof(GrassVertex), sizeof(GrassInstance) };
            const UINT offsets[] = { 0, 0 };
            ID3D11Buffer* buffers[] = { grassShape_.Get(), grassInstances_.Get() };
            context_->IASetVertexBuffers(0, 2, buffers, strides, offsets);
            context_->IASetInputLayout(grassLayout_.Get());
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(grassVertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(grassPixelShader_.Get(), nullptr, 0);
            if (std::abs(cameraZ_ - 285.0f) < 500.0f)
                context_->DrawInstanced(36, nearGrassCount_, 0, 0);
            if (std::abs(cameraZ_ - 1375.0f) < 1000.0f)
                context_->DrawInstanced(18, midGrassCount_, 0, nearGrassCount_);
            if (std::abs(cameraZ_ - 3020.0f) < 1120.0f)
                context_->DrawInstanced(18, grassCount_ - nearGrassCount_ - midGrassCount_,
                                        0, nearGrassCount_ + midGrassCount_);
        }

        void drawBubbles()
        {
            context_->OMSetDepthStencilState(transparentDepthState_.Get(), 0);
            const float blendFactor[4] = { 0, 0, 0, 0 };
            context_->OMSetBlendState(bubbleBlendState_.Get(), blendFactor, 0xffffffff);
            context_->VSSetShader(sceneVertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(bubblePixelShader_.Get(), nullptr, 0);

            for (const Bubble& bubble : bubbles_)
            {
                const float cycle = std::fmod(time_ * bubble.speed + bubble.phase * 9.0f, 9.0f) / 9.0f;
                if (cycle > 0.92f)
                    continue;
                const float x = bubble.position.x + std::sin(time_ * 0.37f + bubble.phase * 6.28f) * 1.4f;
                const float z = bubble.position.y + std::cos(time_ * 0.29f + bubble.phase * 6.28f) * 1.1f;
                const float y = bubble.ground + 2.0f + cycle * 22.0f;
                const float distanceX = x - cameraX_;
                const float distanceY = y - (groundHeight(cameraX_, cameraZ_) + 1.75f);
                const float distanceZ = z - cameraZ_;
                if (distanceX * distanceX + distanceY * distanceY + distanceZ * distanceZ > 1100.0f * 1100.0f)
                    continue;

                const float popFade = std::min((0.92f - cycle) * 10.0f, 1.0f);
                const float pulse = 1.0f + 0.12f * std::sin(cycle * Pi / 0.92f);
                const XMMATRIX world = XMMatrixScaling(bubble.scale * pulse, bubble.scale * pulse,
                                                       bubble.scale * pulse) *
                    XMMatrixTranslation(x, y, z);
                drawMesh(bubbleMesh_, world, XMFLOAT4(1, 1, 1, popFade));
            }

            context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
            context_->OMSetDepthStencilState(nullptr, 0);
            context_->PSSetShader(scenePixelShader_.Get(), nullptr, 0);
        }

        UINT width_ = 1;
        UINT height_ = 1;
        float cameraX_ = 0.0f;
        float cameraZ_ = -12.0f;
        float yaw_ = 0.0f;
        float pitch_ = -0.035f;
        float time_ = 0.0f;
        float jumpHeight_ = 0.0f;
        float verticalVelocity_ = 0.0f;
        bool grounded_ = true;
        bool fullscreen_ = false;
        DWORD windowedStyle_ = 0;
        RECT windowedBounds_{};
        UINT grassCount_ = 0;
        UINT nearGrassCount_ = 0;
        UINT midGrassCount_ = 0;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGISwapChain> swapChain_;
        ComPtr<ID3D11RenderTargetView> renderTarget_;
        ComPtr<ID3D11Texture2D> depthBuffer_;
        ComPtr<ID3D11DepthStencilView> depthView_;
        ComPtr<ID3D11DepthStencilState> skyDepthState_;
        ComPtr<ID3D11DepthStencilState> transparentDepthState_;
        ComPtr<ID3D11RasterizerState> rasterizerState_;
        ComPtr<ID3D11VertexShader> sceneVertexShader_;
        ComPtr<ID3D11PixelShader> scenePixelShader_;
        ComPtr<ID3D11PixelShader> bubblePixelShader_;
        ComPtr<ID3D11VertexShader> grassVertexShader_;
        ComPtr<ID3D11PixelShader> grassPixelShader_;
        ComPtr<ID3D11VertexShader> skyVertexShader_;
        ComPtr<ID3D11PixelShader> skyPixelShader_;
        ComPtr<ID3D11InputLayout> sceneLayout_;
        ComPtr<ID3D11InputLayout> grassLayout_;
        ComPtr<ID3D11Buffer> sceneBuffer_;
        ComPtr<ID3D11Buffer> skyBuffer_;
        ComPtr<ID3D11Buffer> objectBuffer_;
        ComPtr<ID3D11Buffer> grassShape_;
        ComPtr<ID3D11Buffer> grassInstances_;
        ComPtr<ID3D11BlendState> bubbleBlendState_;
        Mesh ground_;
        Mesh arch_;
        Mesh portalLeg_;
        Mesh portalGlow_;
        Mesh portalGlowLeg_;
        Mesh fanFrame_;
        Mesh fanGrille_;
        Mesh turbineBlade_;
        Mesh hub_;
        Mesh flowers_;
        Mesh rock_;
        Mesh clockTower_;
        Mesh clockRim_;
        Mesh clockFace_;
        Mesh clockHourHand_;
        Mesh clockMinuteHand_;
        Mesh clockHub_;
        Mesh bubbleMesh_;
        std::vector<XMFLOAT4> rocks_;
        std::vector<Portal> portals_;
        std::vector<Clock> clocks_;
        std::vector<Bubble> bubbles_;
        std::vector<Turbine> turbines_;
        std::vector<float> terrainSamples_;
        float mouseSensitivity_ = 0.0028f;
        bool mouseCaptured_ = false;
        bool ignoreNextMouseInput_ = false;
    };

    Meadow meadow;
    HWND mainWindow = nullptr;

    LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_SETFOCUS:
            try
            {
                meadow.handleMouseFocus(window, true);
            }
            catch (const std::exception& error)
            {
                MessageBoxA(window, error.what(), "Mouse input error", MB_OK | MB_ICONERROR);
                PostQuitMessage(1);
            }
            return 0;
        case WM_KILLFOCUS:
            meadow.handleMouseFocus(window, false);
            return 0;
        case WM_INPUT:
            try
            {
                meadow.handleRawMouse(lParam);
            }
            catch (const std::exception& error)
            {
                MessageBoxA(window, error.what(), "Mouse input error", MB_OK | MB_ICONERROR);
                PostQuitMessage(1);
            }
            return DefWindowProcW(window, message, wParam, lParam);
        case WM_ACTIVATEAPP:
            if (wParam == FALSE)
                meadow.handleMouseFocus(window, false);
            else
            {
                try
                {
                    meadow.handleMouseFocus(window, true);
                }
                catch (const std::exception& error)
                {
                    MessageBoxA(window, error.what(), "Mouse input error", MB_OK | MB_ICONERROR);
                    PostQuitMessage(1);
                }
            }
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                try
                {
                    meadow.resize(LOWORD(lParam), HIWORD(lParam));
                }
                catch (const std::exception& error)
                {
                    MessageBoxA(window, error.what(), "DirectX error", MB_OK | MB_ICONERROR);
                    PostQuitMessage(1);
                }
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
                DestroyWindow(window);
            else if (wParam == 'R')
                meadow.resetView();
            else if (wParam == VK_SPACE && (lParam & (1LL << 30)) == 0)
                meadow.jump();
            else if (wParam == 'F' && (lParam & (1LL << 30)) == 0)
            {
                try
                {
                    meadow.toggleFullscreen(window);
                }
                catch (const std::exception& error)
                {
                    MessageBoxA(window, error.what(), "Display mode error", MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        const wchar_t* className = L"LiminalMeadowWindow";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = windowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = className;
        if (!RegisterClassExW(&windowClass))
            throw std::runtime_error("Could not register the application window.");

        RECT bounds{ 0, 0, 1440, 900 };
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
        mainWindow = CreateWindowExW(0, className,
            L"LIMINAL MEADOW  |  WASD move  |  Space jump  |  Mouse look  |  F fullscreen  |  Esc exit",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, nullptr);
        if (!mainWindow)
            throw std::runtime_error("Could not create the application window.");

        meadow.initialize(mainWindow);
        ShowWindow(mainWindow, showCommand);
        UpdateWindow(mainWindow);
        wchar_t executablePath[MAX_PATH]{};
        const DWORD executablePathLength = GetModuleFileNameW(nullptr, executablePath, ARRAYSIZE(executablePath));
        if (executablePathLength == 0 || executablePathLength >= ARRAYSIZE(executablePath))
            throw std::runtime_error("Could not locate the application folder for background music.");
        std::wstring musicPath(executablePath, executablePathLength);
        const size_t pathSeparator = musicPath.find_last_of(L"\\/");
        if (pathSeparator == std::wstring::npos)
            throw std::runtime_error("Could not locate the application folder for background music.");
        musicPath.resize(pathSeparator + 1);
        musicPath += L"music\\music.mp3";
        BackgroundMusic music;
        music.start(musicPath);

        LARGE_INTEGER frequency{};
        LARGE_INTEGER previous{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);
        MSG message{};
        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else
            {
                LARGE_INTEGER current{};
                QueryPerformanceCounter(&current);
                const float deltaTime = std::min(static_cast<float>(current.QuadPart - previous.QuadPart) /
                                                  static_cast<float>(frequency.QuadPart), 0.05f);
                previous = current;
                meadow.update(deltaTime);
                meadow.render();
            }
        }
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Liminal Meadow could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
}
