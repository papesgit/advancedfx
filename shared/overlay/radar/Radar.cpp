#include "Radar.h"

#ifdef _WIN32
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#endif

#include <algorithm>
#include <fstream>
#include <sstream>

#include "../third_party/nlohmann/json.hpp"

using nlohmann::json;

// Local PI constant to avoid depending on ImGui's IM_PI macro presence
static constexpr float RADAR_PI = 3.14159265358979323846f;

namespace Radar {

static std::string GetFilename(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p+1);
}

bool LoadRadarsJson(const std::string& jsonPath,
                    std::unordered_map<std::string, RadarConfig>& outConfigs,
                    const std::string& imageBaseDir) {
    outConfigs.clear();

    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) return false;

    std::stringstream buf; buf << f.rdbuf();
    std::string text = buf.str();

    json j = json::parse(text);
    if (!j.is_object()) return false;

    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string mapName = it.key();
        const json& v = it.value();
        if (!v.is_object()) continue;
        RadarConfig cfg;
        if (v.contains("pos_x")) cfg.pos_x = v["pos_x"].is_number() ? v["pos_x"].get<double>() : std::atof(v["pos_x"].get<std::string>().c_str());
        if (v.contains("pos_y")) cfg.pos_y = v["pos_y"].is_number() ? v["pos_y"].get<double>() : std::atof(v["pos_y"].get<std::string>().c_str());
        if (v.contains("scale")) cfg.scale = v["scale"].is_number() ? v["scale"].get<double>() : std::atof(v["scale"].get<std::string>().c_str());

        // Optional vertical sections
        if (v.contains("verticalsections") && v["verticalsections"].is_object()) {
            const json& vs = v["verticalsections"];
            for (auto it2 = vs.begin(); it2 != vs.end(); ++it2) {
                VerticalSection s; s.name = it2.key();
                const json& sec = it2.value();
                if (sec.contains("AltitudeMin")) s.min = sec["AltitudeMin"].is_number() ? sec["AltitudeMin"].get<double>() : std::atof(sec["AltitudeMin"].get<std::string>().c_str());
                if (sec.contains("AltitudeMax")) s.max = sec["AltitudeMax"].is_number() ? sec["AltitudeMax"].get<double>() : std::atof(sec["AltitudeMax"].get<std::string>().c_str());
                cfg.sections.push_back(std::move(s));
            }
            std::sort(cfg.sections.begin(), cfg.sections.end(), [](const VerticalSection& a, const VerticalSection& b){ return a.min > b.min; });
        }

        // Optional image from URL: use filename and join with base dir
        if (!imageBaseDir.empty() && v.contains("radarImageUrl") && v["radarImageUrl"].is_string()) {
            std::string url = v["radarImageUrl"].get<std::string>();
            std::string fn = GetFilename(url);
            if (!fn.empty()) {
                cfg.imagePath = imageBaseDir;
                if (!cfg.imagePath.empty() && cfg.imagePath.back() != '/' && cfg.imagePath.back() != '\\') cfg.imagePath.push_back('/');
                cfg.imagePath += fn;
            }
        }

        outConfigs[mapName] = std::move(cfg);
    }

    return !outConfigs.empty();
}

bool LoadTextureWIC(ID3D11Device* device,
                    const std::wstring& filePath,
                    ID3D11ShaderResourceView** outSrv,
                    int* outWidth,
                    int* outHeight) {
#ifdef _WIN32
    if (!device || !outSrv) return false;
    *outSrv = nullptr;

    CoInitialize(nullptr);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { factory->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { decoder->Release(); factory->Release(); return false; }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); decoder->Release(); factory->Release(); return false; }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { converter->Release(); frame->Release(); decoder->Release(); factory->Release(); return false; }

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    std::vector<BYTE> pixels(width * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();

    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels.data();
    init.SysMemPitch = width * 4;

    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&desc, &init, &tex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sdesc = {};
    sdesc.Format = desc.Format;
    sdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sdesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(tex, &sdesc, outSrv);
    tex->Release();
    if (FAILED(hr)) return false;

    if (outWidth) *outWidth = (int)width;
    if (outHeight) *outHeight = (int)height;
    return true;
#else
    (void)device; (void)filePath; (void)outSrv; (void)outWidth; (void)outHeight; return false;
#endif
}

void Render(ImDrawList* dl,
            const ImVec2& topLeft,
            const ImVec2& size,
            Context& ctx,
            const std::vector<Entity>& entities,
            float markerRadius,
            bool drawBorder,
            bool drawBackground) {
    if (!dl) return;

    ImVec2 br = ImVec2(topLeft.x + size.x, topLeft.y + size.y);
    if (drawBackground) {
        if (ctx.srv) {
            dl->AddImage((ImTextureID)ctx.srv, topLeft, br);
        } else {
            dl->AddRectFilled(topLeft, br, IM_COL32(50,50,50,128));
        }
    }
    if (drawBorder) dl->AddRect(topLeft, br, IM_COL32(255,255,255,255));

    for (const auto& e : entities) {
        Vec3 p = e.pos;
        if (e.smooth) {
            auto& s = ctx.smooth[e.id];
            s.push(p);
            p = s.avg();
        }
        Vec2 uv = WorldToUV(p, ctx.cfg);
        ImVec2 pt = ImVec2(topLeft.x + uv.x * size.x, topLeft.y + uv.y * size.y);
        dl->AddCircleFilled(pt, markerRadius, e.color);

        // facing indicator: white quarter-circle wedge outline on top of the dot
        float yaw = YawFromForward(e.fwd) * 3.14159265f / 180.0f;
        float theta = yaw - RADAR_PI * 0.5f;       // convert to ImGui angle basis (+X axis, CCW)
        float half  = RADAR_PI * 0.25f;            // ±45° around facing direction
        dl->PathClear();
        dl->PathArcTo(pt, markerRadius, theta - half, theta + half, 12);
        dl->PathLineTo(pt);
        dl->PathFillConvex(IM_COL32(255,255,255,255));
        // mask inner area with a smaller team-colored dot to leave only an outer ring wedge
        float wedgeOutline = markerRadius * 0.25f; if (wedgeOutline < 1.0f) wedgeOutline = 1.0f; if (wedgeOutline > markerRadius*0.9f) wedgeOutline = markerRadius*0.9f;
        float innerR = markerRadius - wedgeOutline;
        if (innerR > 0.5f) dl->AddCircleFilled(pt, innerR, e.color);
    }
}

} // namespace Radar
