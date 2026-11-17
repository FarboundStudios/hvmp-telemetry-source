#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include "heavymp_packet_inbox.h"
#include "heavymp_snapshot_pool.h"
#include "heavymp_performance.h"
#include "heavymp_vehicles.h"
#include "heavymp_native.h"
#include "heavymp_zone_policy.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "vendor/minhook/include/MinHook.h"
using Microsoft::WRL::ComPtr;
namespace {
constexpr double tau = 6.283185307179586;
struct Vec {
    double x = 0, y = 0, z = 0;
    Vec operator+(Vec b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    Vec operator-(Vec b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    Vec operator*(double t) const {
        return {x * t, y * t, z * t};
    }
};
double dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec rotate(Vec v, Vec r) {
    const double h = r.x * tau, p = r.y * tau, z = r.z * tau;
    Vec a{v.x * cos(z) - v.y * sin(z), v.x * sin(z) + v.y * cos(z), v.z};
    Vec b{a.x, a.y * cos(p) - a.z * sin(p), a.y * sin(p) + a.z * cos(p)};
    return {b.x * cos(h) + b.z * sin(h), b.y, -b.x * sin(h) + b.z * cos(h)};
}
Vec position(const scs_value_fvector_t& v) {
    return {v.x, v.y, v.z};
}
Vec angles(const scs_value_euler_t& v) {
    return {v.heading, v.pitch, v.roll};
}
struct Camera {
    Vec eye, right, up, forward;
    bool active = false;
    ULONGLONG at = 0;
} camera;
struct Pose {
    Vec p, r;
};
struct Peer {
    mutable HeavyMpSnapshotPool<HeavyMpNativePeer> snapshots;
    std::array<unsigned char, 16> id{}, owner{};
    unsigned segment = 0;
    Pose previous, next;
    ULONGLONG at = 0;
    HeavyMpNativePeer animation{};
};
std::vector<Peer> peers, trailerPeers;
HeavyMpZonePolicy zonePolicy;
ULONGLONG zonePolicyUntil = 0;
struct Configuration {
    std::array<unsigned char, 16> id{};
    std::shared_ptr<const HeavyMpAppearance> appearance;
    ULONGLONG at = 0;
};
std::vector<Configuration> configurations;
struct AudioConfiguration {
    std::array<unsigned char, 16> id{};
    std::array<std::string, 3> refs{};
    ULONGLONG at = 0;
};
std::vector<AudioConfiguration> audioConfigurations;
SRWLOCK lock = SRWLOCK_INIT;
SOCKET input = INVALID_SOCKET;
HeavyMpPacketInbox packetInbox;
std::atomic<bool> receiverRunning{false};
std::thread receiver;
bool startReceiver() {
    packetInbox.clear();
    receiverRunning = true;
    try {
        receiver = std::thread([]() {
            std::array<unsigned char, 60001> data{};
            while (receiverRunning) {
                for (unsigned i = 0; i < 128 && receiverRunning; ++i) {
                    sockaddr_in from{};
                    int size = sizeof(from);
                    const int bytes =
                        recvfrom(input, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()),
                                 0, reinterpret_cast<sockaddr*>(&from), &size);
                    if (bytes < 0)
                        break;
                    if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
                        packetInbox.push(data.data(), static_cast<size_t>(bytes));
                }
                Sleep(1);
            }
        });
        return true;
    } catch (...) {
        receiverRunning = false;
        return false;
    }
}
void stopReceiver() {
    receiverRunning = false;
    if (receiver.joinable())
        receiver.join();
    packetInbox.clear();
}

scs_value_fplacement_t cabinOffset{}, headOffset{};
Vec cabinBase, headBase;
bool headConfigured = false, haveHead = false, haveCabin = false;
scs_log_t gameLog = nullptr;
std::atomic<bool> enabled{false};
bool nativeMode = false;
std::atomic<unsigned> lastDrawn{0};
std::atomic<ULONGLONG> lastDrawAt{0};
void forwardUiPacket(const unsigned char* data, unsigned bytes) {
    const auto ui = GetModuleHandleW(L"heavymp-ui.dll");
    if (!ui)
        return;
    using Packet = void (*)(const unsigned char*, unsigned);
    const auto receive = reinterpret_cast<Packet>(GetProcAddress(ui, "HeavyMpUiChatPacket"));
    if (receive)
        receive(data, bytes);
}
void log(const char* message) {
    if (gameLog)
        gameLog(SCS_LOG_TYPE_message, message);
}
struct Vertex {
    float x, y, z, w, red, green, blue;
};
struct MeshVertex {
    Vec position, color;
};
std::vector<MeshVertex> mesh;
void triangle(Vec a, Vec b, Vec c, Vec color) {
    mesh.push_back({a, color});
    mesh.push_back({b, color});
    mesh.push_back({c, color});
}
void box(Vec low, Vec high, Vec color) {
    Vec v[8] = {{low.x, low.y, low.z},    {high.x, low.y, low.z}, {high.x, high.y, low.z},
                {low.x, high.y, low.z},   {low.x, low.y, high.z}, {high.x, low.y, high.z},
                {high.x, high.y, high.z}, {low.x, high.y, high.z}};
    const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                             {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    for (int i = 0; i < 6; ++i) {
        auto f = faces[i];
        Vec shade = color * (0.65 + 0.07 * i);
        triangle(v[f[0]], v[f[1]], v[f[2]], shade);
        triangle(v[f[0]], v[f[2]], v[f[3]], shade);
    }
}
void wheel(double x, double z) {
    constexpr int sides = 16;
    const double outer = x + (x > 0 ? 0.23 : -0.23), inner = x + (x > 0 ? -0.23 : 0.23);
    for (int i = 0; i < sides; ++i) {
        const double a = tau * i / sides, b = tau * (i + 1) / sides;
        Vec u{outer, 0.55 + 0.55 * cos(a), z + 0.55 * sin(a)},
            v{outer, 0.55 + 0.55 * cos(b), z + 0.55 * sin(b)}, w{inner, u.y, u.z}, q{inner, v.y, v.z};
        triangle(u, v, {outer, 0.55, z}, {0.12, 0.13, 0.14});
        triangle(u, w, q, {0.05, 0.06, 0.07});
        triangle(u, q, v, {0.05, 0.06, 0.07});
    }
}
void makeMesh() {
    mesh.clear();
    box({-1.05, 0.65, -3.0}, {1.05, 1.05, 3.0}, {0.20, 0.24, 0.28});
    box({-1.22, 1.0, -3.0}, {1.22, 3.6, -0.6}, {0.10, 0.65, 0.82});
    box({-1.08, 2.2, -3.025}, {1.08, 3.14, -3.005}, {0.06, 0.12, 0.17});
    box({-1.225, 2.2, -2.7}, {-1.21, 3.1, -0.9}, {0.06, 0.12, 0.17});
    box({1.21, 2.2, -2.7}, {1.225, 3.1, -0.9}, {0.06, 0.12, 0.17});
    box({-1.25, 0.65, -3.12}, {1.25, 1.05, -2.9}, {0.28, 0.32, 0.36});
    box({-0.7, 1.02, 1.0}, {0.7, 1.16, 2.1}, {0.10, 0.11, 0.12});
    for (double x : {-1.08, 1.08}) {
        wheel(x, -2.0);
        wheel(x, 1.9);
    }
}
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
Present originalPresent = nullptr;
void* presentAddress = nullptr;
ComPtr<ID3D11Device1> device;
ComPtr<ID3D11DeviceContext1> context;
ComPtr<ID3DDeviceContextState> ownState;
ComPtr<ID3D11VertexShader> vertexShader;
ComPtr<ID3D11PixelShader> pixelShader;
ComPtr<ID3D11InputLayout> layout;
ComPtr<ID3D11Buffer> vertices;
ComPtr<ID3D11RasterizerState> rasterizer;
ComPtr<ID3D11DepthStencilState> depthState;
ComPtr<ID3D11Texture2D> depthTexture;
ComPtr<ID3D11DepthStencilView> depthView;
UINT depthWidth = 0, depthHeight = 0;
float horizontalFov = 75;
void releaseRenderer() {
    depthView.Reset();
    depthTexture.Reset();
    vertices.Reset();
    layout.Reset();
    vertexShader.Reset();
    pixelShader.Reset();
    rasterizer.Reset();
    depthState.Reset();
    ownState.Reset();
    context.Reset();
    device.Reset();
    depthWidth = depthHeight = 0;
}
bool initialize(IDXGISwapChain* chain) {
    ComPtr<ID3D11Device> base;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&base))) || FAILED(base.As(&device)))
        return false;
    ComPtr<ID3D11DeviceContext> c;
    base->GetImmediateContext(&c);
    if (FAILED(c.As(&context)))
        return false;
    D3D_FEATURE_LEVEL chosen;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(device->CreateDeviceContextState(0, levels, 2, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                &chosen, &ownState)))
        return false;
    const char* shader =
        "struct V{float4 p:POSITION;float3 c:COLOR;};struct O{float4 p:SV_Position;float3 c:COLOR;};O vs(V "
        "v){O o;o.p=v.p;o.c=v.c;return o;}float4 ps(O o):SV_Target{return float4(o.c,1);}";
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &errors)) ||
        FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &errors)))
        return false;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                          &vertexShader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader)))
        return false;
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (FAILED(device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout)))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(mesh.size() * 32 * sizeof(Vertex));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &vertices)))
        return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &rasterizer)))
        return false;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&dd, &depthState)))
        return false;
    return true;
}
double angleMix(double a, double b, double t) {
    double delta = std::remainder(b - a, 1.0);
    return a + delta * t;
}
void draw(IDXGISwapChain* chain) {
    const auto now = GetTickCount64();
    if (!camera.active || now - camera.at > 500 || peers.empty())
        return;
    DXGI_SWAP_CHAIN_DESC swap{};
    if (FAILED(chain->GetDesc(&swap)))
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(swap.OutputWindow, &pid);
    if (pid != GetCurrentProcessId())
        return;
    if (!device && !initialize(chain)) {
        releaseRenderer();
        return;
    }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return;
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target)))
        return;
    if (depthWidth != description.Width || depthHeight != description.Height) {
        depthView.Reset();
        depthTexture.Reset();
        auto dd = description;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.CPUAccessFlags = dd.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &depthTexture)) ||
            FAILED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView)))
            return;
        depthWidth = description.Width;
        depthHeight = description.Height;
    }
    std::vector<Vertex> output;
    output.reserve(mesh.size() * peers.size());
    const double sx = 1 / tan(horizontalFov * tau / 720), sy = sx * description.Width / description.Height;
    unsigned drawn = 0;
    for (const auto& peer : peers) {
        if (now - peer.at > 1000)
            continue;
        const double t = std::clamp(static_cast<double>(now - peer.at) / 50.0, 0.0, 1.0);
        Pose pose{peer.previous.p * (1 - t) + peer.next.p * t,
                  {angleMix(peer.previous.r.x, peer.next.r.x, t),
                   angleMix(peer.previous.r.y, peer.next.r.y, t),
                   angleMix(peer.previous.r.z, peer.next.r.z, t)}};
        const Vec delta = pose.p - camera.eye;
        if (dot(delta, delta) > 600 * 600)
            continue;
        if (dot(delta, camera.forward) <= 0)
            continue;
        ++drawn;
        for (const auto& v : mesh) {
            const Vec relative = pose.p + rotate(v.position, pose.r) - camera.eye;
            const double x = dot(relative, camera.right), y = dot(relative, camera.up),
                         z = dot(relative, camera.forward);
            output.push_back({static_cast<float>(x * sx), static_cast<float>(y * sy),
                              static_cast<float>(z * 1000 / 999.9 - 0.10001), static_cast<float>(z),
                              static_cast<float>(v.color.x), static_cast<float>(v.color.y),
                              static_cast<float>(v.color.z)});
        }
    }
    if (output.empty())
        return;
    // Swap the entire immediate context state, rather than leaving modified game shaders/bindings.
    ComPtr<ID3DDeviceContextState> previous;
    context->SwapDeviceContextState(ownState.Get(), &previous);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, output.data(), output.size() * sizeof(Vertex));
        context->Unmap(vertices.Get(), 0);
        auto rtv = target.Get();
        context->OMSetRenderTargets(1, &rtv, depthView.Get());
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->RSSetState(rasterizer.Get());
        D3D11_VIEWPORT viewport{
            0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        auto vb = vertices.Get();
        const UINT stride = sizeof(Vertex), offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->Draw(static_cast<UINT>(output.size()), 0);
        lastDrawn = drawn;
        lastDrawAt = now;
    }
    context->SwapDeviceContextState(previous.Get(), nullptr);
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    AcquireSRWLockExclusive(&lock);
    if (enabled && !(flags & DXGI_PRESENT_TEST))
        draw(chain);
    auto original = originalPresent;
    ReleaseSRWLockExclusive(&lock);
    return original(chain, interval, flags);
}
bool attach() {
    HWND dummy = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!dummy)
        return false;
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = dummy;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> dd;
    ComPtr<ID3D11DeviceContext> cc;
    D3D_FEATURE_LEVEL feature;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &d, &chain, &dd, &feature, &cc);
    DestroyWindow(dummy);
    if (FAILED(result))
        return false;
    presentAddress = (*reinterpret_cast<void***>(chain.Get()))[8];
    auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(presentAddress, reinterpret_cast<void*>(&present),
                      reinterpret_cast<void**>(&originalPresent)) != MH_OK)
        return false;
    // Pin the plugin: the retained, disabled trampoline remains valid during SDK shutdown.
    HMODULE module;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&present), &module);
    return MH_EnableHook(presentAddress) == MH_OK;
}
SCSAPI_VOID offsetCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t ctx) {
    auto* output = static_cast<scs_value_fplacement_t*>(ctx);
    bool good = value && value->type == SCS_VALUE_TYPE_fplacement;
    if (good)
        *output = value->value_fplacement;
    else
        *output = {};
    if (output == &headOffset)
        haveHead = good;
    else
        haveCabin = good;
}
} // namespace
void heavymp_vehicles_prepare(const scs_telemetry_init_params_v100_t* api) {
    const auto flags = SCS_TELEMETRY_CHANNEL_FLAG_each_frame | SCS_TELEMETRY_CHANNEL_FLAG_no_value;
    api->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_cabin_offset, SCS_U32_NIL,
                              SCS_VALUE_TYPE_fplacement, flags, offsetCallback, &cabinOffset);
    api->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_head_offset, SCS_U32_NIL, SCS_VALUE_TYPE_fplacement,
                              flags, offsetCallback, &headOffset);
}
bool heavymp_vehicles_start(const scs_telemetry_init_params_v100_t* api) {
    wchar_t executable[MAX_PATH];
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto* name = wcsrchr(executable, L'\\');
    if (!name || _wcsicmp(name + 1, L"eurotrucks2.exe") != 0)
        return false;
    wchar_t skip[8];
    if (GetEnvironmentVariableW(L"HEAVYMP_NO_RENDER", skip, 8))
        return false;
    gameLog = api->common.log;
    input = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (input == INVALID_SOCKET)
        return false;
    BOOL exclusive = TRUE;
    setsockopt(input, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive),
               sizeof(exclusive));
    unsigned short vehiclePort = 34063;
    wchar_t portText[16]{};
    if (GetEnvironmentVariableW(L"HEAVYMP_VEHICLE_PORT", portText, 16)) {
        wchar_t* end = nullptr;
        const auto port = wcstoul(portText, &end, 10);
        if (end && !*end && port > 0 && port <= 65535)
            vehiclePort = static_cast<unsigned short>(port);
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(vehiclePort);
    u_long nonblocking = 1;
    if (bind(input, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        ioctlsocket(input, FIONBIO, &nonblocking) != 0) {
        closesocket(input);
        input = INVALID_SOCKET;
        log("[HeavyMP] Tractor receiver unavailable (UDP 34063).");
        return false;
    }
    makeMesh();
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&present), &module);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    auto slash = wcsrchr(path, L'\\');
    if (slash)
        wcscpy_s(slash, MAX_PATH - static_cast<size_t>(slash - path), L"\\heavymp-render.ini");
    nativeMode = GetPrivateProfileIntW(L"native", L"enabled", 0, path) != 0;
    horizontalFov = static_cast<float>(
        std::clamp(GetPrivateProfileIntW(L"camera", L"horizontal_fov", 75, path), 30u, 140u));
    if (nativeMode) {
        const bool started = heavymp_native_start(gameLog);
        enabled = started;
        if (started && !startReceiver()) {
            enabled = false;
            heavymp_native_stop();
            log("[HeavyMP] Receiver thread unavailable.");
            return false;
        }
        return started;
    }
    bool hooked = originalPresent ? MH_EnableHook(presentAddress) == MH_OK : attach();
    if (hooked && !startReceiver())
        hooked = false;
    enabled = hooked;
    log(hooked ? "[HeavyMP] Experimental tractors enabled: UDP 34063, Direct3D 11, no trailers/collisions. "
                 "Scene occlusion NOT implemented."
               : "[HeavyMP] Direct3D 11 tractor hook unavailable; telemetry remains active.");
    return hooked;
}
void heavymp_vehicles_event(scs_event_t event, const void* info) {
    if (event == SCS_TELEMETRY_EVENT_paused) {
        AcquireSRWLockExclusive(&lock);
        camera.active = false;
        ReleaseSRWLockExclusive(&lock);
    }
    if (event == SCS_TELEMETRY_EVENT_started) {
        haveHead = false;
        haveCabin = false;
    }
    if (event != SCS_TELEMETRY_EVENT_configuration || !info)
        return;
    const auto* c = static_cast<const scs_telemetry_configuration_t*>(info);
    if (strcmp(c->id, SCS_TELEMETRY_CONFIG_truck) != 0)
        return;
    headConfigured = false;
    cabinBase = {};
    headBase = {};
    for (auto a = c->attributes; a && a->name; ++a) {
        if (a->value.type != SCS_VALUE_TYPE_fvector)
            continue;
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cabin_position) == 0)
            cabinBase = position(a->value.value_fvector);
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_head_position) == 0) {
            headBase = position(a->value.value_fvector);
            headConfigured = true;
        }
    }
}
void heavymp_vehicles_frame(const scs_value_dplacement_t& local, bool available, bool paused) {
    if (!enabled)
        return;
    AcquireSRWLockExclusive(&lock);
    const auto now = GetTickCount64();
    camera.active = available && !paused && headConfigured && haveHead;
    if (camera.active) {
        auto truck = angles(local.orientation), cab = angles(cabinOffset.orientation),
             head = angles(headOffset.orientation);
        const Vec base{local.position.x, local.position.y, local.position.z};
        camera.eye = base + rotate(cabinBase + position(cabinOffset.position) +
                                       rotate(headBase + position(headOffset.position), cab),
                                   truck);
        auto basis = [&](Vec v) { return rotate(rotate(rotate(v, head), cab), truck); };
        camera.right = basis({1, 0, 0});
        camera.up = basis({0, 1, 0});
        camera.forward = basis({0, 0, -1});
        camera.at = now;
    }
    const auto receiveStart = heavyMpPerfClock();
    std::vector<unsigned char> packetData;
    for (int packet = 0; packet < 16; ++packet) {
        if (packet && heavyMpPerfMs(heavyMpPerfClock() - receiveStart) >= 1.5)
            break;
        if (!packetInbox.pop(packetData))
            break;
        const auto* data = packetData.data();
        const int bytes = static_cast<int>(packetData.size());
        unsigned int count = 0, stride = 0;
        if (bytes < 16)
            continue;
        if (memcmp(data, "HIA1", 4) == 0 || memcmp(data, "HIV1", 4) == 0) {
            if (nativeMode)
                heavymp_native_traffic_packet(data, static_cast<size_t>(bytes));
            continue;
        }
        if (memcmp(data, "HME1", 4) == 0)
            continue; // Legacy dealer access packet.
        if (memcmp(data, "HMX1", 4) == 0 || memcmp(data, "HMX2", 4) == 0 || memcmp(data, "HMX3", 4) == 0 ||
            memcmp(data, "HMP1", 4) == 0 || (bytes == 16 && memcmp(data, "HML1", 4) == 0)) {
            forwardUiPacket(data, static_cast<unsigned>(bytes));
            continue;
        }
        if (memcmp(data, "HMR1", 4) == 0) {
            if (nativeMode)
                heavymp_native_repair_packet(data, static_cast<size_t>(bytes));
            continue;
        }
        if (memcmp(data, "HMD1", 4) == 0) {
            if (nativeMode)
                heavymp_native_dispatch_packet(data, static_cast<size_t>(bytes));
            continue;
        }
        if (memcmp(data, "HNC1", 4) == 0) {
            HeavyMpZonePolicy next;
            if (heavyMpZonePacket(data, static_cast<size_t>(bytes), &next)) {
                zonePolicy = next;
                zonePolicyUntil = now + 1500;
                forwardUiPacket(data, static_cast<unsigned>(bytes));
            }
            continue;
        }
        if (memcmp(data, "HMCL", 4) == 0) {
            if (nativeMode)
                heavymp_native_collision_packet(data, static_cast<size_t>(bytes));
            continue;
        }
        if (memcmp(data, "HMW1", 4) == 0) {
            if (nativeMode)
                heavymp_native_world_packet(data, static_cast<size_t>(bytes));
            continue;
        }
        if (memcmp(data, "HMS1", 4) == 0) {
            memcpy(&count, data + 8, 4);
            memcpy(&stride, data + 12, 4);
            if (count > 32 || stride != 784 || bytes != 16 + static_cast<int>(count * stride))
                continue;
            std::vector<AudioConfiguration> incoming;
            bool valid = true;
            for (unsigned i = 0; i < count; ++i) {
                AudioConfiguration c{};
                const auto* record = data + 16 + i * stride;
                memcpy(c.id.data(), record, 16);
                c.at = now;
                for (unsigned j = 0; j < 3; ++j) {
                    const char* begin = reinterpret_cast<const char*>(record + 16 + j * 256);
                    const auto length = strnlen_s(begin, 256);
                    if (length > 240) {
                        valid = false;
                        break;
                    }
                    c.refs[j].assign(begin, length);
                    if (!heavympAudioReference(c.refs[j])) {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    break;
                incoming.push_back(std::move(c));
            }
            if (valid)
                audioConfigurations = std::move(incoming);
            continue;
        }
        if (memcmp(data, "HMC1", 4) == 0 || memcmp(data, "HMC2", 4) == 0) {
            std::array<unsigned char, 16> id{};
            const auto appearance = heavympAppearancePacket(data, static_cast<size_t>(bytes), id);
            if (!appearance)
                continue;
            auto existing =
                std::find_if(configurations.begin(), configurations.end(), [&](const Configuration& c) {
                    return c.id == id && c.appearance->trailer == appearance->trailer;
                });
            if (existing != configurations.end()) {
                if (existing->appearance->revision != appearance->revision)
                    existing->appearance = appearance;
                existing->at = now;
            } else if (configurations.size() < 384)
                configurations.push_back({id, appearance, now});
            continue;
        }
        if (memcmp(data, "HMT1", 4) == 0 || memcmp(data, "HMT2", 4) == 0 || memcmp(data, "HMT3", 4) == 0) {
            const bool timedTrailer = memcmp(data, "HMT3", 4) == 0;
            const bool chain = memcmp(data, "HMT2", 4) == 0 || timedTrailer;
            memcpy(&count, data + 8, 4);
            memcpy(&stride, data + 12, 4);
            if (count > (chain ? 128u : 32u) ||
                stride != (timedTrailer ? 424u
                           : chain      ? 416u
                                        : 400u) ||
                bytes != 16 + static_cast<int>(count * stride))
                continue;
            std::vector<Peer> incoming;
            bool valid = true;
            for (unsigned i = 0; i < count; ++i) {
                const auto* r = data + 16 + i * stride;
                unsigned flags = 0, wheelCount = 0;
                memcpy(&flags, r + 64, 4);
                memcpy(&wheelCount, r + 68, 4);
                if ((flags != 0 && flags != 1 && flags != 3) || wheelCount > 16) {
                    valid = false;
                    break;
                }
                if (!flags)
                    continue;
                double v[6]{};
                memcpy(v, r + 16, 48);
                for (unsigned k = 0; k < 6; ++k)
                    if (!std::isfinite(v[k]) || std::abs(v[k]) > (k == 0 || k == 2 ? 10000000
                                                                  : k == 1         ? 100000
                                                                                   : 10))
                        valid = false;
                if (!valid)
                    break;
                Peer t{};
                memcpy(t.id.data(), r, 16);
                t.next = {{v[0], v[1], v[2]}, {v[3], v[4], v[5]}};
                t.previous = t.next;
                t.at = now;
                if (timedTrailer) {
                    unsigned age = 0;
                    memcpy(&age, r + 416, 4);
                    if (age > 5000) {
                        valid = false;
                        break;
                    }
                    t.animation.wheelSampleAt = now > age ? now - age : 0;
                }
                t.animation.connected = (flags & 2) != 0;
                t.animation.wheelCount = wheelCount;
                t.owner = t.id;
                if (chain) {
                    memcpy(t.owner.data(), r + 400, 16);
                    memcpy(&t.segment, r + 392, 4);
                    if (t.segment >= 10) {
                        valid = false;
                        break;
                    }
                }
                memcpy(t.animation.wheels.data(), r + 72, 320);
                for (unsigned j = 0; j < wheelCount; ++j) {
                    const auto& w = t.animation.wheels[j];
                    if (!std::isfinite(w.rotation) || w.rotation < 0 || w.rotation > 1 ||
                        !std::isfinite(w.velocity) || std::abs(w.velocity) > 100 ||
                        !std::isfinite(w.steering) || std::abs(w.steering) > .25 ||
                        !std::isfinite(w.suspension) || std::abs(w.suspension) > 2 ||
                        !std::isfinite(w.lift) || w.lift < 0 || w.lift > 1)
                        valid = false;
                }
                if (!valid)
                    break;
                const auto old = std::find_if(trailerPeers.begin(), trailerPeers.end(),
                                              [&](const Peer& p) { return p.id == t.id; });
                if (old != trailerPeers.end())
                    t.snapshots = old->snapshots;
                if (old != trailerPeers.end() && dot(old->next.p - t.next.p, old->next.p - t.next.p) < 2500) {
                    const double mix = std::clamp(static_cast<double>(now - old->at) / 50.0, 0.0, 1.0);
                    t.previous.p = old->previous.p * (1 - mix) + old->next.p * mix;
                    t.previous.r = {angleMix(old->previous.r.x, old->next.r.x, mix),
                                    angleMix(old->previous.r.y, old->next.r.y, mix),
                                    angleMix(old->previous.r.z, old->next.r.z, mix)};
                }
                incoming.push_back(t);
            }
            if (valid)
                trailerPeers = std::move(incoming);
            continue;
        }
        const bool timed = memcmp(data, "HMV3", 4) == 0;
        const bool v2 = memcmp(data, "HMV2", 4) == 0 || timed;
        if (!v2 && memcmp(data, "HMV1", 4) != 0)
            continue;
        memcpy(&count, data + 8, 4);
        memcpy(&stride, data + 12, 4);
        if (count > 32 ||
            stride != (timed ? 456u
                       : v2  ? 448u
                             : 72u) ||
            bytes != 16 + static_cast<int>(count * stride))
            continue;
        std::vector<Peer> incoming;
        bool valid = true;
        for (unsigned int i = 0; i < count; ++i) {
            const auto* record = data + 16 + i * stride;
            double values[6];
            memcpy(values, record + 16, 48);
            for (double v : values)
                if (!std::isfinite(v))
                    valid = false;
            if (!valid)
                break;
            unsigned int age, flags;
            memcpy(&age, record + 64, 4);
            memcpy(&flags, record + 68, 4);
            if (age > 1000 || flags != 0) {
                valid = false;
                break;
            }
            if (std::abs(values[0]) > 10000000 || std::abs(values[1]) > 100000 ||
                std::abs(values[2]) > 10000000 || std::abs(values[3]) > 2 || std::abs(values[4]) > 1 ||
                std::abs(values[5]) > 1) {
                valid = false;
                break;
            }
            Peer p;
            memcpy(p.id.data(), record, 16);
            p.next = {{values[0], values[1], values[2]}, {values[3], values[4], values[5]}};
            p.previous = p.next;
            p.at = now;
            if (v2) {
                auto& a = p.animation;
                if (timed) {
                    unsigned animationAge = 0;
                    memcpy(&animationAge, record + 448, 4);
                    if (animationAge > 5000) {
                        valid = false;
                        break;
                    }
                    a.wheelSampleAt = now > animationAge ? now - animationAge : 0;
                }
                memcpy(&a.speed, record + 72, 8);
                memcpy(&a.lights, record + 80, 4);
                memcpy(&a.wheelCount, record + 84, 4);
                memcpy(a.wheels.data(), record + 88, sizeof(HeavyMpWheelState) * 16);
                memcpy(&a.controls, record + 408, 4);
                memcpy(&a.rpm, record + 412, 4);
                memcpy(&a.gear, record + 416, 4);
                memcpy(&a.brake, record + 420, 4);
                memcpy(&a.throttle, record + 424, 4);
                memcpy(&a.steering, record + 428, 4);
                memcpy(&a.horn, record + 432, 4);
                if (a.horn > 3) {
                    valid = false;
                    break;
                }
                unsigned wiperFlags = 0;
                memcpy(&wiperFlags, record + 444, 4);
                if (wiperFlags & 0x80000000u) {
                    a.haveWiperPhase = true;
                    a.wiperMode = wiperFlags & 0x7fffffffu;
                    memcpy(&a.wiperPhase, record + 436, 4);
                    memcpy(&a.rearWiperPhase, record + 440, 4);
                    if (a.wiperMode > 16 || !std::isfinite(a.wiperPhase) || a.wiperPhase < 0 ||
                        a.wiperPhase > 1 || !std::isfinite(a.rearWiperPhase) || a.rearWiperPhase < 0 ||
                        a.rearWiperPhase > 1) {
                        valid = false;
                        break;
                    }
                }
                if (!std::isfinite(a.speed) || std::abs(a.speed) > 150 || a.wheelCount > 16 ||
                    a.controls > 15 || !std::isfinite(a.rpm) || a.rpm < 0 || a.rpm > 10000 || a.gear < -30 ||
                    a.gear > 30 || !std::isfinite(a.brake) || a.brake < 0 || a.brake > 1 ||
                    !std::isfinite(a.throttle) || a.throttle < 0 || a.throttle > 1 ||
                    !std::isfinite(a.steering) || std::abs(a.steering) > 1) {
                    valid = false;
                    break;
                }
                for (unsigned w = 0; w < a.wheelCount; ++w) {
                    const auto& v = a.wheels[w];
                    if (!std::isfinite(v.rotation) || v.rotation < 0 || v.rotation > 1 ||
                        !std::isfinite(v.velocity) || std::abs(v.velocity) > 100 ||
                        !std::isfinite(v.steering) || std::abs(v.steering) > .25 ||
                        !std::isfinite(v.suspension) || std::abs(v.suspension) > 2 ||
                        !std::isfinite(v.lift) || v.lift < 0 || v.lift > 1)
                        valid = false;
                }
                if (!valid)
                    break;
            }
            auto old = std::find_if(peers.begin(), peers.end(), [&](const Peer& v) { return v.id == p.id; });
            if (old != peers.end() && dot(old->next.p - p.next.p, old->next.p - p.next.p) < 2500) {
                double t = std::clamp(static_cast<double>(now - old->at) / 50.0, 0.0, 1.0);
                p.previous.p = old->previous.p * (1 - t) + old->next.p * t;
                p.previous.r = {angleMix(old->previous.r.x, old->next.r.x, t),
                                angleMix(old->previous.r.y, old->next.r.y, t),
                                angleMix(old->previous.r.z, old->next.r.z, t)};
            }
            incoming.push_back(p);
        }
        if (valid)
            peers = std::move(incoming);
    }
    std::vector<HeavyMpNativePeer> nativePeers;
    nativePeers.reserve(32);
    configurations.erase(std::remove_if(configurations.begin(), configurations.end(),
                                        [&](const Configuration& c) { return now - c.at > 30000; }),
                         configurations.end());
    if (nativeMode && available) {
        for (const auto& peer : peers) {
            if (now - peer.at > 1000)
                continue;
            const double t = std::clamp(static_cast<double>(now - peer.at) / 50.0, 0.0, 1.0);
            const auto p = peer.previous.p * (1 - t) + peer.next.p * t;
            auto a = peer.animation;
            a.id = peer.id;
            a.x = p.x;
            a.y = p.y;
            a.z = p.z;
            a.heading = angleMix(peer.previous.r.x, peer.next.r.x, t);
            a.pitch = angleMix(peer.previous.r.y, peer.next.r.y, t);
            a.roll = angleMix(peer.previous.r.z, peer.next.r.z, t);
            const auto config =
                std::find_if(configurations.begin(), configurations.end(), [&](const Configuration& c) {
                    return c.id == peer.id && !c.appearance->trailer;
                });
            if (config != configurations.end())
                a.appearance = config->appearance;
            const auto audio = std::find_if(
                audioConfigurations.begin(), audioConfigurations.end(),
                [&](const AudioConfiguration& c) { return c.id == peer.id && now - c.at <= 1500; });
            if (audio != audioConfigurations.end())
                a.audioReferences = audio->refs;
            // Preserve the raw SDK sample. Only the native wheel callback predicts it,
            // using the original arrival time rather than resetting it each frame.
            a.zoneGhost = zonePolicy.ghost(peer.id, now, zonePolicyUntil);
            a.wheelSampleAt = peer.animation.wheelSampleAt ? peer.animation.wheelSampleAt : peer.at;
            std::array<const Peer*, 10> chain{};
            for (const auto& candidate : trailerPeers)
                if (candidate.owner == peer.id && candidate.segment < 10 && now - candidate.at <= 1000)
                    chain[candidate.segment] = &candidate;
            std::shared_ptr<HeavyMpNativePeer> next;
            if (chain[0])
                for (int index = 9; index >= 0; --index) {
                    const auto* trailer = chain[index];
                    if (!trailer)
                        continue;
                    auto state = trailer->snapshots.acquire();
                    *state = trailer->animation;
                    state->id = trailer->id;
                    const double blend = std::clamp(static_cast<double>(now - trailer->at) / 50.0, 0.0, 1.0);
                    const auto point = trailer->previous.p * (1 - blend) + trailer->next.p * blend;
                    state->x = point.x;
                    state->y = point.y;
                    state->z = point.z;
                    state->heading = angleMix(trailer->previous.r.x, trailer->next.r.x, blend);
                    state->pitch = angleMix(trailer->previous.r.y, trailer->next.r.y, blend);
                    state->roll = angleMix(trailer->previous.r.z, trailer->next.r.z, blend);
                    const auto trailerConfig = std::find_if(
                        configurations.begin(), configurations.end(),
                        [&](const Configuration& c) { return c.id == trailer->id && c.appearance->trailer; });
                    if (trailerConfig != configurations.end())
                        state->appearance = trailerConfig->appearance;
                    state->wheelSampleAt =
                        trailer->animation.wheelSampleAt ? trailer->animation.wheelSampleAt : trailer->at;
                    state->zoneGhost = a.zoneGhost;
                    state->trailer = next;
                    next = state;
                }
            a.trailer = next;
            nativePeers.push_back(a);
        }
    }
    static ULONGLONG queueReportAt = now;
    if (now - queueReportAt >= 5000) {
        queueReportAt = now;
        const auto stats = packetInbox.stats();
        char line[240]{};
        sprintf_s(line, "[HeavyMP receiver] queued=%zu bytes=%zu coalesced=%llu dropped=%llu", stats.packets,
                  stats.bytes, stats.replaced, stats.dropped);
        log(line);
    }
    // Snapshot the displayed native camera, including external/roof/free views.
    // Never fall back to the driver's head in native mode: that would display
    // labels at incorrect coordinates as soon as another camera is selected.
    HeavyMpTagCamera displayed{};
    bool displayedActive = camera.active;
    if (nativeMode)
        displayedActive = available && !paused && heavymp_native_camera(&displayed);
    else {
        const Vec oldBasis[] = {camera.eye, camera.right, camera.up, camera.forward};
        double* vectors[] = {displayed.eye, displayed.right, displayed.up, displayed.forward};
        for (unsigned i = 0; i < 4; ++i) {
            vectors[i][0] = oldBasis[i].x;
            vectors[i][1] = oldBasis[i].y;
            vectors[i][2] = oldBasis[i].z;
        }
        displayed.horizontalFov = horizontalFov;
    }
    const unsigned header = nativeMode ? 192u : 128u;
    std::vector<unsigned char> overlay(header + peers.size() * 48, 0);
    memcpy(overlay.data(), nativeMode ? "HMO2" : "HMO1", 4);
    auto put = [&](unsigned offset, const auto& value) {
        memcpy(overlay.data() + offset, &value, sizeof(value));
    };
    const unsigned version = nativeMode ? 2u : 1u, count = static_cast<unsigned>(peers.size()), stride = 48,
                   active = displayedActive ? 1u : 0u;
    put(4, version);
    put(8, count);
    put(12, stride);
    const double* basis[] = {displayed.eye, displayed.right, displayed.up, displayed.forward};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 3; ++j)
            put(16 + i * 24 + j * 8, basis[i][j]);
    if (nativeMode) {
        memcpy(overlay.data() + 112, displayed.projection, sizeof(displayed.projection));
        put(176, active);
    } else {
        put(112, horizontalFov);
        put(116, active);
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto& peer = peers[i];
        const double blend = std::clamp(static_cast<double>(now - peer.at) / 50., 0., 1.);
        const auto point = peer.previous.p * (1 - blend) + peer.next.p * blend;
        const unsigned offset = header + i * 48;
        memcpy(overlay.data() + offset, peer.id.data(), 16);
        put(offset + 16, point.x);
        put(offset + 24, point.y);
        put(offset + 32, point.z);
    }
    ReleaseSRWLockExclusive(&lock);
    forwardUiPacket(overlay.data(), static_cast<unsigned>(overlay.size()));
    if (nativeMode)
        heavymp_native_frame(nativePeers.data(), static_cast<unsigned>(nativePeers.size()),
                             available && !paused);
    wchar_t debug[2]{};
    if (GetEnvironmentVariableW(L"HEAVYMP_RENDER_DEBUG", debug, 2)) {
        char line[180];
        sprintf_s(
            line,
            "[HeavyMP render diagnostic] camera=%u available=%u paused=%u head=%u configured=%u peers=%zu",
            camera.active, available, paused, haveHead, headConfigured, peers.size());
        log(line);
    }
}
void heavymp_vehicles_stop() {
    stopReceiver();
    if (nativeMode)
        heavymp_native_stop();
    enabled = false;
    if (presentAddress)
        MH_DisableHook(presentAddress);
    AcquireSRWLockExclusive(&lock);
    camera = {};
    peers.clear();
    trailerPeers.clear();
    configurations.clear();
    releaseRenderer();
    ReleaseSRWLockExclusive(&lock);
    if (input != INVALID_SOCKET) {
        closesocket(input);
        input = INVALID_SOCKET;
    }
}
HeavyMpRenderStatus heavymp_vehicles_status() {
    if (nativeMode)
        return {heavymp_native_active(), heavymp_native_count() > 0, heavymp_native_count(), "native"};
    AcquireSRWLockShared(&lock);
    const bool active = camera.active && GetTickCount64() - camera.at <= 500;
    ReleaseSRWLockShared(&lock);
    return {enabled.load(), active, GetTickCount64() - lastDrawAt.load() < 500 ? lastDrawn.load() : 0};
}
