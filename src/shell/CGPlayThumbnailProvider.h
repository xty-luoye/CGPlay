#pragma once

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv);
extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow();
#endif
