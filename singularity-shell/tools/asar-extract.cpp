// asar-extract — minimal extractor for the subset of the Electron asar format
// needed by singularity-shell: pulls "build/**" and "package.json" out of
// app.asar. No third-party dependencies (C++17 STL only).
//
// Asar layout (reverse-engineered, matches Chromium Pickle serialization):
//   offset 0 : uint32 LE  = 4                      (pickle size of next field)
//   offset 4 : uint32 LE  = header_pickle_size
//   offset 8 : uint32 LE  = header_string_pickle_size
//   offset 12: uint32 LE  = json_length
//   offset 16: JSON file table (json_length bytes), padded to 4 bytes
//   data     : file payloads; each entry's "offset" is relative to
//              data_base = 16 + align4(json_length)
//
// Entries marked "unpacked": true are NOT stored in the archive (they live in
// app.asar.unpacked/) and are skipped — singularity-shell does not need them.
//
// Usage: asar-extract <app.asar> <output-dir>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint32_t readU32(const std::vector<char>& b, size_t off) {
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;  // x86/arm are little-endian; fine for our targets
}

// --- Minimal JSON cursor: we only need { "files": { name: {...} } }, -------
// --- "size", "offset" (decimal string), "unpacked" (bool), "files" (dir). --

struct JsonCursor {
    const std::string& s;
    size_t p = 0;
    explicit JsonCursor(const std::string& str) : s(str) {}

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r')) ++p; }
    bool consume(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    void expect(char c) { if (!consume(c)) throw std::runtime_error(std::string("JSON: expected '") + c + "' at " + std::to_string(p)); }

    std::string string() {
        expect('"');
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\') {
                ++p;
                if (p >= s.size()) break;
                char e = s[p++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'u': p += 4; out += '?'; break;  // asar names are ASCII in practice
                    default: out += e;
                }
            } else out += s[p++];
        }
        expect('"');
        return out;
    }

    void skipValue() {
        ws();
        if (p >= s.size()) return;
        char c = s[p];
        if (c == '"') { string(); return; }
        if (c == '{') {
            ++p; ws();
            if (consume('}')) return;
            do { string(); expect(':'); skipValue(); } while (consume(','));
            expect('}'); return;
        }
        if (c == '[') {
            ++p; ws();
            if (consume(']')) return;
            do { skipValue(); } while (consume(','));
            expect(']'); return;
        }
        while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '.' || s[p] == '-' || s[p] == '+')) ++p;
    }

    std::string rawToken() {  // number / true / false as text
        ws();
        size_t start = p;
        while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '.' || s[p] == '-')) ++p;
        return s.substr(start, p - start);
    }
};

struct FileEntry {
    std::string path;    // "build/index.html"
    uint64_t offset = 0;
    uint64_t size = 0;
    bool unpacked = false;
};

static void walk(JsonCursor& cur, const std::string& prefix, std::vector<FileEntry>& out) {
    // Cursor is at the '{' of a node's "files" object.
    cur.expect('{');
    cur.ws();
    if (cur.consume('}')) return;
    do {
        std::string name = cur.string();
        cur.expect(':');
        cur.expect('{');
        FileEntry fe;
        fe.path = prefix.empty() ? name : prefix + "/" + name;
        bool hasFiles = false;
        cur.ws();
        if (!cur.consume('}')) {
            do {
                std::string key = cur.string();
                cur.expect(':');
                if (key == "size") {
                    fe.size = std::stoull(cur.rawToken());
                } else if (key == "offset") {
                    cur.ws();
                    if (cur.s[cur.p] == '"') { cur.expect('"'); fe.offset = std::stoull(cur.rawToken()); cur.expect('"'); }
                    else fe.offset = std::stoull(cur.rawToken());
                } else if (key == "unpacked") {
                    fe.unpacked = (cur.rawToken() == "true");
                } else if (key == "files") {
                    hasFiles = true;
                    walk(cur, fe.path, out);
                } else {
                    cur.skipValue();
                }
            } while (cur.consume(','));
            cur.expect('}');
        }
        if (!hasFiles && !fe.unpacked) out.push_back(std::move(fe));
    } while (cur.consume(','));
    cur.expect('}');
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: asar-extract <app.asar> <output-dir>\n";
        return 2;
    }
    const fs::path asarPath = argv[1];
    const fs::path outRoot  = argv[2];

    std::ifstream in(asarPath, std::ios::binary);
    if (!in) { std::cerr << "cannot open " << asarPath << "\n"; return 1; }

    std::vector<char> head(16);
    in.read(head.data(), 16);
    if (in.gcount() != 16) { std::cerr << "truncated header\n"; return 1; }

    const uint32_t jsonLen = readU32(reinterpret_cast<std::vector<char>&>(head), 12);
    const uint64_t dataBase = 16ull + ((uint64_t(jsonLen) + 3ull) & ~3ull);

    std::string json(jsonLen, '\0');
    in.read(json.data(), jsonLen);
    if ((uint32_t)in.gcount() != jsonLen) { std::cerr << "truncated file table\n"; return 1; }

    std::vector<FileEntry> files;
    try {
        JsonCursor cur(json);
        cur.expect('{');
        do {
            std::string key = cur.string();
            cur.expect(':');
            if (key == "files") walk(cur, "", files);
            else cur.skipValue();
        } while (cur.consume(','));
        cur.expect('}');
    } catch (const std::exception& e) {
        std::cerr << "asar header parse error: " << e.what() << "\n";
        return 1;
    }

    size_t extracted = 0, skipped = 0;
    std::vector<char> buf(1 << 20);
    for (const auto& fe : files) {
        // Extract only what the shell serves: build/** and package.json.
        const bool wanted = fe.path.rfind("build/", 0) == 0 || fe.path == "package.json";
        if (!wanted) { ++skipped; continue; }

        const fs::path dest = outRoot / fe.path;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        if (ec) { std::cerr << "mkdir failed: " << dest.parent_path() << "\n"; return 1; }

        in.clear();
        in.seekg((std::streamoff)(dataBase + fe.offset), std::ios::beg);
        if (!in) { std::cerr << "seek failed for " << fe.path << "\n"; return 1; }

        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) { std::cerr << "cannot write " << dest << "\n"; return 1; }

        uint64_t left = fe.size;
        while (left > 0) {
            const size_t chunk = (size_t)std::min<uint64_t>(left, buf.size());
            in.read(buf.data(), chunk);
            if ((size_t)in.gcount() != chunk) { std::cerr << "short read: " << fe.path << "\n"; return 1; }
            out.write(buf.data(), chunk);
            left -= chunk;
        }
        ++extracted;
    }

    std::cout << "extracted " << extracted << " files (skipped " << skipped
              << " non-shell entries) -> " << outRoot << "\n";
    return 0;
}
