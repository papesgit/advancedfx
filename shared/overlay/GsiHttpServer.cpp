#include "GsiHttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <algorithm>
#include <sstream>

using nlohmann::json;

static constexpr const char* kExpectedUserAgentPrefix = "Valve/Steam HTTP Client";
static constexpr const char* kExpectedAuthToken = "7ATvXUzTfBYyMLrA";

GsiHttpServer::GsiHttpServer() {}
GsiHttpServer::~GsiHttpServer() { Stop(); }

bool GsiHttpServer::Start(int port) {
    Stop();
    running_.store(true);
    try {
        thr_ = std::thread(&GsiHttpServer::ThreadMain, this, port);
    } catch (...) { running_.store(false); return false; }
    return true;
}

void GsiHttpServer::Stop() {
    if (!running_.exchange(false)) return;
    if (thr_.joinable()) thr_.join();
}

std::optional<Hud::State> GsiHttpServer::TryGetHudState() {
    std::lock_guard<std::mutex> lk(mtx_);
    return hud_;
}

static bool ParseVec3Csv(const std::string& s, float out[3]) {
    // expected format: "x, y, z"
    out[0] = out[1] = out[2] = 0.0f;
    if (s.empty()) return false;
    // simple parser
    double vals[3] = {0,0,0};
    int count = 0;
    size_t start = 0;
    for (;;) {
        size_t comma = s.find(',', start);
        std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos : (comma - start));
        // trim spaces
        size_t b = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (b == std::string::npos) tok.clear(); else tok = tok.substr(b, e - b + 1);
        if (!tok.empty()) {
            vals[count] = std::atof(tok.c_str());
        }
        count++;
        if (comma == std::string::npos || count >= 3) break;
        start = comma + 1;
    }
    if (count < 3) return false;
    out[0] = (float)vals[0]; out[1] = (float)vals[1]; out[2] = (float)vals[2];
    return true;
}

// Helper to check if a player name should be filtered out based on comma-separated filter list
static bool IsPlayerFiltered(const std::string& playerName, const std::string& filterList) {
    if (filterList.empty() || playerName.empty()) return false;

    size_t start = 0;
    for (;;) {
        size_t comma = filterList.find(',', start);
        std::string filterName = filterList.substr(start, comma == std::string::npos ? std::string::npos : (comma - start));
        // Trim spaces
        size_t b = filterName.find_first_not_of(" \t");
        size_t e = filterName.find_last_not_of(" \t");
        if (b != std::string::npos) {
            filterName = filterName.substr(b, e - b + 1);
            // Case-insensitive comparison
            if (filterName.length() == playerName.length() &&
                std::equal(filterName.begin(), filterName.end(), playerName.begin(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
                return true; // Player is filtered
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

std::optional<std::vector<GsiHttpServer::RadarPlayer>> GsiHttpServer::TryGetRadarPlayers() {
    std::lock_guard<std::mutex> lk(mtx_);
    return radar_players_;
}

std::optional<std::vector<GsiHttpServer::RadarGrenade>> GsiHttpServer::TryGetRadarGrenades() {
    std::lock_guard<std::mutex> lk(mtx_);
    return radar_grenades_;
}

std::optional<GsiHttpServer::RadarBomb> GsiHttpServer::TryGetRadarBomb() {
    std::lock_guard<std::mutex> lk(mtx_);
    return radar_bomb_;
}

std::optional<std::string> GsiHttpServer::TryGetMapName() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!gsi_.is_object() || !gsi_.contains("map")) return std::nullopt;
    const json& map = gsi_["map"];
    if (map.contains("name") && map["name"].is_string()) {
        return map["name"].get<std::string>();
    }
    return std::nullopt;
}

void GsiHttpServer::RebuildRadarSnapshotFromGsi() {
    // Snapshot gsi_ and filter under lock
    json gsi;
    std::string filterList;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gsi = gsi_;
        filterList = filtered_players_;
    }

    std::vector<RadarPlayer> outPlayers;
    std::vector<RadarGrenade> outGrenades;
    RadarBomb outBomb{};

    if (gsi.is_object()) {
        // Players
        auto allIt = gsi.find("allplayers");
        if (allIt != gsi.end() && allIt->is_object()) {
            const json& all = *allIt;
            for (auto it = all.begin(); it != all.end(); ++it) {
                const std::string steam = it.key();
                const json& pl = it.value();
                if (!pl.is_object()) continue;

                // Check if player should be filtered
                std::string playerName;
                if (pl.contains("name") && pl["name"].is_string()) playerName = pl["name"].get<std::string>();
                if (IsPlayerFiltered(playerName, filterList)) continue; // Skip filtered players

                RadarPlayer rp{};
                rp.id = (int)std::hash<std::string>{}(steam);
                if (pl.contains("team")) {
                    std::string teamStr = pl.find("team")!=pl.end() && pl["team"].is_string() ? pl["team"].get<std::string>() : std::string();
                    rp.teamSide = (teamStr == "CT") ? 3 : 2;
                }
                if (pl.contains("observer_slot")) {
                    auto os = pl.find("observer_slot");
                    if (os != pl.end() && os->is_number()) rp.observerSlot = os->get<int>();
                }
                // Alive state
                if (pl.contains("state")) {
                    auto stIt = pl.find("state");
                    if (stIt != pl.end() && stIt->is_object()) {
                        const json& st = *stIt;
                        auto hIt = st.find("health");
                        if (hIt != st.end() && hIt->is_number()) rp.alive = ((int)hIt->get<double>()) > 0;
                    }
                }
                // Carrying bomb?
                auto wIt = pl.find("weapons");
                if (wIt != pl.end() && wIt->is_object()) {
                    const json& ws = *wIt;
                    for (auto wit = ws.begin(); wit != ws.end(); ++wit) {
                        const json& w = wit.value();
                        auto tIt = w.find("type");
                        if (tIt != w.end() && tIt->is_string()) {
                            std::string t = tIt->get<std::string>();
                            if (t == "C4") { rp.hasBomb = true; break; }
                        }
                    }
                }
                // Position and forward
                bool okPos = false;
                auto posIt = pl.find("position");
                if (posIt != pl.end() && posIt->is_string()) {
                    float v[3]; if (ParseVec3Csv(posIt->get<std::string>(), v)) {
                        rp.pos[0] = v[0]; rp.pos[1] = v[1]; rp.pos[2] = v[2]; okPos = true;
                    }
                }
                auto fIt = pl.find("forward");
                if (fIt != pl.end() && fIt->is_string()) {
                    float v[3]; if (ParseVec3Csv(fIt->get<std::string>(), v)) {
                        rp.fwd[0] = v[0]; rp.fwd[1] = v[1];
                    }
                }
                if (okPos) outPlayers.emplace_back(rp);
            }
        }

        // Grenades
        auto gIt = gsi.find("grenades");
        if (gIt != gsi.end() && gIt->is_object()) {
            // helper to get owner side from steam64 id
            auto ownerSideFromSteam = [&](const std::string& steam)->int{
                auto apIt = gsi.find("allplayers");
                if (apIt != gsi.end() && apIt->is_object()) {
                    auto it = apIt->find(steam);
                    if (it != apIt->end() && it->is_object()) {
                        const json& pl = *it;
                        auto tIt = pl.find("team");
                        if (tIt != pl.end() && tIt->is_string()) {
                            std::string ts = tIt->get<std::string>();
                            return ts == "CT" ? 3 : 2;
                        }
                    }
                }
                return 0;
            };
            const json& grenades = *gIt;
            for (auto it = grenades.begin(); it != grenades.end(); ++it) {
                const json& gr = it.value();
                if (!gr.is_object()) continue;
                std::string type = gr.contains("type") && gr["type"].is_string() ? gr["type"].get<std::string>() : std::string();
                int ownerSide = 0;
                if (gr.contains("owner") && gr["owner"].is_string()) ownerSide = ownerSideFromSteam(gr["owner"].get<std::string>());

                if (type == "smoke") {
                    bool isDet = false;
                    if (gr.contains("velocity") && gr["velocity"].is_string()) {
                        float v[3]; if (ParseVec3Csv(gr["velocity"].get<std::string>(), v)) {
                            isDet = (v[0]==0.0f && v[1]==0.0f && v[2]==0.0f);
                        }
                    }
                    if (!isDet) continue;
                    float p[3]; if (gr.contains("position") && gr["position"].is_string() && ParseVec3Csv(gr["position"].get<std::string>(), p)) {
                        RadarGrenade rg{}; rg.type = RadarGrenade::Smoke; rg.ownerSide = ownerSide; rg.pos[0]=p[0]; rg.pos[1]=p[1]; rg.pos[2]=p[2];
                        outGrenades.emplace_back(rg);
                    }
                } else if (type == "inferno") {
                    if (gr.contains("flames") && gr["flames"].is_object()) {
                        const json& fl = gr["flames"];
                        for (auto fit = fl.begin(); fit != fl.end(); ++fit) {
                            const json& pos = fit.value();
                            if (pos.is_string()) {
                                float p[3]; if (ParseVec3Csv(pos.get<std::string>(), p)) {
                                    RadarGrenade rg{}; rg.type = RadarGrenade::Inferno; rg.ownerSide = ownerSide; rg.pos[0]=p[0]; rg.pos[1]=p[1]; rg.pos[2]=p[2];
                                    outGrenades.emplace_back(rg);
                                }
                            }
                        }
                    } else if (gr.contains("position") && gr["position"].is_string()) {
                        float p[3]; if (ParseVec3Csv(gr["position"].get<std::string>(), p)) {
                            RadarGrenade rg{}; rg.type = RadarGrenade::Inferno; rg.ownerSide = ownerSide; rg.pos[0]=p[0]; rg.pos[1]=p[1]; rg.pos[2]=p[2];
                            outGrenades.emplace_back(rg);
                        }
                    }
                }
            }
        }

        // Bomb
        auto bIt = gsi.find("bomb");
        if (bIt != gsi.end() && bIt->is_object()) {
            const json& b = *bIt;
            if (b.contains("state") && b["state"].is_string()) outBomb.state = b["state"].get<std::string>();
            if (b.contains("position") && b["position"].is_string()) {
                float p[3]; if (ParseVec3Csv(b["position"].get<std::string>(), p)) { outBomb.hasPosition = true; outBomb.pos[0]=p[0]; outBomb.pos[1]=p[1]; outBomb.pos[2]=p[2]; }
            }
        }
    }

    // Publish under lock
    {
        std::lock_guard<std::mutex> lk(mtx_);
        radar_players_  = std::move(outPlayers);
        radar_grenades_ = std::move(outGrenades);
        radar_bomb_     = outBomb;
    }
}

void GsiHttpServer::ThreadMain(int port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { running_.store(false); return; }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { WSACleanup(); running_.store(false); return; }

    BOOL opt = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0) { closesocket(ls); WSACleanup(); running_.store(false); return; }
    if (listen(ls, 4) != 0) { closesocket(ls); WSACleanup(); running_.store(false); return; }

    while (running_.load()) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(ls, &rfds);
        timeval tv{0, 200*1000}; // 200 ms
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        HandleOneConnection(s);
        closesocket(s);
    }

    closesocket(ls);
    WSACleanup();
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

bool GsiHttpServer::ReadHttpRequest(SocketHandle sh, std::string& method, std::string& path, std::string& headers, std::string& body) {
    SOCKET s = (SOCKET)sh;
    method.clear(); path.clear(); headers.clear(); body.clear();
    std::string req;
    char buf[4096];
    int got = recv(s, buf, sizeof(buf), 0);
    if (got <= 0) return false;
    req.append(buf, buf + got);

    // Find header/body split
    size_t sep = req.find("\r\n\r\n");
    while (sep == std::string::npos) {
        got = recv(s, buf, sizeof(buf), 0);
        if (got <= 0) return false;
        req.append(buf, buf + got);
        sep = req.find("\r\n\r\n");
        if (req.size() > (1<<20)) return false; // guard
    }
    std::string head = req.substr(0, sep);
    body = req.substr(sep + 4);

    // Parse request line
    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string reqLine = head.substr(0, lineEnd);
    {
        std::istringstream is(reqLine);
        is >> method >> path; // ignore HTTP version
    }

    headers = head.substr(lineEnd + 2);

    // Content-Length
    size_t clpos = ToLower(headers).find("content-length:");
    size_t len = 0;
    if (clpos != std::string::npos) {
        size_t lineEnd2 = headers.find("\r\n", clpos);
        std::string val = headers.substr(clpos + strlen("content-length:"), (lineEnd2 == std::string::npos ? headers.size() : lineEnd2) - (clpos + strlen("content-length:")) );
        val.erase(0, val.find_first_not_of(" \t"));
        len = (size_t)std::strtoul(val.c_str(), nullptr, 10);
    }

    while (body.size() < len) {
        got = recv(s, buf, sizeof(buf), 0);
        if (got <= 0) return false;
        body.append(buf, buf + got);
    }
    if (body.size() > len) body.resize(len);

    return true;
}

void GsiHttpServer::SendHttpResponse(SocketHandle sh, int statusCode) {
    SOCKET s = (SOCKET)sh;
    const char* msg = (statusCode == 204) ? "No Content" : (statusCode == 401 ? "Unauthorized" : (statusCode == 400 ? "Bad Request" : "OK"));
    char hdr[256];
    int n = _snprintf_s(hdr, _TRUNCATE, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", statusCode, msg);
    send(s, hdr, n, 0);
}

bool GsiHttpServer::HandleOneConnection(SocketHandle sh) {
    SOCKET s = (SOCKET)sh;
    std::string method, path, headers, body;
    if (!ReadHttpRequest((SocketHandle)s, method, path, headers, body)) return false;
    if (method != "POST" || path != "/gsi") { SendHttpResponse((SocketHandle)s, 400); return false; }

    // User-Agent check (optional lenient)
    auto hl = ToLower(headers);
    size_t uapos = hl.find("user-agent:");
    if (uapos != std::string::npos) {
        size_t lineEnd = headers.find("\r\n", uapos);
        std::string ua = headers.substr(uapos + strlen("user-agent:"), (lineEnd == std::string::npos ? headers.size() : lineEnd) - (uapos + strlen("user-agent:")) );
        // Trim
        while (!ua.empty() && (ua.front()==' '||ua.front()=='\t')) ua.erase(ua.begin());
        if (ua.rfind(kExpectedUserAgentPrefix, 0) != 0) {
            // Allow, but continue (some games might change UA)
        }
    }

    // Parse JSON
    json j;
    try { j = json::parse(body); }
    catch (...) { SendHttpResponse((SocketHandle)s, 400); return false; }

    // Auth token
    try {
        std::string tok = j["auth"]["token"].get<std::string>();
        if (tok != kExpectedAuthToken) { SendHttpResponse((SocketHandle)s, 401); return false; }
    } catch (...) { SendHttpResponse((SocketHandle)s, 401); return false; }

    MergeGsiIntoState(j);
    RebuildHudStateFromGsi();
    RebuildRadarSnapshotFromGsi();

    SendHttpResponse((SocketHandle)s, 204);
    return true;
}

void GsiHttpServer::MergeGsiIntoState(const json& body) {
    bool hasPlayer = false;
    std::lock_guard<std::mutex> lk(mtx_);
    // Preserve previous state in gsi_
    for (auto it = body.begin(); it != body.end(); ++it) {
        const std::string key = it.key();
        if (key == "added" || key == "auth" || key == "previously") continue;
        if (key == "player") hasPlayer = true;
        gsi_[key] = it.value();
    }
    if (!hasPlayer) gsi_["player"] = nullptr;
}

static std::string TeamFallbackName(int side) {
    return side == 3 ? std::string("Counter-Terrorists") : std::string("Terrorists");
}

void GsiHttpServer::RebuildHudStateFromGsi() {
    Hud::State st{};
    // Snapshot gsi_ and filter under lock
    json gsi;
    std::string filterList;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gsi = gsi_;
        filterList = filtered_players_;
    }
    if (!gsi.is_object() || !gsi.contains("map")) { std::lock_guard<std::mutex> lk(mtx_); hud_ = st; return; }

    const json& map = gsi["map"];

    // Focused steamid
    std::string focused;
    if (gsi.contains("player") && gsi["player"].is_object() && gsi["player"].contains("steamid") && gsi["player"]["steamid"].is_string())
        focused = gsi["player"]["steamid"].get<std::string>();

    auto parseTeamObj = [&](int side, const json& teamObj, Hud::Team& out){
        out.side = side;
        if (teamObj.contains("name") && teamObj["name"].is_string()) out.name = teamObj["name"].get<std::string>();
        if (out.name.empty()) out.name = TeamFallbackName(side);
        if (teamObj.contains("score")) out.score = teamObj["score"].get<int>();
        if (teamObj.contains("timeouts_remaining")) out.timeoutsLeft = teamObj["timeouts_remaining"].get<int>();
        out.color = (side==3) ? IM_COL32(125,168,198,255) : IM_COL32(219,170,98,255);
    };

    if (map.contains("team_t")) parseTeamObj(2, map["team_t"], st.leftTeam);
    if (map.contains("team_ct")) parseTeamObj(3, map["team_ct"], st.rightTeam);

    // Players
    std::vector<Hud::Player> tPlayers, ctPlayers;
    if (gsi.contains("allplayers") && gsi["allplayers"].is_object()) {
        for (auto it = gsi["allplayers"].begin(); it != gsi["allplayers"].end(); ++it) {
            const std::string steam = it.key();
            const json& pl = it.value();
            Hud::Player p{};
            p.id = (int)std::hash<std::string>{}(steam);
            if (pl.contains("name")) p.name = pl["name"].get<std::string>();

            // Check if player should be filtered
            if (IsPlayerFiltered(p.name, filterList)) continue; // Skip filtered players

            std::string teamStr = pl.contains("team") ? pl["team"].get<std::string>() : "";
            p.teamSide = (teamStr == "CT") ? 3 : 2;
            if (pl.contains("observer_slot")) p.observerSlot = pl["observer_slot"].get<int>();
            if (pl.contains("match_stats")) {
                const json& ms = pl["match_stats"];
                if (ms.contains("kills")) p.kills = ms["kills"].get<int>();
                if (ms.contains("deaths")) p.deaths = ms["deaths"].get<int>();
            }
            if (pl.contains("state")) {
                const json& stt = pl["state"];
                if (stt.contains("health")) { p.health = stt["health"].get<int>(); p.isAlive = p.health > 0; }
                if (stt.contains("armor")) p.armor = stt["armor"].get<int>();
                if (stt.contains("money")) p.money = stt["money"].get<int>();
                if (stt.contains("helmet")) p.hasHelmet = stt["helmet"].get<bool>();
                if (stt.contains("defusekit")) p.hasDefuser = stt["defusekit"].get<bool>();
            }
            if (pl.contains("weapons") && pl["weapons"].is_object()) {
                for (auto wit = pl["weapons"].begin(); wit != pl["weapons"].end(); ++wit) {
                    const json& w = wit.value();
                    std::string wtype = w.contains("type") && w["type"].is_string() ? w["type"].get<std::string>() : std::string();
                    std::string wname = w.contains("name") && w["name"].is_string() ? w["name"].get<std::string>() : std::string();
                    std::string wstate = w.contains("state") && w["state"].is_string() ? w["state"].get<std::string>() : std::string();
                    int ammoClip = w.contains("ammo_clip") ? (w["ammo_clip"].is_number() ? (int)w["ammo_clip"].get<double>() : -1) : -1;
                    int ammoReserve = w.contains("ammo_reserve") ? (w["ammo_reserve"].is_number() ? (int)w["ammo_reserve"].get<double>() : -1) : -1;

                    auto unprefix = [&](const std::string& nm){ return (nm.rfind("weapon_", 0) == 0) ? nm.substr(7) : nm; };

                    Hud::Weapon wpn{};
                    wpn.name = unprefix(wname);
                    wpn.isActive = (wstate == "active");
                    wpn.ammoClip = ammoClip;
                    wpn.ammoReserve = ammoReserve;
                    if (wtype == "Grenade") {
                        wpn.isGrenade = true;
                        // map to icons keys similar to cs-hud
                        if (wname == "weapon_flashbang") p.grenades.push_back("flashbang");
                        else if (wname == "weapon_smokegrenade") p.grenades.push_back("smokegrenade");
                        else if (wname == "weapon_incgrenade" || wname == "weapon_molotov") p.grenades.push_back("molotov");
                        else if (wname == "weapon_hegrenade") p.grenades.push_back("hegrenade");
                        else if (wname == "weapon_decoy") p.grenades.push_back("decoy");
                    } else if (wtype == "C4") {
                        wpn.isBomb = true; p.hasBomb = true;
                    } else if (wtype == "Pistol") {
                        wpn.isSecondary = true; p.secondary = wpn;
                    } else if (wtype == "Knife") {
                        // skip for now
                    } else {
                        // classify as primary
                        if (!wtype.empty()) { wpn.isPrimary = true; p.primary = wpn; }
                    }

                    if (wpn.isActive) {
                        // if active is grenade/C4, prefer fallback to primary/secondary for ammo display
                        if (!wpn.isGrenade && !wpn.isBomb) p.active = wpn;
                    }
                }
                if (p.active.name.empty()) {
                    if (!p.primary.name.empty()) p.active = p.primary;
                    else if (!p.secondary.name.empty()) p.active = p.secondary;
                }
            }
            if (!focused.empty() && steam == focused) p.isFocused = true;
            if (p.teamSide == 3) ctPlayers.push_back(std::move(p)); else tPlayers.push_back(std::move(p));
        }
    }

    auto bySlot = [](const Hud::Player& a, const Hud::Player& b){ return a.observerSlot < b.observerSlot; };
    std::sort(tPlayers.begin(), tPlayers.end(), bySlot);
    std::sort(ctPlayers.begin(), ctPlayers.end(), bySlot);

    // Decide left/right by first slot
    int tFirst = tPlayers.empty() ? 99 : tPlayers.front().observerSlot;
    int cFirst = ctPlayers.empty() ? 99 : ctPlayers.front().observerSlot;
    bool tOnLeft = tFirst <= cFirst;
    if (tOnLeft) {
        st.leftTeam.side = 2; st.rightTeam.side = 3;
        st.leftTeam.color = IM_COL32(219,170,98,255);
        st.rightTeam.color = IM_COL32(125,168,198,255);
        st.leftPlayers = std::move(tPlayers);
        st.rightPlayers = std::move(ctPlayers);
    } else {
        std::swap(st.leftTeam, st.rightTeam);
        st.leftPlayers = std::move(ctPlayers);
        st.rightPlayers = std::move(tPlayers);
    }

    for (auto& p : st.leftPlayers) if (p.isFocused) { st.focusedPlayerId = p.id; break; }
    if (st.focusedPlayerId < 0) for (auto& p : st.rightPlayers) if (p.isFocused) { st.focusedPlayerId = p.id; break; }

    if (map.contains("round")) { st.round.number = map["round"].get<int>() + 1; }
    if (gsi.contains("phase_countdowns") && gsi["phase_countdowns"].is_object()) {
        const json& pc = gsi["phase_countdowns"];
        if (pc.contains("phase") && pc["phase"].is_string()) st.round.phase = pc["phase"].get<std::string>();
        if (pc.contains("phase_ends_in")) {
            if (pc["phase_ends_in"].is_string()) st.round.timeLeft = (float)std::atof(pc["phase_ends_in"].get<std::string>().c_str());
            else if (pc["phase_ends_in"].is_number()) st.round.timeLeft = (float)pc["phase_ends_in"].get<double>();
        }
    }

    // Bomb
    if (gsi.contains("bomb") && gsi["bomb"].is_object()) {
        const json& b = gsi["bomb"];
        if (b.contains("state") && b["state"].is_string()) st.bomb.state = b["state"].get<std::string>();
        st.bomb.isPlanted = (st.bomb.state == "planted" || st.bomb.state == "defusing");
        st.bomb.isDefusing = (st.bomb.state == "defusing");
        if (b.contains("countdown")) {
            if (b["countdown"].is_string()) st.bomb.countdownSec = (float)std::atof(b["countdown"].get<std::string>().c_str());
            else if (b["countdown"].is_number()) st.bomb.countdownSec = (float)b["countdown"].get<double>();
        } else if (st.bomb.state == "planted") {
            // keep at 0 if near explosion
            st.bomb.countdownSec = 0.0f;
        }
    } else {
        st.bomb = Hud::BombInfo{};
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        hud_ = std::move(st);
    }
}
