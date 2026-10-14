// SPDX-License-Identifier: GPL-3.0-only
// nvngx.dll_nrshim.dll -- a generic "call from a module whose path contains nvngx.dll" trampoline.
//
// nvngx_dlssnr.dll resolves the module owning its return address (RtlPcToFileHeader) and refuses
// callers whose path lacks "nvngx.dll". Same idea as OptiScaler's nvngx.dll_dlssnr.dll forwarder, but
// argument-agnostic: every snippet entry point we use takes <= 5 integer/pointer arguments, which on
// Win64 all travel in RCX/RDX/R8/R9 + stack, so one 5-arg trampoline serves all of them.
//
// The result MUST be stored in a volatile before returning: `return fn(...)` would compile to a tail
// jmp, this module's frame would vanish and the snippet would see our caller (the exe) instead.
// Built with -fno-optimize-sibling-calls as a second guard; verified with objdump (call, not jmp).
#include <windows.h>
#include <cstdint>

typedef long long (*Fn5)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

extern "C" __declspec(dllexport) __attribute__((noinline)) long long nrshim_call(void* fn, uintptr_t a, uintptr_t b,
                                                                                  uintptr_t c, uintptr_t d,
                                                                                  uintptr_t e) {
  volatile long long r = ((Fn5)fn)(a, b, c, d, e);
  return r;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
