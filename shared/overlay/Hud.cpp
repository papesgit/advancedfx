#include "Hud.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#endif

#include "radar/Radar.h"
#ifdef HUD_USE_NANOSVG
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#include "third_party/nanosvg/nanosvgrast.h"
#endif

namespace Hud {

static ImVec2 Centered(const ImVec2& start, const ImVec2& size, float w, float h) {
    return ImVec2(start.x + (size.x - w) * 0.5f, start.y + (size.y - h) * 0.5f);
}

static void DrawOutlinedText(ImDrawList* dl, ImVec2 p, ImU32 col, const char* txt, float outlineAlpha = 0.75f) {
    ImU32 oc = (col & 0x00FFFFFF) | (ImU32)(outlineAlpha * 255) << 24; // same rgb, dark alpha
    dl->AddText(ImVec2(p.x+1, p.y+1), oc, txt);
    dl->AddText(p, col, txt);
}

static inline ImU32 MakeRGBA(int r, int g, int b, int a = 255) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return IM_COL32(r, g, b, a);
}

static inline ImU32 Brighten(ImU32 c, float factor) {
    int a = (int)((c >> 24) & 0xFF);
    int b = (int)((c >> 16) & 0xFF);
    int g = (int)((c >> 8) & 0xFF);
    int r = (int)(c & 0xFF);
    r = (int)(r * factor); if (r > 255) r = 255;
    g = (int)(g * factor); if (g > 255) g = 255;
    b = (int)(b * factor); if (b > 255) b = 255;
    return IM_COL32(r, g, b, a);
}

#ifdef _WIN32
static ID3D11Device* g_hudIconDevice = nullptr;
static std::wstring g_hudIconsDir; // ends with backslash
struct IconTex { ID3D11ShaderResourceView* srv=nullptr; int w=0; int h=0; };
static std::unordered_map<std::string, IconTex> g_iconCache;

void SetIconDevice(void* d3d11DevicePtr) { g_hudIconDevice = (ID3D11Device*)d3d11DevicePtr; }
void SetIconsDirectoryW(const std::wstring& dir) {
    g_hudIconsDir = dir;
    if (!g_hudIconsDir.empty()) {
        wchar_t c = g_hudIconsDir.back(); if (c != L'\\' && c != L'/') g_hudIconsDir += L"\\";
    }
}

static IconTex GetIcon(const char* name) {
    auto it = g_iconCache.find(name);
    if (it != g_iconCache.end()) return it->second;
    IconTex tex;
    if (!g_hudIconDevice || g_hudIconsDir.empty()) { g_iconCache[name] = tex; return tex; }
    std::wstring wname(name, name + strlen(name));
    for (auto &ch : wname) if (ch == L'/') ch = L'\\'; // normalize separators on Windows
    std::wstring pngPath = g_hudIconsDir + wname + L".png";
    if (Radar::LoadTextureWIC(g_hudIconDevice, pngPath, &tex.srv, &tex.w, &tex.h)) {
        g_iconCache[name] = tex;
    } else {
        // Try SVG via NanoSVG (optional)
        IconTex svgTex{};
        std::wstring svgPath = g_hudIconsDir + wname + L".svg";
        DWORD attr = GetFileAttributesW(svgPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
#ifdef HUD_USE_NANOSVG
            // Read SVG file using wide path to avoid locale issues, then parse from memory
            auto ReadFileBinaryW = [](const std::wstring& path, std::string& out)->bool {
                FILE* f = _wfopen(path.c_str(), L"rb");
                if (!f) return false;
                if (0 != fseek(f, 0, SEEK_END)) { fclose(f); return false; }
                long sz = ftell(f); if (sz < 0) { fclose(f); return false; }
                rewind(f);
                out.clear(); out.resize((size_t)sz);
                size_t rd = fread(out.data(), 1, (size_t)sz, f);
                fclose(f);
                if (rd != (size_t)sz) return false;
                // Ensure 0-terminated for NanoSVG parser
                out.push_back('\0');
                return true;
            };
            std::string svgText;
            if (ReadFileBinaryW(svgPath, svgText)) {
                NSVGimage* img = nsvgParse(svgText.data(), "px", 96.0f);
            if (img) {
                float maxDim = (img->width > img->height) ? img->width : img->height;
                float target = 64.0f; // base raster size
                float scale = (maxDim > 0.0f) ? (target / maxDim) : 1.0f;
                int rw = (int)ceilf(img->width * scale);
                int rh = (int)ceilf(img->height * scale);
                if (rw < 1) rw = 1; if (rh < 1) rh = 1;
                std::vector<unsigned char> rgba((size_t)rw * (size_t)rh * 4u);
                NSVGrasterizer* rast = nsvgCreateRasterizer();
                if (rast) {
                    nsvgRasterize(rast, img, 0.0f, 0.0f, scale, rgba.data(), rw, rh, rw*4);
                    nsvgDeleteRasterizer(rast);
                    // Create D3D11 texture
                    D3D11_TEXTURE2D_DESC desc{}; desc.Width=rw; desc.Height=rh; desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA init{}; init.pSysMem=rgba.data(); init.SysMemPitch=rw*4;
                    ID3D11Texture2D* tex2d=nullptr; if (SUCCEEDED(g_hudIconDevice->CreateTexture2D(&desc, &init, &tex2d)) && tex2d) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC sdesc{}; sdesc.Format=desc.Format; sdesc.ViewDimension= D3D11_SRV_DIMENSION_TEXTURE2D; sdesc.Texture2D.MostDetailedMip=0; sdesc.Texture2D.MipLevels=1;
                        ID3D11ShaderResourceView* srv=nullptr; if (SUCCEEDED(g_hudIconDevice->CreateShaderResourceView(tex2d, &sdesc, &srv)) && srv) {
                            svgTex.srv = srv; svgTex.w = rw; svgTex.h = rh;
                        }
                        tex2d->Release();
                    }
                }
                nsvgDelete(img);
                }
            }
#endif
        }
        g_iconCache[name] = svgTex;
    }
    return g_iconCache[name];
}

static void DrawIcon(ImDrawList* dl, const char* name, ImVec2 tl, float height, ImU32 tint = IM_COL32(255,255,255,255)) {
    IconTex t = GetIcon(name);
    if (!t.srv) return;
    float w = height;
    if (t.h > 0) w = height * (float)t.w / (float)t.h; // keep aspect ratio
    dl->AddImage((ImTextureID)t.srv, tl, ImVec2(tl.x + w, tl.y + height), ImVec2(0,0), ImVec2(1,1), tint);
}
#else
void SetIconDevice(void*) {}
void SetIconsDirectoryW(const std::wstring&) {}
static void DrawIcon(ImDrawList*, const char*, ImVec2, float, ImU32 = 0) {}
#endif

void RenderTopBar(ImDrawList* dl, const Viewport& vp, const State& st) {
    if (!dl) return;
    // Sizing
    const float barW = vp.size.x * 0.50f;
    const float barH = 26.0f;
    ImVec2 pos = ImVec2(vp.min.x + (vp.size.x - barW) * 0.5f, vp.min.y + 16.0f);
    ImVec2 br  = ImVec2(pos.x + barW, pos.y + barH);

    // Background
    dl->AddRectFilled(pos, br, IM_COL32(20,20,24,190), 6.0f);
    dl->AddRect(pos, br, IM_COL32(255,255,255,40), 6.0f);

    // Split into left / center / right
    float pad = 8.0f;
    float third = barW / 3.0f;
    ImVec2 l0 = ImVec2(pos.x + pad, pos.y + pad);
    ImVec2 l1 = ImVec2(pos.x + third - pad, pos.y + barH - pad);
    ImVec2 c0 = ImVec2(pos.x + third + pad, pos.y + pad);
    ImVec2 c1 = ImVec2(pos.x + 2*third - pad, pos.y + barH - pad);
    ImVec2 r0 = ImVec2(pos.x + 2*third + pad, pos.y + pad);
    ImVec2 r1 = ImVec2(pos.x + barW - pad, pos.y + barH - pad);

    // Scores (left and right)
    char sbuf[64];
    snprintf(sbuf, sizeof(sbuf), "%s  %d", st.leftTeam.name.c_str(), st.leftTeam.score);
    DrawOutlinedText(dl, ImVec2(l0.x, l0.y), st.leftTeam.color, sbuf);

    snprintf(sbuf, sizeof(sbuf), "%d  %s", st.rightTeam.score, st.rightTeam.name.c_str());
    ImVec2 size = ImGui::CalcTextSize(sbuf);
    DrawOutlinedText(dl, ImVec2(r1.x - size.x, r0.y), st.rightTeam.color, sbuf);

    // Center clock + round + bomb
    int tl = (std::max)(0, (int)std::ceil(st.round.timeLeft));
    int mm = tl / 60, ss = tl % 60;
    char cbuf[128];
    if (st.bomb.isPlanted) {
        int bt = (std::max)(0, (int)std::ceil(st.bomb.countdownSec));
        int bm = bt / 60, bs = bt % 60;
        snprintf(cbuf, sizeof(cbuf), "%02d:%02d  R%d   BOMB %s %d:%02d",
                 mm, ss, st.round.number, st.bomb.isDefusing ? "DEF" : "PLT", bm, bs);
    } else {
        snprintf(cbuf, sizeof(cbuf), "%02d:%02d  R%d", mm, ss, st.round.number);
    }
    ImVec2 csz = ImGui::CalcTextSize(cbuf);
    DrawOutlinedText(dl, ImVec2(c0.x + (c1.x - c0.x - csz.x) * 0.5f, c0.y), IM_COL32(255,255,255,255), cbuf);
}

static void RenderPlayerRow(ImDrawList* dl, ImVec2 pos, ImVec2 size, const Player& p, bool alignRight, ImU32 teamColor, char labelDigit = 0) {
    // Background tile
    ImU32 bg;
    if (p.isAlive) {
        // Custom team colors with full alpha
        // CT (3): RGB(24,35,120), T (2): RGB(120,66,24)
        ImU32 base = (p.teamSide == 3) ? MakeRGBA(7, 63, 184, 255)
                      : (p.teamSide == 2) ? MakeRGBA(184, 78, 7, 255)
                      : IM_COL32(28,32,39,255);
        // Focused: increase luminance ~30% (approximate by scaling channels)
        bg = p.isFocused ? Brighten(base, 1.5f) : base;
    } else {
        // Dim when dead
        bg = IM_COL32(20,22,26,160);
    }
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(255,255,255,40), 4.0f);

    // Number label circle at outer edge (half outside, half inside)
    float circleRadius = 12.0f;
    float circlePadding = 8.0f; // extra padding for text after circle
    if (labelDigit != 0) {
        ImVec2 circleCenter;
        if (alignRight) {
            // Right sidebar: circle on right edge
            circleCenter = ImVec2(pos.x + size.x, pos.y + size.y * 0.5f);
        } else {
            // Left sidebar: circle on left edge
            circleCenter = ImVec2(pos.x, pos.y + size.y * 0.5f);
        }

        // Draw team-colored circle with outline
        dl->AddCircleFilled(circleCenter, circleRadius, teamColor, 24);
        dl->AddCircle(circleCenter, circleRadius, IM_COL32(255,255,255,180), 24, 2.0f);

        // Draw number label centered in circle
        char labelText[2] = { labelDigit, '\0' };
        ImVec2 textSize = ImGui::CalcTextSize(labelText);
        ImVec2 textPos = ImVec2(circleCenter.x - textSize.x * 0.5f, circleCenter.y - textSize.y * 0.5f);
        DrawOutlinedText(dl, textPos, IM_COL32(255,255,255,255), labelText, 1.0f);
    }

    // Health bar
    float pad = 6.0f;
    // Add extra padding if we have a label circle
    float textPadLeft = pad + (labelDigit != 0 && !alignRight ? circleRadius + circlePadding : 0);
    float textPadRight = pad + (labelDigit != 0 && alignRight ? circleRadius + circlePadding : 0);

    float hbH = 6.0f;
    float hbW = size.x - pad*2;
    ImVec2 hb0 = ImVec2(pos.x + pad, pos.y + size.y - pad - hbH);
    ImVec2 hb1 = ImVec2(hb0.x + hbW, hb0.y + hbH);
    dl->AddRectFilled(hb0, hb1, IM_COL32(36,40,48,255), 2.0f);
    float frac = (std::max)(0, (std::min)(100, p.health)) / 100.0f;
    dl->AddRectFilled(hb0, ImVec2(hb0.x + hbW * frac, hb1.y), IM_COL32(0,180,80,220), 2.0f);

    // Name and money
    char nbuf[256]; snprintf(nbuf, sizeof(nbuf), "%s  $%d", p.name.c_str(), p.money);
    ImVec2 nsz = ImGui::CalcTextSize(nbuf);
    ImVec2 namePos;
    if (alignRight) namePos = ImVec2(pos.x + size.x - textPadRight - nsz.x, pos.y + pad);
    else            namePos = ImVec2(pos.x + textPadLeft, pos.y + pad);
    DrawOutlinedText(dl, namePos, IM_COL32(255,255,255,255), nbuf);

    // Active weapon icon near the name (if available)
    if (!p.active.name.empty()) {
        IconTex wt = GetIcon((std::string("weapons/") + p.active.name).c_str());
        float hIcon = 16.0f * 1.3f; // 30% bigger in sidebars
        float wIcon = (wt.srv && wt.h > 0) ? (hIcon * (float)wt.w / (float)wt.h) : hIcon;
        float yIcon = pos.y + pad - 1.0f;
        // Draw icon to the side of the name text without overlapping
        if (alignRight) {
            float xIcon = namePos.x - 6.0f - wIcon; // to the left of right-aligned name
            if (xIcon > pos.x + pad)
                DrawIcon(dl, (std::string("weapons/") + p.active.name).c_str(), ImVec2(xIcon, yIcon), hIcon);
        } else {
            float xIcon = namePos.x + nsz.x + 6.0f; // to the right of left-aligned name
            if (xIcon + wIcon < pos.x + size.x - pad)
                DrawIcon(dl, (std::string("weapons/") + p.active.name).c_str(), ImVec2(xIcon, yIcon), hIcon);
        }
    }

    // K/D and armor
    char kdbuf[64]; snprintf(kdbuf, sizeof(kdbuf), "%d/%d  %dAR", p.kills, p.deaths, p.armor);
    ImVec2 kdsz = ImGui::CalcTextSize(kdbuf);
    if (alignRight) DrawOutlinedText(dl, ImVec2(pos.x + size.x - textPadRight - kdsz.x, pos.y + pad + nsz.y + 2), IM_COL32(200,200,200,220), kdbuf);
    else            DrawOutlinedText(dl, ImVec2(pos.x + textPadLeft, pos.y + pad + nsz.y + 2), IM_COL32(200,200,200,220), kdbuf);

    // Status icons (C4/defuser/helmet)
    float iconSize = 14.0f * 1.3f; // 30% bigger
    float iconY = pos.y + pad - 2.0f;
    auto drawIconOrTextW = [&](const char* iconKey, const char* textFallback, ImVec2 at)->float {
        IconTex t = GetIcon(iconKey);
        if (t.srv) {
            float w = iconSize;
            if (t.h > 0) w = iconSize * (float)t.w / (float)t.h;
            DrawIcon(dl, iconKey, at, iconSize);
            return w;
        } else {
            DrawOutlinedText(dl, at, IM_COL32(230,230,230,220), textFallback);
            ImVec2 tsz = ImGui::CalcTextSize(textFallback);
            return tsz.x;
        }
    };

    if (alignRight) {
        float x = pos.x + textPadRight;
        if (p.hasBomb) { x += drawIconOrTextW("weapons/c4", "C4", ImVec2(x, iconY)) + 4.0f; }
        if (p.hasDefuser && p.teamSide==3) { x += drawIconOrTextW("weapons/defuser", "DEF", ImVec2(x, iconY)) + 4.0f; }
        if (p.hasHelmet) { x += drawIconOrTextW("icons/armor-helmet", "H", ImVec2(x, iconY)) + 4.0f; }
    } else {
        float x = pos.x + size.x - textPadLeft;
        auto drawRight = [&](const char* ik, const char* tf){
            IconTex t = GetIcon(ik);
            float w = iconSize;
            if (t.srv && t.h > 0) w = iconSize * (float)t.w / (float)t.h;
            x -= w;
            if (t.srv) DrawIcon(dl, ik, ImVec2(x, iconY), iconSize);
            else { ImVec2 ts = ImGui::CalcTextSize(tf); x -= (ts.x - w); DrawOutlinedText(dl, ImVec2(x, iconY), IM_COL32(230,230,230,220), tf); w = ts.x; }
            x -= 4.0f;
        };
        if (p.hasHelmet) drawRight("icons/armor-helmet", "H");
        if (p.hasDefuser && p.teamSide==3) drawRight("weapons/defuser", "DEF");
        if (p.hasBomb) drawRight("weapons/c4", "C4");
    }

    // Grenades line at bottom
    if (!p.grenades.empty()) {
        float gy = pos.y + size.y - pad - iconSize;
        if (alignRight) {
            float gx = pos.x + pad;
            for (const auto& g : p.grenades) {
                std::string key = std::string("weapons/") + g;
                IconTex t = GetIcon(key.c_str());
                float w = iconSize;
                if (t.srv && t.h > 0) w = iconSize * (float)t.w / (float)t.h;
                if (t.srv) DrawIcon(dl, key.c_str(), ImVec2(gx, gy), iconSize);
                else {
                    const char* tf = (g=="flashbang"?"F": g=="smokegrenade"?"S": g=="molotov"?"M": g=="hegrenade"?"H": g=="decoy"?"D":"?");
                    DrawOutlinedText(dl, ImVec2(gx, gy), IM_COL32(200,200,200,220), tf);
                    ImVec2 ts = ImGui::CalcTextSize(tf); w = ts.x;
                }
                gx += w + 4.0f;
            }
        } else {
            float gx = pos.x + size.x - pad;
            for (const auto& g : p.grenades) {
                std::string key = std::string("weapons/") + g;
                IconTex t = GetIcon(key.c_str());
                float w = iconSize;
                if (t.srv && t.h > 0) w = iconSize * (float)t.w / (float)t.h;
                gx -= w;
                if (t.srv) DrawIcon(dl, key.c_str(), ImVec2(gx, gy), iconSize);
                else {
                    const char* tf = (g=="flashbang"?"F": g=="smokegrenade"?"S": g=="molotov"?"M": g=="hegrenade"?"H": g=="decoy"?"D":"?");
                    ImVec2 ts = ImGui::CalcTextSize(tf);
                    gx -= (ts.x - w);
                    DrawOutlinedText(dl, ImVec2(gx, gy), IM_COL32(200,200,200,220), tf);
                    w = ts.x;
                }
                gx -= 4.0f;
            }
        }
    }

    // Strike-through if dead
    if (!p.isAlive) {
        ImVec2 line0 = ImVec2(pos.x + 4, pos.y + size.y * 0.5f);
        ImVec2 line1 = ImVec2(pos.x + size.x - 4, pos.y + size.y * 0.5f);
        dl->AddLine(line0, line1, IM_COL32(200,80,80,200), 2.0f);
    }
}

void RenderSidebars(ImDrawList* dl, const Viewport& vp, const State& st, std::vector<PlayerRowRect>* outRects) {
    if (!dl) return;
    if (outRects) outRects->clear();

    const float barW = (std::min)(360.0f, vp.size.x * 0.20f);
    const float rowH = 55.0f;
    const float gap  = 6.0f;
    const float headerH = 28.0f;
    const int count  = (int)(std::max)(st.leftPlayers.size(), st.rightPlayers.size());
    const float totalH = headerH + 6.0f + (count*rowH + (count? (count-1)*gap : 0));
    const float marginY = 24.0f;
    const float yStart = vp.min.y + vp.size.y - marginY - totalH;

    // Left
    float y = yStart;
    {
        ImVec2 h0 = ImVec2(vp.min.x + 16.0f, y);
        ImVec2 h1 = ImVec2(h0.x + barW, h0.y + headerH);
        dl->AddRectFilled(h0, h1, IM_COL32(28,32,39,220), 4.0f);
        dl->AddRect(h0, h1, IM_COL32(255,255,255,40), 4.0f);
        DrawOutlinedText(dl, ImVec2(h0.x + 8, h0.y + 6), st.leftTeam.color, st.leftTeam.name.c_str());
        y = h1.y + 6.0f;
    }
    // Left sidebar: labels 1-5
    const char leftDigits[5] = {'1', '2', '3', '4', '5'};
    size_t leftIdx = 0;
    for (const auto& p : st.leftPlayers) {
        char labelDigit = (leftIdx < 5) ? leftDigits[leftIdx] : 0;
        ImVec2 pos = ImVec2(vp.min.x + 16.0f, y);
        RenderPlayerRow(dl, pos, ImVec2(barW, rowH), p, false, st.leftTeam.color, labelDigit);

        if (outRects) {
            PlayerRowRect rect;
            rect.min = pos;
            rect.max = ImVec2(pos.x + barW, pos.y + rowH);
            rect.observerSlot = p.observerSlot;
            rect.playerId = p.id;
            outRects->push_back(rect);
        }

        y += rowH + gap;
        ++leftIdx;
    }

    // Right
    float yR = yStart;
    float rightX = vp.min.x + vp.size.x - 16.0f - barW;
    {
        ImVec2 h0 = ImVec2(rightX, yR);
        ImVec2 h1 = ImVec2(h0.x + barW, h0.y + headerH);
        dl->AddRectFilled(h0, h1, IM_COL32(28,32,39,220), 4.0f);
        dl->AddRect(h0, h1, IM_COL32(255,255,255,40), 4.0f);
        ImVec2 ts = ImGui::CalcTextSize(st.rightTeam.name.c_str());
        DrawOutlinedText(dl, ImVec2(h1.x - 8 - ts.x, h0.y + 6), st.rightTeam.color, st.rightTeam.name.c_str());
        yR = h1.y + 6.0f;
    }
    // Right sidebar: labels 6-9, 0
    const char rightDigits[5] = {'6', '7', '8', '9', '0'};
    size_t rightIdx = 0;
    for (const auto& p : st.rightPlayers) {
        char labelDigit = (rightIdx < 5) ? rightDigits[rightIdx] : 0;
        ImVec2 pos = ImVec2(rightX, yR);
        RenderPlayerRow(dl, pos, ImVec2(barW, rowH), p, true, st.rightTeam.color, labelDigit);

        if (outRects) {
            PlayerRowRect rect;
            rect.min = pos;
            rect.max = ImVec2(pos.x + barW, pos.y + rowH);
            rect.observerSlot = p.observerSlot;
            rect.playerId = p.id;
            outRects->push_back(rect);
        }

        yR += rowH + gap;
        ++rightIdx;
    }
}

void RenderFocusedPlayer(ImDrawList* dl, const Viewport& vp, const State& st) {
    if (!dl) return;
    const Player* fp = nullptr;
    for (auto& p : st.leftPlayers) if (p.id == st.focusedPlayerId) { fp = &p; break; }
    if (!fp) for (auto& p : st.rightPlayers) if (p.id == st.focusedPlayerId) { fp = &p; break; }
    if (!fp) return;

    const float panelW = vp.size.x * 0.25f;
    const float panelH = 72.0f;
    ImVec2 pos = ImVec2(vp.min.x + (vp.size.x - panelW) * 0.5f, vp.min.y + vp.size.y - panelH - 24.0f);
    ImVec2 br  = ImVec2(pos.x + panelW, pos.y + panelH);
    dl->AddRectFilled(pos, br, IM_COL32(20,20,24,200), 8.0f);
    dl->AddRect(pos, br, IM_COL32(255,255,255,40), 8.0f);

    // Name
    DrawOutlinedText(dl, ImVec2(pos.x + 12, pos.y + 10), IM_COL32(255,255,255,255), fp->name.c_str());

    // Active weapon icon (top-right anchor)
    if (!fp->active.name.empty()) {
        std::string key = std::string("weapons/") + fp->active.name;
        IconTex it = GetIcon(key.c_str());
        float hIcon = 20.0f;
        float wIcon = (it.srv && it.h > 0) ? (hIcon * (float)it.w / (float)it.h) : hIcon;
        float margin = 12.0f;
        ImVec2 at = ImVec2(br.x - margin - wIcon, pos.y + 8.0f);
        DrawIcon(dl, key.c_str(), at, hIcon);
    }

    // Health / Armor bars
    float pad = 12.0f; float barH = 10.0f; float bw = panelW - pad*2.0f;
    ImVec2 hb0 = ImVec2(pos.x + pad, pos.y + panelH - pad - barH*2 - 4);
    ImVec2 hb1 = ImVec2(hb0.x + bw, hb0.y + barH);
    dl->AddRectFilled(hb0, hb1, IM_COL32(36,40,48,255), 3.0f);
    dl->AddRectFilled(hb0, ImVec2(hb0.x + bw * (std::max)(0, (std::min)(100, fp->health)) / 100.0f, hb1.y), IM_COL32(200,40,40,230), 3.0f);
    char htxt[32]; snprintf(htxt, sizeof(htxt), "%d HP", fp->health);
    ImVec2 hsz = ImGui::CalcTextSize(htxt);
    DrawOutlinedText(dl, ImVec2(hb0.x + (bw - hsz.x)*0.5f, hb0.y - hsz.y - 2), IM_COL32(255,255,255,220), htxt);

    ImVec2 ab0 = ImVec2(pos.x + pad, pos.y + panelH - pad - barH);
    ImVec2 ab1 = ImVec2(ab0.x + bw, ab0.y + barH);
    // Helmet/Defuser indicators (icons)
    float tx = ab1.x - 34.0f;
    float ty = ab0.y - 18.0f;
    if (fp->hasHelmet) { DrawIcon(dl, "icons/armor-helmet", ImVec2(tx, ty), 16.0f); tx -= 20.0f; }
    if (fp->hasDefuser && fp->teamSide==3) { DrawIcon(dl, "weapons/defuser", ImVec2(tx, ty), 16.0f); tx -= 20.0f; }
}

} // namespace Hud
