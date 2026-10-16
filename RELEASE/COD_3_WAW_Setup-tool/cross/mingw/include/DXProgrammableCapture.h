/* DXProgrammableCapture.h for llvm-mingw, which doesn't ship it.
 *
 * Declares the one interface the ReXGlue SDK uses from it: the graphics
 * debugger's capture control (IDXGraphicsAnalysis, two methods), as
 * documented by Microsoft. The SDK only calls it when a graphics debugger
 * is attached. */
#pragma once
#include <unknwn.h>

#ifdef __cplusplus
struct __declspec(uuid("9f251514-9d4d-4902-9d60-18988ab7d4b5")) __declspec(novtable) IDXGraphicsAnalysis
    : public IUnknown {
  virtual void STDMETHODCALLTYPE BeginCapture() = 0;
  virtual void STDMETHODCALLTYPE EndCapture() = 0;
};
__CRT_UUID_DECL(IDXGraphicsAnalysis, 0x9f251514, 0x9d4d, 0x4902, 0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5)
#endif
