// Background integration smoke for the real Windows thumbnail cache path.
// This deliberately uses IThumbnailCache rather than an image factory that
// can silently fall back to the file association icon. It never registers a
// handler, opens Explorer, or changes the user's registry/settings.
#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct WindowCounts {
    unsigned platform = 0;
    unsigned own = 0;
};

WindowCounts visibleWindows()
{
    WindowCounts result;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window)) return TRUE;
        auto& counts = *reinterpret_cast<WindowCounts*>(parameter);
        ++counts.platform;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == GetCurrentProcessId()) ++counts.own;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

struct WindowMonitor {
    std::atomic<bool> stop{false};
    std::atomic<unsigned> ownMaximum{0};
    std::thread worker;

    WindowMonitor() : worker([this] {
        do {
            const auto counts = visibleWindows();
            ownMaximum.store(std::max(ownMaximum.load(), counts.own));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (!stop.load());
    }) {}

    void finish()
    {
        stop.store(true);
        if (worker.joinable()) worker.join();
        ownMaximum.store(std::max(ownMaximum.load(), visibleWindows().own));
    }

    ~WindowMonitor() { finish(); }
};

bool writeAll(HANDLE file, const void* bytes, DWORD size)
{
    DWORD written = 0;
    return WriteFile(file, bytes, size, &written, nullptr) && written == size;
}

bool saveBitmap(const std::wstring& path, int width, int height,
                const std::vector<std::uint32_t>& pixels)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto byteCount = static_cast<DWORD>(pixels.size() * sizeof(pixels.front()));
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + byteCount;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = byteCount;
    const bool saved = writeAll(file, &header, sizeof(header)) &&
        writeAll(file, &info, sizeof(info)) && writeAll(file, pixels.data(), byteCount);
    CloseHandle(file);
    return saved;
}

void printHr(const char* name, HRESULT value)
{
    std::cout << "  \"" << name << "\": \"0x" << std::hex << std::uppercase
              << std::setfill('0') << std::setw(8) << static_cast<std::uint32_t>(value)
              << std::dec << "\",\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: windows_thumbnail_smoke MEDIA [--bmp OUTPUT.bmp] "
                     "[--size 256] [--expect-variation]\n";
        return 2;
    }
    UINT requestedSize = 256;
    std::wstring bitmapPath;
    bool expectVariation = false;
    for (int index = 2; index < argc; ++index) {
        const std::wstring argument(argv[index]);
        if (argument == L"--bmp" && index + 1 < argc) {
            bitmapPath = argv[++index];
        } else if (argument == L"--size" && index + 1 < argc) {
            wchar_t* end = nullptr;
            const auto size = wcstoul(argv[++index], &end, 10);
            if (!end || *end || size < 1 || size > 1024) return 2;
            requestedSize = static_cast<UINT>(size);
        } else if (argument == L"--expect-variation") {
            expectVariation = true;
        } else {
            std::cerr << "Unknown or incomplete argument\n";
            return 2;
        }
    }

    // The Shell parsing API expects a fully qualified native path (unlike
    // CreateFile, it rejects forward slashes on this Windows version).
    std::wstring sourcePath(argv[1]);
    std::replace(sourcePath.begin(), sourcePath.end(), L'/', L'\\');
    const DWORD pathLength = GetFullPathNameW(sourcePath.c_str(), 0, nullptr, nullptr);
    if (pathLength > 0) {
        std::vector<wchar_t> fullPath(pathLength);
        const DWORD written = GetFullPathNameW(sourcePath.c_str(), pathLength,
                                               fullPath.data(), nullptr);
        if (written > 0 && written < pathLength) sourcePath.assign(fullPath.data(), written);
    }

    const auto initialWindows = visibleWindows();
    WindowMonitor monitor;
    const auto start = std::chrono::steady_clock::now();
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT itemHr = E_UNEXPECTED;
    HRESULT cacheHr = E_UNEXPECTED;
    HRESULT thumbnailHr = E_UNEXPECTED;
    HRESULT bitmapHr = E_UNEXPECTED;
    WTS_CACHEFLAGS flags = WTS_DEFAULT;
    WTS_THUMBNAILID thumbnailId{};
    int width = 0;
    int height = 0;
    unsigned uniqueRgb = 0;
    bool validPixels = false;
    bool bitmapSaved = bitmapPath.empty();
    {
        ComPtr<IShellItem> item;
        ComPtr<IThumbnailCache> cache;
        ComPtr<ISharedBitmap> sharedBitmap;
        if (SUCCEEDED(comHr)) {
            itemHr = SHCreateItemFromParsingName(sourcePath.c_str(), nullptr, IID_PPV_ARGS(&item));
            cacheHr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
            if (SUCCEEDED(itemHr) && SUCCEEDED(cacheHr)) {
                thumbnailHr = cache->GetThumbnail(item.Get(), requestedSize,
                    WTS_FORCEEXTRACTION, &sharedBitmap, &flags, &thumbnailId);
            }
        }
        if (SUCCEEDED(thumbnailHr) && sharedBitmap) {
            // ISharedBitmap owns this handle; do not call DeleteObject on it.
            HBITMAP bitmap = nullptr;
            bitmapHr = sharedBitmap->GetSharedBitmap(&bitmap);
            BITMAP description{};
            if (SUCCEEDED(bitmapHr) && bitmap &&
                GetObjectW(bitmap, sizeof(description), &description)) {
                width = description.bmWidth;
                height = description.bmHeight;
                if (width > 0 && height > 0 && width <= 4096 && height <= 4096) {
                    BITMAPINFO info{};
                    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth = width;
                    info.bmiHeader.biHeight = -height;
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
                    HDC dc = GetDC(nullptr);
                    validPixels = dc && GetDIBits(dc, bitmap, 0, height, pixels.data(),
                                                 &info, DIB_RGB_COLORS) == height;
                    if (dc) ReleaseDC(nullptr, dc);
                    if (validPixels) {
                        std::set<std::uint32_t> colors;
                        for (const auto pixel : pixels) colors.insert(pixel & 0x00ffffff);
                        uniqueRgb = static_cast<unsigned>(colors.size());
                        if (!bitmapPath.empty()) {
                            bitmapSaved = saveBitmap(bitmapPath, width, height, pixels);
                        }
                    }
                }
            }
        }
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    monitor.finish();
    const auto finalWindows = visibleWindows();
    const auto ownVisible = std::max(initialWindows.own, monitor.ownMaximum.load());
    const bool thumbnailOnly = SUCCEEDED(thumbnailHr) && SUCCEEDED(bitmapHr) && validPixels;
    const bool passed = thumbnailOnly && bitmapSaved && ownVisible == 0 &&
                        (!expectVariation || uniqueRgb > 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << std::boolalpha << "{\n"
              << "  \"passed\": " << passed << ",\n"
              << "  \"background_mode\": true,\n"
              << "  \"visible_test_process_windows\": " << ownVisible << ",\n"
              << "  \"platform_visible_top_level_windows_before\": " << initialWindows.platform << ",\n"
              << "  \"platform_visible_top_level_windows_after\": " << finalWindows.platform << ",\n"
              << "  \"extraction_api\": \"IThumbnailCache::GetThumbnail\",\n"
              << "  \"force_extraction\": true,\n"
              << "  \"allows_icon_fallback\": false,\n"
              << "  \"thumbnail_returned\": " << thumbnailOnly << ",\n";
    printHr("com_hresult", comHr);
    printHr("item_hresult", itemHr);
    printHr("cache_hresult", cacheHr);
    printHr("thumbnail_hresult", thumbnailHr);
    printHr("bitmap_hresult", bitmapHr);
    std::cout << "  \"cache_flags\": " << static_cast<unsigned>(flags) << ",\n"
              << "  \"requested_size\": " << requestedSize << ",\n"
              << "  \"width\": " << width << ",\n"
              << "  \"height\": " << height << ",\n"
              << "  \"unique_rgb_values\": " << uniqueRgb << ",\n"
              << "  \"expect_pixel_variation\": " << expectVariation << ",\n"
              << "  \"bitmap_requested\": " << !bitmapPath.empty() << ",\n"
              << "  \"bitmap_saved\": " << (!bitmapPath.empty() && bitmapSaved) << ",\n"
              << "  \"elapsed_ms\": " << elapsed << "\n}\n";
    return passed ? 0 : 1;
}
