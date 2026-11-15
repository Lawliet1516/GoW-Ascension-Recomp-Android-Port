// gowa_discord: Discord Rich Presence + "Join" for the GoW Ascension community multiplayer (mpnet, 2026-11-13).
//
// Runs next to the game (the launcher starts it while online is on). Every few seconds it asks the community server
// what this player is doing (GET /api/presence?id=NAME: lobby / searching / in match, mode, players, private match
// code; never an address) and shows it on the player's Discord profile through the Discord app on this PC (no
// Discord sign-in needed: Rich Presence goes through the running Discord client).
//
// Private match code set (launcher Online page) -> the presence carries a Join secret "server|code"; a friend who
// presses Join in Discord gets that secret here, and it is written to the join file (default
// <exe folder>\discord-join.txt) for the launcher to use as server + match code.
//
//   gowa_discord.exe --server 192.168.1.10[:36101] --name Player [--app 1570765892000682144]
//                    [--join-file PATH] [--pid GAME_PID] [--once]
//
// Build (clang-cl, x64): see build.cmd next to this file.

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::string g_server = "127.0.0.1:36101", g_name, g_join_file;
static uint64_t g_app = 1570765892000682144ull;
static DWORD g_pid = 0;

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// GET http://host:port/path -> body ("" on any error). Plain HTTP on purpose: the community server has no TLS yet.
static std::string http_get(const std::string& path)
{
    std::string host = g_server, body;
    INTERNET_PORT port = 36101;
    size_t c = host.rfind(':');
    if (c != std::string::npos) { port = (INTERNET_PORT)atoi(host.c_str() + c + 1); host.resize(c); }
    HINTERNET s = WinHttpOpen(L"gowa-discord/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 3000, 3000, 3000, 3000);
    HINTERNET k = WinHttpConnect(s, widen(host).c_str(), port, 0);
    HINTERNET r = k ? WinHttpOpenRequest(k, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        char buf[4096]; DWORD got = 0;
        while (status == 200 && WinHttpReadData(r, buf, sizeof buf, &got) && got && body.size() < 65536) body.append(buf, got);
    }
    if (r) WinHttpCloseHandle(r);
    if (k) WinHttpCloseHandle(k);
    WinHttpCloseHandle(s);
    return body;
}

// The server's own small JSON (flat object, plain strings and integers): enough to read one field.
static std::string json_str(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\": ";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    if (p < j.size() && j[p] == '"') {
        size_t e = j.find('"', p + 1);
        return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
    }
    size_t e = j.find_first_of(",}", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static bool valid_name(const std::string& s)
{
    if (s.empty() || s.size() > 16) return false;
    for (char ch : s) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return false;
    return true;
}

static bool game_alive()
{
    if (!g_pid) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_pid);
    if (!h) return false;
    DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;
}

int main(int argc, char** argv)
{
    bool once = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](void) -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--server") { g_server = next(); if (g_server.find(':') == std::string::npos) g_server += ":36101"; }
        else if (a == "--name") g_name = next();
        else if (a == "--app") g_app = _strtoui64(next().c_str(), nullptr, 10);
        else if (a == "--join-file") g_join_file = next();
        else if (a == "--pid") g_pid = (DWORD)strtoul(next().c_str(), nullptr, 10);
        else if (a == "--once") once = true;
    }
    if (!valid_name(g_name)) { fprintf(stderr, "gowa_discord: --name NAME (1-16 letters/digits) is needed\n"); return 2; }
    if (g_join_file.empty()) {
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string d = exe; d.resize(d.find_last_of("\\/") + 1);
        g_join_file = d + "discord-join.txt";
    }

    auto client = std::make_shared<discordpp::Client>();
    client->SetApplicationId(g_app);
    client->SetActivityJoinCallback([](std::string secret) {
        // secret = "server|code" from the friend's presence: hand it to the launcher
        FILE* f = fopen(g_join_file.c_str(), "wb");
        if (f) { fputs(secret.c_str(), f); fclose(f); }
        printf("[discord] join: %s -> %s\n", secret.c_str(), g_join_file.c_str());
        fflush(stdout);
    });

    std::string last;
    std::atomic<int> pending{0};
    auto next_poll = std::chrono::steady_clock::now();
    printf("[discord] app %llu, server %s, player %s\n", (unsigned long long)g_app, g_server.c_str(), g_name.c_str());
    fflush(stdout);
    for (;;) {
        discordpp::RunCallbacks();
        if (!game_alive()) break;
        auto now = std::chrono::steady_clock::now();
        if (now >= next_poll && pending == 0) {
            next_poll = now + std::chrono::seconds(5);
            std::string j = http_get("/api/presence?id=" + g_name);
            std::string state = json_str(j, "state"), mode = json_str(j, "mode"), party = json_str(j, "party");
            int players = atoi(json_str(j, "players").c_str()), mx = atoi(json_str(j, "max").c_str());
            std::string key = state + "|" + mode + "|" + party + "|" + std::to_string(players) + "/" + std::to_string(mx);
            if (!j.empty() && key != last) {
                last = key;
                discordpp::Activity act;
                act.SetType(discordpp::ActivityTypes::Playing);
                act.SetDetails(std::string("Multiplayer") + (party.empty() ? "" : " - private match"));
                std::string st = state == "in match" ? "In a match" : state == "searching" ? "Searching for players"
                               : state == "lobby" ? "In the lobby" : "Online";
                if (!mode.empty()) st += ": " + mode;
                act.SetState(st);
                discordpp::ActivityAssets as;
                as.SetLargeImage(std::string("logo"));
                as.SetLargeText(std::string("Community server (unofficial PC port)"));
                act.SetAssets(as);
                if (!party.empty()) {
                    discordpp::ActivityParty pt;
                    pt.SetId("gowa-" + party);
                    pt.SetCurrentSize(players > 0 ? players : 1);
                    pt.SetMaxSize(mx > 0 ? mx : 8);
                    pt.SetPrivacy(discordpp::ActivityPartyPrivacy::Private);
                    act.SetParty(pt);
                    discordpp::ActivitySecrets sec;
                    sec.SetJoin(g_server + "|" + party);
                    act.SetSecrets(sec);
                }
                pending = 1;
                client->UpdateRichPresence(std::move(act), [&pending, key](discordpp::ClientResult r) {
                    printf("[discord] presence %s: %s\n", key.c_str(), r.Successful() ? "ok" : r.Error().c_str());
                    fflush(stdout);
                    pending = 0;
                });
            } else if (j.empty() && last != "no server") {
                last = "no server";
                printf("[discord] the server at %s does not answer\n", g_server.c_str());
                fflush(stdout);
            }
        }
        if (once && !last.empty() && pending == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    client->ClearRichPresence();
    for (int i = 0; i < 10; i++) { discordpp::RunCallbacks(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return 0;
}
