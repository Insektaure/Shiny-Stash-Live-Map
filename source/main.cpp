#include <switch.h>
#include <switch/dmntcht.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

// ============================================================
// Constants
// ============================================================

static constexpr int SCREEN_W = 1280;
static constexpr int SCREEN_H = 720;

static constexpr u64 TITLE_ID          = 0x0100F43008C44000ULL;
static constexpr u64 TERMINATOR_HASH   = 0xCBF29CE484222645ULL;
static constexpr int SHINY_STASH_SIZE  = 4960;
static constexpr int ENTRY_SIZE        = 0x1F0;
static constexpr int PA9_DATA_OFFSET   = 0x08;  // hash(8) then PA9 starts
static constexpr int PA9_SPECIES_OFF   = 0x08;  // species u16 within PA9
static constexpr u64 PTR_CHAIN[]       = {0x120, 0x168, 0x0};

// Version detection via build ID (first 8 bytes of main_nso_build_id)
struct GameVersion {
    u8 build_id[8];
    const char* version;
    u64 basePointer;
};

static const GameVersion g_versions[] = {
    {{0xBC,0xE5,0xD5,0x39,0x3B,0x5A,0xA3,0xA8}, "2.0.1", 0x610A710},
    {{0x8A,0x1C,0x86,0xC4,0x37,0x39,0x4B,0x69}, "2.0.0", 0x6105710},
    {{0x17,0x9C,0x38,0x43,0xB9,0x84,0xF8,0x78}, "1.0.3", 0x5F0E250},
};

// Layout
static constexpr int MAP_AREA_X = 20;
static constexpr int MAP_AREA_Y = 20;
static constexpr int MAP_AREA_W = 680;
static constexpr int MAP_AREA_H = 630;
static constexpr int INFO_Y     = MAP_AREA_Y + MAP_AREA_H + 8;
static constexpr int LIST_X     = MAP_AREA_X + MAP_AREA_W + 20;
static constexpr int LIST_Y     = 20;
static constexpr int LIST_W     = SCREEN_W - LIST_X - 20;
static constexpr int ITEM_H     = 62;

// Colors (SDL)
static constexpr SDL_Color COL_BG       = {0x16, 0x16, 0x2B, 0xFF};
static constexpr SDL_Color COL_PANEL    = {0x1E, 0x1E, 0x38, 0xFF};
static constexpr SDL_Color COL_SEL      = {0x1A, 0x3A, 0x6E, 0xFF};
static constexpr SDL_Color COL_BORDER   = {0x30, 0x30, 0x55, 0xFF};
static constexpr SDL_Color COL_WHITE    = {0xFF, 0xFF, 0xFF, 0xFF};
static constexpr SDL_Color COL_GRAY     = {0x88, 0x88, 0x88, 0xFF};
static constexpr SDL_Color COL_DIMGRAY  = {0x55, 0x55, 0x55, 0xFF};
static constexpr SDL_Color COL_GOLD     = {0xFF, 0xD7, 0x00, 0xFF};
static constexpr SDL_Color COL_CYAN     = {0x40, 0xC8, 0xFF, 0xFF};
static constexpr SDL_Color COL_RED      = {0xFF, 0x33, 0x33, 0xFF};

// ============================================================
// Map Transform (from ShinyStashMap/MapTransform.cs)
// ============================================================

struct MapTransform {
    double texW, texH;
    double rangeX, rangeZ;
    double scaleX, scaleZ;
    double dirX, dirZ;
    double offsetX, offsetZ;

    double convertX(double x) const {
        return (texW / 2.0) + (dirX * ((rangeX / scaleX) * (x + offsetX)));
    }
    double convertZ(double z) const {
        return (texH / 2.0) + (dirZ * ((rangeZ / scaleZ) * (z + offsetZ)));
    }
};

static const MapTransform g_transforms[] = {
    {4096,4096, 3940,3940, 1000,1000, -1,-1, 500,500},
    {2160,2160, 1662,2041, 1662.0/10.291021,2041.0/10.291021, -1,-1, -3,-80},
    {2160,2160, 1364,1975, 1364.0/6.2,1975.0/6.2, 1,1, 1,146},
    {2160,2160, 1521,1966, 1521.0/16.714285,1966.0/16.714285, 1,1, 39,45},
};

static const char* g_mapNames[]  = {"Lumiose City","Lysandre Labs","The Sewers","The Sewers B"};
static const char* g_mapFiles[]  = {"romfs:/lumiose.png","romfs:/LysandreLabs.png",
                                    "romfs:/Sewers.png","romfs:/SewersB.png"};

// ============================================================
// Data Types
// ============================================================

struct SpawnerEntry {
    u64 hash;
    float x, y, z;
    int mapIdx;
    std::string location;
};

struct ShinyEntry {
    u64 hash;
    u16 speciesInternal;
    u16 nationalDex;
};

// ============================================================
// Global State
// ============================================================

static SDL_Window*   g_window   = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static TTF_Font*     g_fontLg   = nullptr;   // 26
static TTF_Font*     g_fontMd   = nullptr;   // 20
static TTF_Font*     g_fontSm   = nullptr;   // 15
static SDL_Texture*  g_mapTex[4]   = {};
static int           g_mapW[4]     = {};
static int           g_mapH[4]     = {};

static std::vector<std::string>  g_speciesNames;
static std::vector<SpawnerEntry> g_spawners;
static std::vector<ShinyEntry>   g_entries;

static int  g_selIdx     = 0;
static int  g_scrollOff  = 0;
static const SpawnerEntry* g_selSpawner = nullptr;
static std::string g_statusMsg = "Press A to read game memory";
static std::string g_gameVersion;
static std::string g_detectedBid;
static bool g_showAbout  = false;

static std::unordered_map<u16, SDL_Texture*> g_spriteCache;
static constexpr int SPRITE_SIZE = 40;  // display size in the list

static SDL_Texture* getSpriteTex(u16 nationalDex) {
    auto it = g_spriteCache.find(nationalDex);
    if (it != g_spriteCache.end()) return it->second;
    char path[64];
    snprintf(path, sizeof(path), "romfs:/sprites/%03u.png", nationalDex);
    SDL_Surface* surf = IMG_Load(path);
    SDL_Texture* tex = nullptr;
    if (surf) {
        tex = SDL_CreateTextureFromSurface(g_renderer, surf);
        SDL_FreeSurface(surf);
    }
    g_spriteCache[nationalDex] = tex;  // cache nullptr too to avoid retrying
    return tex;
}

// ============================================================
// PKX Decryption (LCRNG XOR + Block Shuffle)
// ============================================================

static constexpr u32 LCRNG_MULT = 0x41C64E6D;
static constexpr u32 LCRNG_ADD  = 0x00006073;
static constexpr int PKX_HEADER = 8;   // EC(4) + Sanity(2) + Checksum(2)
static constexpr int PKX_BLOCK  = 80;  // 0x50 bytes per block

static const u8 g_blockPos[] = {
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1,
    1,0,2,3, 1,0,3,2, 2,0,1,3, 3,0,1,2, 2,0,3,1, 3,0,2,1,
    1,2,0,3, 1,3,0,2, 2,1,0,3, 3,1,0,2, 2,3,0,1, 3,2,0,1,
    1,2,3,0, 1,3,2,0, 2,1,3,0, 3,1,2,0, 2,3,1,0, 3,2,1,0,
    // Duplicates of 0-7 for sv values 24-31
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1,
    1,0,2,3, 1,0,3,2,
};

static void decryptPA9(u8* data, int len) {
    u32 ec;
    memcpy(&ec, data, sizeof(u32));

    // XOR decrypt from byte 8 onwards
    u32 seed = ec;
    int count = (len - PKX_HEADER) / 2;
    u16* ptr = (u16*)(data + PKX_HEADER);
    for (int i = 0; i < count; i++) {
        seed = seed * LCRNG_MULT + LCRNG_ADD;
        ptr[i] ^= (u16)(seed >> 16);
    }

    // Unshuffle 4 blocks
    u32 sv = (ec >> 13) & 31;
    u8 temp[4 * PKX_BLOCK];
    const u8* order = &g_blockPos[sv * 4];
    for (int b = 0; b < 4; b++)
        memcpy(&temp[b * PKX_BLOCK], &data[PKX_HEADER + order[b] * PKX_BLOCK], PKX_BLOCK);
    memcpy(&data[PKX_HEADER], temp, 4 * PKX_BLOCK);
}

// ============================================================
// Gen9 Species Converter
// ============================================================

static const s8 g_t9[] = {
    65,-1,-1,-1,-1,31,31,47,47,29,29,53,31,31,46,44,30,30,-7,-7,-7,13,13,
    -2,-2,23,23,24,-21,-21,27,27,47,47,47,26,14,-33,-33,-33,-17,-17,3,-29,
    12,-12,-31,-31,-31,3,3,-24,-24,-44,-44,-30,-30,-28,-28,23,23,6,7,29,8,
    3,4,4,20,4,23,6,3,3,4,-1,13,9,7,5,7,9,9,-43,-43,-43,-68,-68,-68,-58,
    -58,-25,-29,-31,6,-1,6,0,0,0,3,3,4,2,3,3,-5,-12,-12,
};

static u16 getNational9(u16 raw) {
    int s = (int)raw - 917;
    if (s < 0 || (size_t)s >= sizeof(g_t9)) return raw;
    return (u16)((int)raw + g_t9[s]);
}

static const char* getSpeciesName(u16 ndex) {
    if (ndex < g_speciesNames.size()) return g_speciesNames[ndex].c_str();
    static char buf[32];
    snprintf(buf, sizeof(buf), "Species #%u", ndex);
    return buf;
}

// ============================================================
// Drawing Helpers
// ============================================================

static void drawText(TTF_Font* font, const char* text, int x, int y, SDL_Color col) {
    if (!text || !text[0]) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(g_renderer, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void drawTextRight(TTF_Font* font, const char* text, int rightX, int y, SDL_Color col) {
    if (!text || !text[0]) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(g_renderer, surf);
    SDL_Rect dst = {rightX - surf->w, y, surf->w, surf->h};
    SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void fillCircle(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        SDL_RenderDrawLine(g_renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void drawCircleOutline(int cx, int cy, int r) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        SDL_RenderDrawPoint(g_renderer, cx+x, cy+y); SDL_RenderDrawPoint(g_renderer, cx-x, cy+y);
        SDL_RenderDrawPoint(g_renderer, cx+x, cy-y); SDL_RenderDrawPoint(g_renderer, cx-x, cy-y);
        SDL_RenderDrawPoint(g_renderer, cx+y, cy+x); SDL_RenderDrawPoint(g_renderer, cx-y, cy+x);
        SDL_RenderDrawPoint(g_renderer, cx+y, cy-x); SDL_RenderDrawPoint(g_renderer, cx-y, cy-x);
        y++;
        if (err < 0) err += 2*y+1;
        else { x--; err += 2*(y-x)+1; }
    }
}

static void drawRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_renderer, &r);
}

static void drawBorder(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderDrawRect(g_renderer, &r);
}

// ============================================================
// File I/O
// ============================================================

static std::string readTextFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ""; }
    std::string s(sz, '\0');
    fread(&s[0], 1, sz, f);
    fclose(f);
    return s;
}

// ============================================================
// Data Loading
// ============================================================

static void parseSpawnerFile(const std::string& content, int mapIdx) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t le = content.find('\n', pos);
        if (le == std::string::npos) le = content.size();
        std::string line(content, pos, le - pos);
        pos = le + 1;
        if (line.size() < 20) continue;

        size_t d1 = line.find(" - ");
        if (d1 == std::string::npos) continue;
        size_t hs = d1 + 3;
        size_t d2 = line.find(" - ", hs);
        if (d2 == std::string::npos) continue;

        std::string hashStr(line, hs, d2 - hs);
        if (hashStr.size() != 16) continue;
        char* ep;
        u64 hash = strtoull(hashStr.c_str(), &ep, 16);
        if (ep != hashStr.c_str() + 16) continue;

        size_t v = line.find("V3f(");
        if (v == std::string::npos) continue;
        size_t cs = v + 4, ce = line.find(')', cs);
        if (ce == std::string::npos) continue;

        float x, y, z;
        std::string coords(line, cs, ce - cs);
        if (sscanf(coords.c_str(), "%f, %f, %f", &x, &y, &z) != 3) continue;

        std::string loc(line, 0, d1);
        size_t ns = loc.find_first_not_of(" \t\"");
        size_t ne = loc.find_last_not_of(" \t\"");
        if (ns != std::string::npos && ne != std::string::npos)
            loc = loc.substr(ns, ne - ns + 1);
        else loc = "";

        g_spawners.push_back({hash, x, y, z, mapIdx, std::move(loc)});
    }
}

static void loadData() {
    // Species names
    std::string content = readTextFile("romfs:/species_en.txt");
    if (!content.empty()) {
        size_t pos = 0;
        while (pos < content.size()) {
            size_t end = content.find('\n', pos);
            if (end == std::string::npos) end = content.size();
            std::string name(content, pos, end - pos);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            g_speciesNames.push_back(std::move(name));
            if (end == content.size()) break;
            pos = end + 1;
        }
    }

    // Spawners
    static const struct { const char* path; int idx; } files[] = {
        {"romfs:/t1_point_spawners.txt", 0}, {"romfs:/t2_point_spawners.txt", 1},
        {"romfs:/t3_point_spawners.txt", 2}, {"romfs:/t4_point_spawners.txt", 3},
    };
    for (auto& f : files) {
        content = readTextFile(f.path);
        if (!content.empty()) parseSpawnerFile(content, f.idx);
    }

    // Map textures
    for (int i = 0; i < 4; i++) {
        SDL_Surface* surf = IMG_Load(g_mapFiles[i]);
        if (!surf) {
            g_statusMsg = std::string("IMG_Load failed: ") + IMG_GetError();
            continue;
        }
        g_mapTex[i] = SDL_CreateTextureFromSurface(g_renderer, surf);
        g_mapW[i] = surf->w;
        g_mapH[i] = surf->h;
        SDL_FreeSurface(surf);
        if (!g_mapTex[i])
            g_statusMsg = std::string("Texture failed: ") + SDL_GetError();
    }
}

// ============================================================
// Memory Reading (dmnt:cht)
// ============================================================

static const SpawnerEntry* findSpawner(u64 hash) {
    for (const auto& sp : g_spawners)
        if (sp.hash == hash) return &sp;
    return nullptr;
}

static void updateSelection() {
    g_selSpawner = nullptr;
    if (g_selIdx >= 0 && g_selIdx < (int)g_entries.size())
        g_selSpawner = findSpawner(g_entries[g_selIdx].hash);
}

static void readShinyStash() {
    g_entries.clear();
    g_selIdx = 0;
    g_scrollOff = 0;
    g_selSpawner = nullptr;
    g_detectedBid.clear();

    Result rc = dmntchtInitialize();
    if (R_FAILED(rc)) { g_statusMsg = "dmntcht init failed"; return; }

    bool hasProc = false;
    rc = dmntchtHasCheatProcess(&hasProc);
    if (R_FAILED(rc) || !hasProc) {
        g_statusMsg = "No cheat process (is Atmosphere running?)";
        dmntchtExit(); return;
    }

    rc = dmntchtForceOpenCheatProcess();
    if (R_FAILED(rc)) {
        g_statusMsg = "Can't open cheat process";
        dmntchtExit(); return;
    }

    DmntCheatProcessMetadata meta;
    rc = dmntchtGetCheatProcessMetadata(&meta);
    if (R_FAILED(rc)) {
        g_statusMsg = "Metadata read failed";
        goto done;
    }
    if (meta.title_id != TITLE_ID) {
        g_statusMsg = "Pokemon Legends: Z-A is not running";
        goto done;
    }

    {
        // Detect game version from build ID
        char bid[24];
        snprintf(bid, sizeof(bid), "%02X%02X%02X%02X%02X%02X%02X%02X",
            meta.main_nso_build_id[0], meta.main_nso_build_id[1],
            meta.main_nso_build_id[2], meta.main_nso_build_id[3],
            meta.main_nso_build_id[4], meta.main_nso_build_id[5],
            meta.main_nso_build_id[6], meta.main_nso_build_id[7]);
        g_detectedBid = bid;

        const GameVersion* ver = nullptr;
        for (const auto& v : g_versions) {
            if (memcmp(meta.main_nso_build_id, v.build_id, 8) == 0) {
                ver = &v;
                break;
            }
        }
        if (!ver) {
            g_statusMsg = "Unsupported game version";
            g_gameVersion.clear();
            goto done;
        }
        g_gameVersion = ver->version;

        u64 addr = meta.main_nso_extents.base + ver->basePointer;
        u64 ptr;
        for (int i = 0; i < 3; i++) {
            rc = dmntchtReadCheatProcessMemory(addr, &ptr, sizeof(u64));
            if (R_FAILED(rc)) { g_statusMsg = "Pointer resolve failed"; goto done; }
            addr = ptr + PTR_CHAIN[i];
        }

        u8* buf = (u8*)malloc(SHINY_STASH_SIZE);
        if (!buf) { g_statusMsg = "malloc failed"; goto done; }

        rc = dmntchtReadCheatProcessMemory(addr, buf, SHINY_STASH_SIZE);
        if (R_FAILED(rc)) { g_statusMsg = "Stash read failed"; free(buf); goto done; }

        for (int i = 0; i + ENTRY_SIZE <= SHINY_STASH_SIZE; i += ENTRY_SIZE) {
            u64 hash;
            memcpy(&hash, &buf[i], sizeof(u64));
            if (hash == 0 || hash == TERMINATOR_HASH) break;

            // Decrypt PA9 data to read species
            u8 pa9[0x158];
            memcpy(pa9, &buf[i + PA9_DATA_OFFSET], 0x158);
            decryptPA9(pa9, 0x158);

            u16 specInt;
            memcpy(&specInt, &pa9[PA9_SPECIES_OFF], sizeof(u16));
            if (specInt == 0) continue; // skip empty entries
            u16 ndex = getNational9(specInt);

            if (!findSpawner(hash)) continue; // skip entries with no known spawn location

            bool dup = false;
            for (auto& e : g_entries)
                if (e.hash == hash) { dup = true; break; }
            if (!dup)
                g_entries.push_back({hash, specInt, ndex});
        }
        free(buf);

        if (g_entries.empty())
            g_statusMsg = "Shiny stash is empty";
        else {
            g_statusMsg = std::to_string(g_entries.size()) + " shiny entries loaded (v" + g_gameVersion + ")";
            updateSelection();
        }
    }

done:
    dmntchtExit();
}

// ============================================================
// Rendering
// ============================================================

static void renderMap() {
    // Panel background
    drawRect(MAP_AREA_X, MAP_AREA_Y, MAP_AREA_W, MAP_AREA_H, COL_PANEL);
    drawBorder(MAP_AREA_X, MAP_AREA_Y, MAP_AREA_W, MAP_AREA_H, COL_BORDER);

    int mapIdx = -1;
    if (g_selSpawner) mapIdx = g_selSpawner->mapIdx;

    if (mapIdx >= 0 && g_mapTex[mapIdx]) {
        // Scale map to fit area while keeping aspect ratio
        int tw = g_mapW[mapIdx], th = g_mapH[mapIdx];
        float sx = (float)(MAP_AREA_W - 4) / tw;
        float sy = (float)(MAP_AREA_H - 4) / th;
        float sc = std::min(sx, sy);
        int dw = (int)(tw * sc), dh = (int)(th * sc);
        int dx = MAP_AREA_X + (MAP_AREA_W - dw) / 2;
        int dy = MAP_AREA_Y + (MAP_AREA_H - dh) / 2;

        SDL_Rect dst = {dx, dy, dw, dh};
        SDL_RenderCopy(g_renderer, g_mapTex[mapIdx], nullptr, &dst);

        const MapTransform& tr = g_transforms[mapIdx];

        // Draw all spawner positions in this map as tiny dim dots
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer, 0xFF, 0xFF, 0xFF, 0x20);
        for (const auto& sp : g_spawners) {
            if (sp.mapIdx != mapIdx) continue;
            double texX = tr.convertX(sp.x);
            double texZ = tr.convertZ(sp.z);
            int px = dx + (int)((texX / tr.texW) * dw);
            int py = dy + (int)((texZ / tr.texH) * dh);
            if (px >= dx && px < dx+dw && py >= dy && py < dy+dh)
                SDL_RenderDrawPoint(g_renderer, px, py);
        }

        // Draw all stash entries on this map as gold dots
        for (int ei = 0; ei < (int)g_entries.size(); ei++) {
            if (ei == g_selIdx) continue; // draw selected last
            const SpawnerEntry* sp = findSpawner(g_entries[ei].hash);
            if (!sp || sp->mapIdx != mapIdx) continue;
            double texX = tr.convertX(sp->x);
            double texZ = tr.convertZ(sp->z);
            int px = dx + (int)((texX / tr.texW) * dw);
            int py = dy + (int)((texZ / tr.texH) * dh);
            if (px < dx || px >= dx+dw || py < dy || py >= dy+dh) continue;
            SDL_SetRenderDrawColor(g_renderer, COL_GOLD.r, COL_GOLD.g, COL_GOLD.b, 0xCC);
            fillCircle(px, py, 5);
            SDL_SetRenderDrawColor(g_renderer, 0x00, 0x00, 0x00, 0xAA);
            drawCircleOutline(px, py, 5);
        }

        // Draw selected spawn point with crosshair
        {
            double texX = tr.convertX(g_selSpawner->x);
            double texZ = tr.convertZ(g_selSpawner->z);
            int px = dx + (int)((texX / tr.texW) * dw);
            int py = dy + (int)((texZ / tr.texH) * dh);
            px = std::clamp(px, dx + 4, dx + dw - 4);
            py = std::clamp(py, dy + 4, dy + dh - 4);

            // Outer ring
            SDL_SetRenderDrawColor(g_renderer, 0xFF, 0xFF, 0xFF, 0xFF);
            drawCircleOutline(px, py, 12);
            drawCircleOutline(px, py, 11);
            // Filled dot
            SDL_SetRenderDrawColor(g_renderer, COL_RED.r, COL_RED.g, COL_RED.b, 0xFF);
            fillCircle(px, py, 8);
            // Crosshair
            SDL_SetRenderDrawColor(g_renderer, 0xFF, 0xFF, 0xFF, 0xCC);
            SDL_RenderDrawLine(g_renderer, px - 18, py, px - 13, py);
            SDL_RenderDrawLine(g_renderer, px + 13, py, px + 18, py);
            SDL_RenderDrawLine(g_renderer, px, py - 18, px, py - 13);
            SDL_RenderDrawLine(g_renderer, px, py + 13, px, py + 18);
        }
        // Map name label
        drawText(g_fontSm, g_mapNames[mapIdx], dx + 6, dy + 4, {0xFF, 0xFF, 0xFF, 0x88});
    } else if (!g_entries.empty()) {
        drawText(g_fontMd, "Unknown spawn location", MAP_AREA_X + 200, MAP_AREA_Y + 300, COL_DIMGRAY);
    } else {
        drawText(g_fontMd, "No location selected", MAP_AREA_X + 220, MAP_AREA_Y + 300, COL_DIMGRAY);
    }
}

static void renderInfo() {
    int y = INFO_Y;
    if (g_selSpawner) {
        drawText(g_fontSm, g_mapNames[g_selSpawner->mapIdx], MAP_AREA_X + 4, y, COL_CYAN);
        drawText(g_fontSm, g_selSpawner->location.c_str(), MAP_AREA_X + 160, y, COL_GRAY);

        char buf[96];
        snprintf(buf, sizeof(buf), "X: %.1f  Y: %.1f  Z: %.1f", g_selSpawner->x, g_selSpawner->y, g_selSpawner->z);
        drawTextRight(g_fontSm, buf, MAP_AREA_X + MAP_AREA_W, y, COL_DIMGRAY);
    } else if (!g_entries.empty() && g_selIdx < (int)g_entries.size()) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Hash: %016llX", (unsigned long long)g_entries[g_selIdx].hash);
        drawText(g_fontSm, buf, MAP_AREA_X + 4, y, COL_DIMGRAY);
    } else if (!g_detectedBid.empty()) {
        std::string bidLine = "BID: " + g_detectedBid;
        drawText(g_fontSm, bidLine.c_str(), MAP_AREA_X + 4, y, COL_CYAN);
        drawText(g_fontSm, g_statusMsg.c_str(), MAP_AREA_X + 4, y + 18, COL_RED);
        y += 18;
    } else {
        drawText(g_fontSm, g_statusMsg.c_str(), MAP_AREA_X + 4, y, COL_DIMGRAY);
    }

    // Controls
    drawText(g_fontSm, "A: Read stash    -: About    +: Exit", MAP_AREA_X + 4, y + 24, {0x44,0x44,0x44,0xFF});
}

static void renderList() {
    // Panel
    drawRect(LIST_X - 10, LIST_Y - 10, LIST_W + 20, SCREEN_H - 20, COL_PANEL);
    drawBorder(LIST_X - 10, LIST_Y - 10, LIST_W + 20, SCREEN_H - 20, COL_BORDER);

    // Title
    char title[64];
    if (g_entries.empty())
        snprintf(title, sizeof(title), "Shiny Stash");
    else
        snprintf(title, sizeof(title), "Shiny Stash (%d)", (int)g_entries.size());
    drawText(g_fontLg, title, LIST_X + 8, LIST_Y, COL_GOLD);
    int headerH = 40;

    // Separator
    SDL_SetRenderDrawColor(g_renderer, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 0xFF);
    SDL_RenderDrawLine(g_renderer, LIST_X, LIST_Y + headerH, LIST_X + LIST_W, LIST_Y + headerH);

    int listTop = LIST_Y + headerH + 6;
    int listH = SCREEN_H - 30 - listTop;

    if (g_entries.empty()) {
        drawText(g_fontMd, g_statusMsg.c_str(), LIST_X + 12, listTop + 20, COL_GRAY);
        return;
    }

    // Visible entries
    int maxVis = listH / ITEM_H;
    if (maxVis < 1) maxVis = 1;

    if (g_selIdx < g_scrollOff)
        g_scrollOff = g_selIdx;
    else if (g_selIdx >= g_scrollOff + maxVis)
        g_scrollOff = g_selIdx - maxVis + 1;

    for (int vi = 0; vi < maxVis && (vi + g_scrollOff) < (int)g_entries.size(); vi++) {
        int idx = vi + g_scrollOff;
        int iy = listTop + vi * ITEM_H;
        bool sel = (idx == g_selIdx);

        // Selection bg
        if (sel)
            drawRect(LIST_X, iy, LIST_W, ITEM_H - 4, COL_SEL);

        // Pokemon image
        int textOffX = 14;
        SDL_Texture* spriteTex = getSpriteTex(g_entries[idx].nationalDex);
        if (spriteTex) {
            SDL_Rect dst = {LIST_X + 10, iy + (ITEM_H - 4 - SPRITE_SIZE) / 2, SPRITE_SIZE, SPRITE_SIZE};
            SDL_RenderCopy(g_renderer, spriteTex, nullptr, &dst);
            textOffX = 10 + SPRITE_SIZE + 6;
        }

        // Species name
        const char* name = getSpeciesName(g_entries[idx].nationalDex);
        drawText(g_fontMd, name, LIST_X + textOffX, iy + 4,
                 sel ? COL_WHITE : SDL_Color{0xCC, 0xCC, 0xCC, 0xFF});

        // Dex number right-aligned
        char num[16];
        snprintf(num, sizeof(num), "#%03u", g_entries[idx].nationalDex);
        drawTextRight(g_fontSm, num, LIST_X + LIST_W - 10, iy + 6, COL_DIMGRAY);

        // Location name on second line
        const SpawnerEntry* sp = findSpawner(g_entries[idx].hash);
        if (sp) {
            drawText(g_fontSm, sp->location.c_str(), LIST_X + textOffX, iy + 30, COL_DIMGRAY);
            drawTextRight(g_fontSm, g_mapNames[sp->mapIdx], LIST_X + LIST_W - 10, iy + 30, {0x44,0x66,0x88,0xFF});
        } else {
            drawText(g_fontSm, "Unknown location", LIST_X + textOffX, iy + 30, {0x66,0x44,0x44,0xFF});
        }

        // Bottom separator
        if (vi < maxVis - 1 && (vi + g_scrollOff + 1) < (int)g_entries.size()) {
            SDL_SetRenderDrawColor(g_renderer, 0x28, 0x28, 0x42, 0xFF);
            SDL_RenderDrawLine(g_renderer, LIST_X + 10, iy + ITEM_H - 4,
                               LIST_X + LIST_W - 10, iy + ITEM_H - 4);
        }
    }

    // Scroll indicator
    if ((int)g_entries.size() > maxVis) {
        int thumbH = std::max(20, listH * maxVis / (int)g_entries.size());
        int maxScr = std::max(1, (int)g_entries.size() - maxVis);
        int thumbY = listTop + (listH - thumbH) * g_scrollOff / maxScr;
        drawRect(LIST_X + LIST_W - 4, thumbY, 4, thumbH, COL_BORDER);
    }
}

// ============================================================
// Initialization / Cleanup
// ============================================================

static bool initSDL() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) return false;
    if (IMG_Init(IMG_INIT_PNG) == 0) return false;
    if (TTF_Init() < 0) return false;

    g_window = SDL_CreateWindow("ZA Shiny Map",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!g_window) return false;

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) return false;

    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    return true;
}

static bool initFonts() {
    PlFontData fontData;
    Result rc = plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    if (R_FAILED(rc)) return false;

    g_fontLg = TTF_OpenFontRW(SDL_RWFromMem(fontData.address, fontData.size), 1, 26);
    g_fontMd = TTF_OpenFontRW(SDL_RWFromMem(fontData.address, fontData.size), 1, 20);
    g_fontSm = TTF_OpenFontRW(SDL_RWFromMem(fontData.address, fontData.size), 1, 15);
    return g_fontLg && g_fontMd && g_fontSm;
}

static void cleanup() {
    for (auto& p : g_spriteCache)
        if (p.second) SDL_DestroyTexture(p.second);
    g_spriteCache.clear();
    for (int i = 0; i < 4; i++)
        if (g_mapTex[i]) SDL_DestroyTexture(g_mapTex[i]);
    if (g_fontLg) TTF_CloseFont(g_fontLg);
    if (g_fontMd) TTF_CloseFont(g_fontMd);
    if (g_fontSm) TTF_CloseFont(g_fontSm);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

// ============================================================
// About Screen
// ============================================================

static void renderAbout() {
    int bw = 700, bh = 400;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;

    // Dim background
    drawRect(0, 0, SCREEN_W, SCREEN_H, {0x00, 0x00, 0x00, 0xBB});
    // Panel
    drawRect(bx, by, bw, bh, COL_PANEL);
    drawBorder(bx, by, bw, bh, COL_BORDER);

    int x = bx + 30, y = by + 24;
    drawText(g_fontLg, "Lumiose - Shiny Stash Live Map", x, y, COL_GOLD);
    y += 40;
    drawText(g_fontSm, "v" APP_VERSION " - Developed by Insektaure (github.com/Insektaure)", x, y, COL_DIMGRAY);
    y += 20;
    if (g_gameVersion.empty())
        drawText(g_fontSm, "Supported: 1.0.3, 2.0.0, 2.0.1", x, y, COL_GRAY);
    else {
        std::string verStr = "Game version: " + g_gameVersion;
        drawText(g_fontSm, verStr.c_str(), x, y, COL_GRAY);
    }
    y += 30;

    SDL_SetRenderDrawColor(g_renderer, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 0xFF);
    SDL_RenderDrawLine(g_renderer, bx + 20, y, bx + bw - 20, y);
    y += 16;

    drawText(g_fontMd, "Reads the Shiny Stash from Pokemon Legends: Z-A", x, y, COL_WHITE);
    y += 28;
    drawText(g_fontMd, "and displays spawn locations on the map.", x, y, COL_WHITE);
    y += 42;

    drawText(g_fontSm, "Based on ShinyStashMap plugin by santacrab2 & PKHeX by kwsch.", x, y, COL_GRAY);
    y += 22;
    drawText(g_fontSm, "Requires Atmosphere CFW with dmnt:cht enabled.", x, y, COL_GRAY);
    y += 38;

    drawText(g_fontSm, "Controls:", x, y, COL_CYAN);
    y += 24;
    drawText(g_fontSm, "A: Read shiny stash from game memory", x + 16, y, COL_GRAY);
    y += 20;
    drawText(g_fontSm, "D-Pad Up/Down: Navigate the stash list", x + 16, y, COL_GRAY);
    y += 20;
    drawText(g_fontSm, "-: Toggle this screen    +: Exit", x + 16, y, COL_GRAY);
    y += 34;

    drawTextRight(g_fontSm, "Press - or B to close", bx + bw - 30, by + bh - 30, COL_DIMGRAY);
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    romfsInit();
    plInitialize(PlServiceType_User);

    if (!initSDL()) {
        plExit();
        romfsExit();
        return 1;
    }

    if (!initFonts()) {
        cleanup();
        plExit();
        romfsExit();
        return 1;
    }

    loadData();

    // Input
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    bool running = true;
    while (running && appletMainLoop()) {
        // Input
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) {
            running = false;
        }
        if (kDown & HidNpadButton_Minus) {
            g_showAbout = !g_showAbout;
        }
        if (g_showAbout) {
            if (kDown & HidNpadButton_B)
                g_showAbout = false;

            SDL_SetRenderDrawColor(g_renderer, COL_BG.r, COL_BG.g, COL_BG.b, 0xFF);
            SDL_RenderClear(g_renderer);
            renderMap(); renderInfo(); renderList();
            renderAbout();
            SDL_RenderPresent(g_renderer);
            continue;
        }
        if (kDown & HidNpadButton_A) {
            g_statusMsg = "Reading...";
            // Render a frame to show status
            SDL_SetRenderDrawColor(g_renderer, COL_BG.r, COL_BG.g, COL_BG.b, 0xFF);
            SDL_RenderClear(g_renderer);
            renderMap(); renderInfo(); renderList();
            SDL_RenderPresent(g_renderer);
            readShinyStash();
        }
        if (kDown & HidNpadButton_Down) {
            if (!g_entries.empty() && g_selIdx < (int)g_entries.size() - 1) {
                g_selIdx++;
                updateSelection();
            }
        }
        if (kDown & HidNpadButton_Up) {
            if (!g_entries.empty() && g_selIdx > 0) {
                g_selIdx--;
                updateSelection();
            }
        }

        // Render
        SDL_SetRenderDrawColor(g_renderer, COL_BG.r, COL_BG.g, COL_BG.b, 0xFF);
        SDL_RenderClear(g_renderer);

        renderMap();
        renderInfo();
        renderList();

        SDL_RenderPresent(g_renderer);
    }

    cleanup();
    plExit();
    romfsExit();
    return 0;
}
