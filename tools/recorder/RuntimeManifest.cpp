#include "RuntimeManifest.h"
#include "Protocol.h"
#include <obs-module.h>
#include <bcrypt.h>
#include <psapi.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <stdexcept>

namespace recorder {
namespace fs = std::filesystem;
static std::string hashFile(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("sha256-provider");
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("sha256-init"); }
    std::ifstream input(path, std::ios::binary);
    char buffer[65536]; bool ok = input.good();
    while (ok && input.read(buffer, sizeof(buffer)).gcount()) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(input.gcount()), 0) >= 0;
    unsigned char digest[32];
    ok = ok && input.eof() && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) throw std::runtime_error("runtime-hash-read");
    std::ostringstream text;
    for (auto byte : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return text.str();
}
static fs::path child(const fs::path& root, const std::string& relative) {
    fs::path part = wide(relative);
    if (part.empty() || part.is_absolute() || part.has_root_name()) throw std::runtime_error("runtime-path");
    fs::path current = root;
    for (const auto& element : part) {
        if (element == L".." || element == L".") throw std::runtime_error("runtime-traversal");
        current /= element;
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || attrs & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("runtime-missing-or-reparse");
    }
    return current;
}
RuntimeManifest::RuntimeManifest(fs::path path) : root(fs::absolute(std::move(path)).lexically_normal()) {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) || !SetDllDirectoryW(L"") || !AddDllDirectory(root.c_str())) throw std::runtime_error("private-dll-policy");
}
void RuntimeManifest::verify() {
    manifestIdentity = hashFile(root / L"runtime-manifest.json");
    DWORD rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES || rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("runtime-root-reparse");
    std::ifstream input(root / L"runtime-manifest.json", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.size() > 2 * 1024 * 1024) throw std::runtime_error("runtime-manifest-size");
    Data data(obs_data_create_from_json(text.c_str()));
    if (!data.value || obs_data_get_int(data, "schema") != 1 || obs_data_get_int(data, "obsApi") != LIBOBS_API_VER || stringField(data, "sourceCommit") != "72e86655715d9a25d69ce446aa4dd44ba58e10d2") throw std::runtime_error("runtime-manifest-version");
    auto* files = obs_data_get_array(data, "files");
    if (!files) throw std::runtime_error("runtime-manifest-files");
    try {
        std::set<std::string> seen;
        for (size_t i = 0; i < obs_data_array_count(files); ++i) {
            Data file(obs_data_array_item(files, i));
            std::string name = stringField(file, "path"), expected = stringField(file, "sha256");
            if (!seen.insert(name).second || hashFile(child(root, name)) != expected) throw std::runtime_error("runtime-integrity:" + name);
        }
        for (auto* name : {"obs.dll", "CGPlayRecorder.exe", "libobs-d3d11.dll", "libobs-winrt.dll", "obs-ffmpeg-mux.exe"}) if (!seen.count(name)) throw std::runtime_error("runtime-required-file");
        for (const auto& file : fs::recursive_directory_iterator(root)) {
            if (file.is_regular_file() && (file.path().extension() == L".dll" || file.path().extension() == L".exe") &&
                !seen.count(file.path().lexically_relative(root).generic_string())) throw std::runtime_error("unlisted-runtime-binary");
        }
    } catch (...) { obs_data_array_release(files); throw; }
    obs_data_array_release(files);
    if (obs_get_version() != LIBOBS_API_VER) throw std::runtime_error("runtime-abi-mismatch");
    auto* list = obs_data_get_array(data, "modules");
    if (!list || obs_data_array_count(list) != 4) { if (list) obs_data_array_release(list); throw std::runtime_error("runtime-module-list"); }
    try {
        std::set<std::string> ids;
        for (size_t i = 0; i < obs_data_array_count(list); ++i) {
            Data module(obs_data_array_item(list, i));
            auto id = stringField(module, "id");
            if (!std::set<std::string>{"obs-x264", "obs-ffmpeg", "win-capture", "win-wasapi"}.count(id) || !ids.insert(id).second) throw std::runtime_error("runtime-module-not-allowed");
            if (stringField(module, "path") != "core/" + id + "/" + id + ".dll" || stringField(module, "data") != "core/" + id + "/data") throw std::runtime_error("runtime-module-path");
            modules.push_back({id, child(root, stringField(module, "path")), child(root, stringField(module, "data"))});
        }
    } catch (...) { obs_data_array_release(list); throw; }
    obs_data_array_release(list);
}
void RuntimeManifest::loadModules() {
    for (const auto& entry : modules) {
        obs_module_t* module = nullptr;
        if (obs_open_module(&module, utf8(entry.dll.wstring()).c_str(), utf8(entry.data.wstring()).c_str()) != MODULE_SUCCESS || !obs_init_module(module)) throw std::runtime_error("module-init:" + entry.id);
        if (entry.id == "win-capture" || entry.id == "win-wasapi") {
            auto handle = GetModuleHandleW(entry.dll.c_str());
            auto version = reinterpret_cast<int(*)()>(GetProcAddress(handle, entry.id == "win-capture" ? "cgplay_recorder_patch_version" : "cgplay_recorder_wasapi_patch_version"));
            if (!version || version() != 1) throw std::runtime_error("capture-patch-required");
        }
    }
    obs_post_load_modules();
    verifyLoadedModules();
}
void RuntimeManifest::verifyLoadedModules() const {
    HMODULE handles[2048]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed) || needed > sizeof(handles)) throw std::runtime_error("loaded-module-enumeration");
    wchar_t windows[MAX_PATH]; GetWindowsDirectoryW(windows, MAX_PATH);
    auto contained = [](std::wstring path, std::wstring parent) {
        for (auto& ch : path) ch = towlower(ch);
        for (auto& ch : parent) ch = towlower(ch);
        return path == parent || path.starts_with(parent + L"\\");
    };
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t path[32768];
        if (!GetModuleFileNameW(handles[i], path, 32768)) throw std::runtime_error("loaded-module-path");
        if (!contained(path, root.wstring()) && !contained(path, windows)) throw std::runtime_error("foreign-runtime-module:" + utf8(path));
        if (fs::path(path).filename().wstring().starts_with(L"Qt")) throw std::runtime_error("qt-runtime-forbidden");
    }
}
}
