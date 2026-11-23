/**
 * @file vk_msvc_compat.h
 * @brief GCC/Clang builtins used by Slipstream's Vulkan code, for Microsoft's compiler.
 *
 * Prestige Firestorm-Vulkan port. Slipstream builds its Windows viewer with clang-cl, which
 * understands __builtin_*; Firestorm builds with MSVC (cl.exe), which does not. CMake
 * force-includes this header (/FI) into the ported vkgfx sources on MSVC only, so the
 * Slipstream files themselves stay unchanged.
 */
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#include <bit>
#include <cstdint>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#ifndef __builtin_return_address
#define __builtin_return_address(level) _ReturnAddress()
#endif

static inline int __builtin_clz(unsigned int x)          { return std::countl_zero(x); }
static inline int __builtin_clzll(unsigned long long x)  { return std::countl_zero(x); }
static inline int __builtin_ctz(unsigned int x)          { return std::countr_zero(x); }
static inline int __builtin_ctzll(unsigned long long x)  { return std::countr_zero(x); }
static inline int __builtin_popcount(unsigned int x)     { return std::popcount(x); }
static inline int __builtin_popcountll(unsigned long long x) { return std::popcount(x); }
static inline int __builtin_ffs(int x)                   { return x ? std::countr_zero((unsigned int)x) + 1 : 0; }
static inline int __builtin_ffsll(long long x)           { return x ? std::countr_zero((unsigned long long)x) + 1 : 0; }
#endif
