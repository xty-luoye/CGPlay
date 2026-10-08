#include "CGPlayThumbnailProvider.h"

#ifdef _WIN32

#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr CLSID kClassId = {
    0xB71A2E3C, 0x9D47, 0x4B1C, {0x8D, 0x4D, 0x2D, 0x4C, 0x04, 0xE1, 0xD9, 0xA7}};

HMODULE g_module = nullptr;
volatile long g_objectCount = 0;
volatile long g_lockCount = 0;

std::wstring quoteArg(const std::wstring& value)
{
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'\"') quoted += L'\\';
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring moduleDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(g_module, buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) return {};
    std::wstring path(buffer, length);
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

bool renderFrame(const std::wstring& source, UINT cx, std::vector<BYTE>* output)
{
    if (!output || source.empty()) return false;
    const std::wstring ffmpeg = moduleDirectory() + L"\\ffmpeg.exe";
    if (GetFileAttributesW(ffmpeg.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    const UINT size = std::clamp<UINT>(cx, 32, 1024);
    const std::wstring filter =
        L"scale=" + std::to_wstring(size) + L":" + std::to_wstring(size) +
        L":force_original_aspect_ratio=decrease,pad=" + std::to_wstring(size) + L":" +
        std::to_wstring(size) + L":(ow-iw)/2:(oh-ih)/2:color=black";
    std::wstring command = quoteArg(ffmpeg) +
        L" -hide_banner -loglevel error -threads 1 -ss 0.25 -i " + quoteArg(source) +
        L" -an -frames:v 1 -vf " + quoteArg(filter) +
        L" -f image2pipe -vcodec mjpeg -q:v 4 pipe:1";

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullHandle = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = writePipe;
    startup.hStdError = nullHandle;
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL started = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, moduleDirectory().c_str(), &startup, &process);
    CloseHandle(writePipe);
    CloseHandle(nullHandle);
    if (!started) {
        CloseHandle(readPipe);
        return false;
    }

    output->clear();
    bool timedOut = false;
    ULONGLONG deadline = GetTickCount64() + 10000;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            std::vector<BYTE> chunk(std::min<DWORD>(available, 64 * 1024));
            DWORD read = 0;
            if (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) && read > 0) {
                output->insert(output->end(), chunk.begin(), chunk.begin() + read);
            }
        }
        const DWORD wait = WaitForSingleObject(process.hProcess, 25);
        if (wait == WAIT_OBJECT_0) {
            for (;;) {
                BYTE chunk[64 * 1024];
                DWORD read = 0;
                if (!ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
                output->insert(output->end(), chunk, chunk + read);
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            timedOut = true;
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return !timedOut && exitCode == 0 && !output->empty();
}

bool decodeBitmap(const std::vector<BYTE>& encoded, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    if (!bitmap || !alpha || encoded.empty()) return false;
    *bitmap = nullptr;
    *alpha = WTSAT_ARGB;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || !factory) {
        return false;
    }
    IStream* stream = SHCreateMemStream(encoded.data(), static_cast<UINT>(encoded.size()));
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool success = false;
    if (stream && SUCCEEDED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
        SUCCEEDED(converter->Initialize(
            frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0 && width <= 4096 && height <= 4096) {
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(width);
            info.bmiHeader.biHeight = -static_cast<LONG>(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (dib && pixels && SUCCEEDED(converter->CopyPixels(
                    nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels)))) {
                *bitmap = dib;
                success = true;
            } else if (dib) {
                DeleteObject(dib);
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return success;
}

class ThumbnailProvider final : public IThumbnailProvider, public IInitializeWithFile
{
public:
    ThumbnailProvider() { InterlockedIncrement(&g_objectCount); }
    ~ThumbnailProvider() { InterlockedDecrement(&g_objectCount); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IThumbnailProvider)) {
            *object = static_cast<IThumbnailProvider*>(this);
        } else if (riid == __uuidof(IInitializeWithFile)) {
            *object = static_cast<IInitializeWithFile*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (count == 0) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR filePath, DWORD) override
    {
        if (!filePath || !*filePath) return E_INVALIDARG;
        if (!filePath_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        filePath_ = filePath;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override
    {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr;
        *alpha = WTSAT_UNKNOWN;
        std::vector<BYTE> encoded;
        if (!renderFrame(filePath_, cx, &encoded)) return E_FAIL;
        return decodeBitmap(encoded, bitmap, alpha) ? S_OK : E_FAIL;
    }

private:
    volatile long refCount_ = 1;
    std::wstring filePath_;
};

class ClassFactory final : public IClassFactory
{
public:
    ClassFactory() { InterlockedIncrement(&g_objectCount); }
    ~ClassFactory() { InterlockedDecrement(&g_objectCount); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* provider = new (std::nothrow) ThumbnailProvider();
        if (!provider) return E_OUTOFMEMORY;
        const HRESULT result = provider->QueryInterface(riid, object);
        provider->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_lockCount);
        else InterlockedDecrement(&g_lockCount);
        return S_OK;
    }

private:
    volatile long refCount_ = 1;
};

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (rclsid != kClassId) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(riid, object);
    factory->Release();
    return result;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

#else

int cgplayThumbnailProviderTranslationUnitStub = 0;

#endif
