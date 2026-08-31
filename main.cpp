// Build (Windows, MSVC):  cl /EHsc main.cpp Ws2_32.lib
// Build (Linux/macOS):    g++ -std=c++17 main.cpp -o pinger
//
// Usage: pinger <host> [port]

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSESOCKET closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
    typedef int socket_t;
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
    #define CLOSESOCKET close
#endif

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolVal = false;
    double numVal = 0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;

    bool isObject() const { return type == Type::Object; }
    bool isString() const { return type == Type::String; }

    const JsonValue* find(const std::string& key) const {
        if (!isObject()) return nullptr;
        auto it = objVal.find(key);
        return it == objVal.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : src(s), pos(0) {}

    JsonValue parse() {
        skipWs();
        return parseValue();
    }

private:
    const std::string& src;
    size_t pos;

    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char get() { return pos < src.size() ? src[pos++] : '\0'; }
    void skipWs() { while (pos < src.size() && isspace((unsigned char)src[pos])) pos++; }

    JsonValue parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { pos += 4; return JsonValue{}; }
        return parseNumber();
    }

    JsonValue parseObject() {
        JsonValue v; v.type = JsonValue::Type::Object;
        get();
        skipWs();
        if (peek() == '}') { get(); return v; }
        while (true) {
            skipWs();
            JsonValue key = parseString();
            skipWs();
            get();
            JsonValue val = parseValue();
            v.objVal[key.strVal] = std::move(val);
            skipWs();
            if (peek() == ',') { get(); continue; }
            break;
        }
        skipWs();
        get();
        return v;
    }

    JsonValue parseArray() {
        JsonValue v; v.type = JsonValue::Type::Array;
        get();
        skipWs();
        if (peek() == ']') { get(); return v; }
        while (true) {
            v.arrVal.push_back(parseValue());
            skipWs();
            if (peek() == ',') { get(); skipWs(); continue; }
            break;
        }
        skipWs();
        get();
        return v;
    }

    JsonValue parseString() {
        JsonValue v; v.type = JsonValue::Type::String;
        get();
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': pos += 4; out += '?'; break;
                    default: out += esc;
                }
            } else {
                out += c;
            }
        }
        get();
        v.strVal = out;
        return v;
    }

    JsonValue parseBool() {
        JsonValue v; v.type = JsonValue::Type::Bool;
        if (peek() == 't') { pos += 4; v.boolVal = true; }
        else { pos += 5; v.boolVal = false; }
        return v;
    }

    JsonValue parseNumber() {
        JsonValue v; v.type = JsonValue::Type::Number;
        size_t start = pos;
        while (pos < src.size() && (isdigit((unsigned char)src[pos]) || strchr("-+.eE", src[pos])))
            pos++;
        v.numVal = std::stod(src.substr(start, pos - start));
        return v;
    }
};

static void writeVarInt(std::vector<uint8_t>& buf, int32_t value) {
    uint32_t uv = (uint32_t)value;
    do {
        uint8_t b = uv & 0x7F;
        uv >>= 7;
        if (uv != 0) b |= 0x80;
        buf.push_back(b);
    } while (uv != 0);
}

static void writeString(std::vector<uint8_t>& buf, const std::string& s) {
    writeVarInt(buf, (int32_t)s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

static void writeUShort(std::vector<uint8_t>& buf, uint16_t value) {
    buf.push_back((uint8_t)(value >> 8));
    buf.push_back((uint8_t)(value & 0xFF));
}

static bool recvAll(socket_t sock, uint8_t* out, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = recv(sock, (char*)out + got, (int)(len - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static bool readVarInt(socket_t sock, int32_t& result) {
    result = 0;
    int numRead = 0;
    uint8_t b;
    do {
        if (!recvAll(sock, &b, 1)) return false;
        result |= (int32_t)(b & 0x7F) << (7 * numRead);
        numRead++;
        if (numRead > 5) return false;
    } while (b & 0x80);
    return true;
}

static bool readString(socket_t sock, std::string& out) {
    int32_t len;
    if (!readVarInt(sock, len) || len < 0) return false;
    out.resize((size_t)len);
    return recvAll(sock, (uint8_t*)out.data(), (size_t)len);
}

static void sendPacket(socket_t sock, int32_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body;
    writeVarInt(body, packetId);
    body.insert(body.end(), payload.begin(), payload.end());

    std::vector<uint8_t> full;
    writeVarInt(full, (int32_t)body.size());
    full.insert(full.end(), body.begin(), body.end());

    send(sock, (const char*)full.data(), (int)full.size(), 0);
}

static std::string stripColorCodes(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ) {
        if (i + 1 < in.size() && (uint8_t)in[i] == 0xC2 && (uint8_t)in[i + 1] == 0xA7) {
            i += 2;
            if (i < in.size()) i += 1;
            continue;
        }
        out += in[i++];
    }
    return out;
}

static std::string extractMotd(const JsonValue& desc) {
    if (desc.isString()) return stripColorCodes(desc.strVal);

    std::string out;
    if (auto* text = desc.find("text")) out += text->strVal;
    if (auto* extra = desc.find("extra")) {
        if (extra->type == JsonValue::Type::Array)
            for (auto& part : extra->arrVal)
                if (auto* t = part.find("text")) out += t->strVal;
    }
    return stripColorCodes(out);
}

struct ServerStatus {
    bool online = false;
    std::string serverName = "Unknown";
    std::string motd;
    int playerCount = 0;
    int maxPlayers = 0;
    std::string ip;
    uint16_t port = 0;
};

static ServerStatus pingServer(const std::string& host, uint16_t port, int timeoutMs = 5000) {
    ServerStatus status;
    status.ip = host;
    status.port = port;

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    socket_t sock = INVALID_SOCKET;
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return status;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
#ifdef _WIN32
    DWORD tv = timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(res);
        CLOSESOCKET(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return status; // offline
    }
    freeaddrinfo(res);

    std::vector<uint8_t> handshake;
    writeVarInt(handshake, -1);
    writeString(handshake, host);
    writeUShort(handshake, port);
    writeVarInt(handshake, 1);
    sendPacket(sock, 0x00, handshake);

    sendPacket(sock, 0x00, {});

    int32_t totalLen, packetId;
    std::string json;
    bool ok = readVarInt(sock, totalLen)
           && readVarInt(sock, packetId)
           && packetId == 0x00
           && readString(sock, json);

    CLOSESOCKET(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    if (!ok) return status; // offline

    try {
        JsonParser parser(json);
        JsonValue root = parser.parse();

        if (auto* desc = root.find("description"))
            status.motd = extractMotd(*desc);

        if (auto* players = root.find("players")) {
            if (auto* online = players->find("online")) status.playerCount = (int)online->numVal;
            if (auto* max = players->find("max")) status.maxPlayers = (int)max->numVal;
        }

        if (auto* version = root.find("version"))
            if (auto* name = version->find("name"))
                status.serverName = name->strVal;

        status.online = true;
    } catch (...) {
        status.online = false;
    }

    return status;
}

static void printStatus(const ServerStatus& s) {
    std::cout << "-- " << s.serverName << " --\n";
    std::cout << "\xF0\x9F\x8C\x90 Online Status: " << (s.online ? "Online" : "Offline") << "\n";
    std::cout << "\xF0\x9F\x93\xA2 MOTD: " << (s.online ? s.motd : "N/A") << "\n";
    std::cout << "\xF0\x9F\x91\xA5 Player Count: ";
    if (s.online) std::cout << s.playerCount << "/" << s.maxPlayers;
    else std::cout << "N/A";
    std::cout << "\n";
    std::cout << "\xF0\x9F\x9B\x9C IP: " << s.ip << ":" << s.port << "\n";
}

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "play.thisminecraftserver.xyz";
    uint16_t port = argc > 2 ? (uint16_t)std::stoi(argv[2]) : 25565;

    ServerStatus status = pingServer(host, port);
    printStatus(status);
    return 0;
}