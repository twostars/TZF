﻿/**
 * This file is part of Tales of Zestiria "Fix".
 *
 * Tales of Zestiria "Fix" is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Tales of Zestiria "Fix" is distributed in the hope that it will be
 * useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tales of Zestiria "Fix".
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/

#define _CRT_SECURE_NO_WARNINGS

#define NOMINMAX
#include <algorithm>

#include "framerate.h"
#include "config.h"
#include "log.h"
#include "hook.h"
#include "scanner.h"

#include "priest.lua.h"

#include "render.h"
#include "textures.h"
#include <d3d9.h>

uint32_t TICK_ADDR_BASE = 0x217B464;
// 0x0217B3D4  1.3

uint8_t          tzf::FrameRateFix::old_speed_reset_code2   [7];
uint8_t          tzf::FrameRateFix::old_limiter_instruction [6];
int32_t          tzf::FrameRateFix::tick_scale               = 2; // 30 FPS

CRITICAL_SECTION tzf::FrameRateFix::alter_speed_cs           = { 0 };

bool             tzf::FrameRateFix::variable_speed_installed = false;
bool             tzf::FrameRateFix::fullscreen               = false;

uint32_t         tzf::FrameRateFix::target_fps               = 30;

HMODULE          tzf::FrameRateFix::bink_dll                 = 0;
HMODULE          tzf::FrameRateFix::kernel32_dll             = 0;

float limiter_tolerance = 0.40f;
int   max_latency       = 2;
bool  wait_for_vblank   = true;

typedef void (WINAPI *Sleep_pfn)(DWORD dwMilliseconds);
Sleep_pfn Sleep_Original = nullptr;

typedef BOOL(WINAPI *QueryPerformanceCounter_pfn)(_Out_ LARGE_INTEGER *lpPerformanceCount);
QueryPerformanceCounter_pfn QueryPerformanceCounter_Original = nullptr;

class FramerateLimiter
{
public:
  FramerateLimiter (double target = 60.0) {
     init (target);
   }
  ~FramerateLimiter (void) {
   }

  void init (double target) {
    ms  = 1000.0 / target;
    fps = target;

    frames = 0;

    IDirect3DDevice9Ex* d3d9ex = nullptr;
    if (tzf::RenderFix::pDevice != nullptr) {
      tzf::RenderFix::pDevice->QueryInterface ( 
                         __uuidof (IDirect3DDevice9Ex),
                           (void **)&d3d9ex );
    }

    QueryPerformanceFrequency (&freq);

    // Align the start to VBlank for minimum input latency
    if (d3d9ex != nullptr) {
      d3d9ex->SetMaximumFrameLatency (1);
      d3d9ex->WaitForVBlank          (0);
      d3d9ex->SetMaximumFrameLatency (max_latency);
      d3d9ex->Release                ();
    }

    QueryPerformanceCounter_Original (&start);

    next.QuadPart = 0ULL;
    time.QuadPart = 0ULL;
    last.QuadPart = 0ULL;

    last.QuadPart = static_cast<LONGLONG> (start.QuadPart - (ms / 1000.0) * freq.QuadPart);
    next.QuadPart = static_cast<LONGLONG> (start.QuadPart + (ms / 1000.0) * freq.QuadPart);
  }

  void wait (void) {
    static bool restart = false;

    frames++;

    QueryPerformanceCounter_Original (&time);

    if ((double)(time.QuadPart - next.QuadPart) / (double)freq.QuadPart / (ms / 1000.0) > (limiter_tolerance * fps)) {
      //dll_log->Log ( L" * Frame ran long (%3.01fx expected) - restarting"
                     //L" limiter...",
             //(double)(time.QuadPart - next.QuadPart) / (double)freq.QuadPart / (ms / 1000.0) / fps );
      restart = true;
    }

    if (restart) {
      frames         = 0;
      start.QuadPart = static_cast<LONGLONG> (time.QuadPart + (ms / 1000.0) * (double)freq.QuadPart);
      restart        = false;
    }

    next.QuadPart = static_cast<LONGLONG> ((start.QuadPart + (double)frames * (ms / 1000.0) * (double)freq.QuadPart));

    if (next.QuadPart > 0ULL) {
      // If available (Windows 7+), wait on the swapchain
      IDirect3DDevice9Ex* d3d9ex = nullptr;
      if (tzf::RenderFix::pDevice != nullptr) {
        tzf::RenderFix::pDevice->QueryInterface ( 
                           __uuidof (IDirect3DDevice9Ex),
                             (void **)&d3d9ex );
      }

      while (time.QuadPart < next.QuadPart) {
        if (wait_for_vblank && (double)(next.QuadPart - time.QuadPart) > (0.0166667 * (double)freq.QuadPart)) {
          if (d3d9ex != nullptr) {
            d3d9ex->WaitForVBlank (0);
          }
        }

        if (GetForegroundWindow () != tzf::RenderFix::hWndDevice &&
            tzf::FrameRateFix::fullscreen) {
          //dll_log->Log (L"[FrameLimit]  # Restarting framerate limiter; fullscreen Alt+Tab...");
          restart = true;
          break;
        }

        QueryPerformanceCounter_Original (&time);
      }

      if (d3d9ex != nullptr)
        d3d9ex->Release ();
    }

    else {
      dll_log->Log (L"[FrameLimit] Lost time");
      start.QuadPart += -next.QuadPart;
    }

    last.QuadPart = time.QuadPart;
  }

  void change_limit (double target) {
    init (target);
  }

private:
    double ms, fps;

    LARGE_INTEGER start, last, next, time, freq;

    uint32_t frames;
} *limiter = nullptr;


typedef D3DPRESENT_PARAMETERS* (__stdcall *SK_SetPresentParamsD3D9_pfn)
  (IDirect3DDevice9*      device,
   D3DPRESENT_PARAMETERS* pparams);
SK_SetPresentParamsD3D9_pfn SK_SetPresentParamsD3D9_Original = nullptr;

COM_DECLSPEC_NOTHROW
D3DPRESENT_PARAMETERS*
__stdcall
SK_SetPresentParamsD3D9_Detour (IDirect3DDevice9*      device,
                                 D3DPRESENT_PARAMETERS* pparams)
{
  D3DPRESENT_PARAMETERS present_params;

  //
  // TODO: Figure out what the hell is doing this when RTSS is allowed to use
  //         custom D3D libs. 1x1@0Hz is obviously NOT for rendering!
  //
  if ( pparams->BackBufferWidth            == 1 &&
       pparams->BackBufferHeight           == 1 &&
       pparams->FullScreen_RefreshRateInHz == 0 ) {
    dll_log->Log (L"[   D3D9   ] * Fake D3D9Ex Device Detected... Ignoring!");
    return SK_SetPresentParamsD3D9_Original (device, pparams);
  }

  tzf::RenderFix::pDevice             = device;
  tzf::RenderFix::pPostProcessSurface = nullptr;

  if (pparams != nullptr) {
    memcpy (&present_params, pparams, sizeof D3DPRESENT_PARAMETERS);

    if (device != nullptr) {
      dll_log->Log ( L"[   D3D9   ] %% Caught D3D9 Swapchain :: Fullscreen=%s "
                     L" (%lux%lu@%lu Hz) "
                     L" [Device Window: 0x%04X]",
                       pparams->Windowed ? L"False" :
                                           L"True",
                         pparams->BackBufferWidth,
                           pparams->BackBufferHeight,
                             pparams->FullScreen_RefreshRateInHz,
                               pparams->hDeviceWindow );
    }

    tzf::RenderFix::hWndDevice    = pparams->hDeviceWindow;

    tzf::RenderFix::width         = present_params.BackBufferWidth;
    tzf::RenderFix::height        = present_params.BackBufferHeight;
    tzf::FrameRateFix::fullscreen = (! pparams->Windowed);

    // Change the Aspect Ratio
    char szAspectCommand [64];
    sprintf (szAspectCommand, "AspectRatio %f", (float)tzf::RenderFix::width / (float)tzf::RenderFix::height);

    SK_GetCommandProcessor ()->ProcessCommandLine (szAspectCommand);
  }

  return SK_SetPresentParamsD3D9_Original (device, pparams);
}

bool          render_sleep0  = false;
LARGE_INTEGER last_perfCount = { 0 };

void
WINAPI
Sleep_Detour (DWORD dwMilliseconds)
{
  if ((! config.framerate.disable_limiter) && GetCurrentThreadId () == tzf::RenderFix::dwRenderThreadID) {
    if (dwMilliseconds == 0)
      render_sleep0 = true;
    else {
      render_sleep0 = false;
    }
  }

  if (config.framerate.yield_processor && dwMilliseconds == 0)
    YieldProcessor ();

  if (dwMilliseconds != 0 || config.framerate.allow_fake_sleep) {
    Sleep_Original (dwMilliseconds);
  }
}

BOOL
WINAPI
QueryPerformanceCounter_Detour (_Out_ LARGE_INTEGER *lpPerformanceCount)
{
  BOOL ret = QueryPerformanceCounter_Original (lpPerformanceCount);

  DWORD dwThreadId = GetCurrentThreadId ();

  //
  // Handle threads that aren't render-related NORMALLY as well as the render
  //   thread when it DID NOT just voluntarily relinquish its scheduling
  //     timeslice
  //
  if (dwThreadId != tzf::RenderFix::dwRenderThreadID || (! render_sleep0) || tzf::RenderFix::bink) {
    if (dwThreadId == tzf::RenderFix::dwRenderThreadID)
      memcpy (&last_perfCount, lpPerformanceCount, sizeof (LARGE_INTEGER));

    return ret;
  }

  //
  // At this point, we're fixing up the thread that throttles the swapchain.
  //
  render_sleep0 = false;

  LARGE_INTEGER freq;
  QueryPerformanceFrequency (&freq);

  // Mess with the numbers slightly to prevent scheduling from wreaking havoc
  lpPerformanceCount->QuadPart += static_cast<LONGLONG> ((double)freq.QuadPart * ((double)tzf::FrameRateFix::target_fps / 1000.0));

  memcpy (&last_perfCount, lpPerformanceCount, sizeof (LARGE_INTEGER));

  return ret;
}

typedef void* (__stdcall *BinkOpen_pfn)(const char* filename, DWORD unknown0);
BinkOpen_pfn BinkOpen_Original = nullptr;

void*
__stdcall
BinkOpen_Detour ( const char* filename,
                  DWORD       unknown0 )
{
  // non-null on success
  void*       bink_ret = nullptr;
  static char szBypassName [MAX_PATH] = { '\0' };

  // Optionally play some other video (or no video)...
  if (! _stricmp (filename, "RAW\\MOVIE\\AM_TOZ_OP_001.BK2")) {
    dll_log->LogEx (true, L"[IntroVideo] >> Using %ws for Opening Movie ...",
                    config.system.intro_video.c_str ());

    sprintf (szBypassName, "%ws", config.system.intro_video.c_str ());

    bink_ret = BinkOpen_Original (szBypassName, unknown0);

    dll_log->LogEx ( false,
                       L" %s!\n",
                         bink_ret != nullptr ? L"Success" :
                                               L"Failed" );
  } else {
    bink_ret = BinkOpen_Original (filename, unknown0);
  }

  if (bink_ret != nullptr) {
    dll_log->Log (L"[FrameLimit] * Disabling TargetFPS -- Bink Video Opened");

    tzf::RenderFix::bink = true;
    tzf::FrameRateFix::Begin30FPSEvent ();
  }

  return bink_ret;
}

typedef void (__stdcall *BinkClose_pfn)(DWORD unknown);
BinkClose_pfn BinkClose_Original = nullptr;

void
__stdcall
BinkClose_Detour (DWORD unknown)
{
  BinkClose_Original (unknown);

  dll_log->Log (L"[FrameLimit] * Restoring TargetFPS -- Bink Video Closed");

  tzf::RenderFix::bink = false;
  tzf::FrameRateFix::End30FPSEvent ();
}



LPVOID pfnQueryPerformanceCounter = nullptr;
LPVOID pfnSleep                   = nullptr;
LPVOID pfnSK_SetPresentParamsD3D9 = nullptr;

// Hook these to properly synch audio subtitles during FMVs
LPVOID pfnBinkOpen                 = nullptr;
LPVOID pfnBinkClose                = nullptr;

void
TZF_FlushInstructionCache ( LPCVOID base_addr,
                            size_t  code_size )
{
  FlushInstructionCache ( GetCurrentProcess (),
                            base_addr,
                              code_size );
}

void
TZF_InjectByteCode ( LPVOID   base_addr,
                     uint8_t* new_code,
                     size_t   code_size,
                     DWORD    permissions,
                     uint8_t* old_code = nullptr )
{
  DWORD dwOld;

  VirtualProtect (base_addr, code_size, permissions, &dwOld);
  {
    if (old_code != nullptr)
      memcpy (old_code, base_addr, code_size);

    memcpy (base_addr, new_code, code_size);
  }
  VirtualProtect (base_addr, code_size, dwOld, &dwOld);

  TZF_FlushInstructionCache (base_addr, code_size);
}

LPVOID pLuaReturn = nullptr;

void
__declspec(naked)
TZF_LuaHook (void)
{
  char    *name;
  size_t  *pSz;
  uint8_t **pBuffer;

  __asm
  {
    pushad
    pushfd

    // save luaL_loadbuffer's stack frame because we need some stuff on it
    mov  ebx, ebp

    push ebp
    mov  ebp, esp
    sub  esp, __LOCAL_SIZE

    // compiler must've optimised away the calling convention here
    mov  name, eax

    mov  eax, ebx
    add  eax, 0xC
    mov  pSz, eax
    mov  eax, ebx
    add  eax, 0x8
    mov  pBuffer, eax
  }

#if 0
  dll_log->Log (L"[  60 FPS  ]Lua script loaded: \"%S\"", name);
#endif

  if (! strcmp (name, "MEP_100_130_010_PF_Script")) {
    dll_log->Log (L"[  60 FPS  ] * Replacing priest script...");

    *pSz     = lua_bytecode_priest_size;
    *pBuffer = lua_bytecode_priest;
  }

  __asm
  {
    mov esp, ebp
    pop ebp

    popfd
    popad

    // overwritten instructions from original function
    mov ecx, [ebp + 0x8]
    mov [esp + 0x4], ecx

    jmp pLuaReturn
  }
}

typedef BOOL (WINAPI *CreateTimerQueueTimer_pfn)
(
  _Out_    PHANDLE             phNewTimer,
  _In_opt_ HANDLE              TimerQueue,
  _In_     WAITORTIMERCALLBACK Callback,
  _In_opt_ PVOID               Parameter,
  _In_     DWORD               DueTime,
  _In_     DWORD               Period,
  _In_     ULONG               Flags
);

CreateTimerQueueTimer_pfn CreateTimerQueueTimer_Original = nullptr;

BOOL
WINAPI
CreateTimerQueueTimer_Override (
  _Out_    PHANDLE             phNewTimer,
  _In_opt_ HANDLE              TimerQueue,
  _In_     WAITORTIMERCALLBACK Callback,
  _In_opt_ PVOID               Parameter,
  _In_     DWORD               DueTime,
  _In_     DWORD               Period,
  _In_     ULONG               Flags
)
{
  // Fix compliance related issues present in both
  //   Tales of Symphonia and Zestiria
  if (Flags & 0x8) {
    Period = 0;
  }

  return CreateTimerQueueTimer_Original (phNewTimer, TimerQueue, Callback, Parameter, DueTime, Period, Flags);
}

void
tzf::FrameRateFix::Init (void)
{
  CommandProcessor* comm_proc = CommandProcessor::getInstance ();

  InitializeCriticalSectionAndSpinCount (&alter_speed_cs, 1000UL);

  target_fps = config.framerate.target;

  TZF_CreateDLLHook2 ( config.system.injector.c_str (), "SK_SetPresentParamsD3D9",
                       SK_SetPresentParamsD3D9_Detour, 
            (LPVOID *)&SK_SetPresentParamsD3D9_Original,
                      &pfnSK_SetPresentParamsD3D9 );

  bink_dll = LoadLibrary (L"bink2w32.dll");

  TZF_CreateDLLHook2 ( L"bink2w32.dll", "_BinkOpen@8",
                       BinkOpen_Detour, 
            (LPVOID *)&BinkOpen_Original,
                      &pfnBinkOpen );

  TZF_CreateDLLHook2 ( L"bink2w32.dll", "_BinkClose@4",
                       BinkClose_Detour, 
            (LPVOID *)&BinkClose_Original,
                      &pfnBinkClose );

  TZF_CreateDLLHook2 ( config.system.injector.c_str (), "QueryPerformanceCounter_Detour",
                       QueryPerformanceCounter_Detour, 
            (LPVOID *)&QueryPerformanceCounter_Original,
            (LPVOID *)&pfnQueryPerformanceCounter );

  TZF_CreateDLLHook2 ( L"kernel32.dll", "CreateTimerQueueTimer",
                       CreateTimerQueueTimer_Override,
            (LPVOID *)&CreateTimerQueueTimer_Original );

  if (true) {
    if (*((DWORD *)config.framerate.speedresetcode_addr) != 0x428CB08D) {
      uint8_t sig [] = { 0x8D, 0xB0, 0x8C, 0x42,
                         0x00, 0x00, 0x8D, 0xBB,
                         0x8C, 0x42, 0x00, 0x00,
                         0xB9, 0x0B, 0x00, 0x00 };
      uintptr_t addr = (uintptr_t)TZF_Scan (sig, 16);

      if (addr != NULL) {
        dll_log->Log (L"[FrameLimit] Scanned SpeedResetCode Address: %06Xh", addr);
        config.framerate.speedresetcode_addr = addr;
      }
      else {
        dll_log->Log (L"[FrameLimit]  >> ERROR: Unable to find SpeedResetCode memory!");
      }
    }

    if (*((DWORD *)config.framerate.speedresetcode3_addr) != 0x02) {
      uint8_t sig [] = { 0x0F, 0x95, 0xC0, 0x3A,
                         0xC3, 0x74, 0x17, 0xB8,
                         0x02, 0x00, 0x00, 0x00 };
      uintptr_t addr = (uintptr_t)TZF_Scan (sig, 12);

      if (addr != NULL && *((DWORD *)((uint8_t *)addr + 8)) == 0x2) {
        dll_log->Log (L"[FrameLimit] Scanned SpeedResetCode3 Address: %06Xh", addr + 8);
        config.framerate.speedresetcode3_addr = addr + 8;
      }
      else {
        dll_log->Log (L"[FrameLimit]  >> ERROR: Unable to find SpeedResetCode3 memory!");
      }
    }

    dll_log->LogEx (true, L"[FrameLimit]  * Installing variable rate simulation... ");

    DWORD dwOld;

    //
    // original code:
    //
    // >> lea esi, [eax+0000428C]
    // >> lea edi, [ebx+0000428C]
    // >> mov ecx, 11
    // rep movsd
    // 
    // we want to skip the first two dwords
    //
    VirtualProtect((LPVOID)config.framerate.speedresetcode_addr, 17, PAGE_EXECUTE_READWRITE, &dwOld);
               *((DWORD *)(config.framerate.speedresetcode_addr +  2)) += 8;
               *((DWORD *)(config.framerate.speedresetcode_addr +  8)) += 8;
               *((DWORD *)(config.framerate.speedresetcode_addr + 13)) -= 2;
    VirtualProtect((LPVOID)config.framerate.speedresetcode_addr, 17, dwOld, &dwOld);

    TZF_FlushInstructionCache ((LPCVOID)config.framerate.speedresetcode_addr, 17);

    uint8_t mask [] = { 0xff, 0xff, 0xff,            // cmp     [ebx+28h], eax
                        0,    0,                     // jz      short <...>
                        0xff, 0xff, 0xff,            // cmp     eax, 2
                        0,    0,                     // jl      short <...>
                        0xff, 0,    0,     0, 0,     // mov     <tickaddr>,  eax
                        0xff, 0,    0,     0, 0,     // mov     <tickaddr2>, eax
                        0xff, 0xff, 0xff             // mov     [ebx+28h],   eax
                      };

    uint8_t sig [] = { 0x39, 0x43, 0x28,             // cmp     [ebx+28h], eax
                       0x74, 0x12,                   // jz      short <...>
                       0x83, 0xF8, 0x02,             // cmp     eax, 2
                       0x7C, 0x0D,                   // jl      short <...>
                       0xA3, 0x64, 0xB4, 0x17, 0x02, // mov     <tickaddr>,  eax
                       0xA3, 0x68, 0xB4, 0x17, 0x02, // mov     <tickaddr2>, eax
                       0x89, 0x43, 0x28              // mov     [ebx+28h],   eax
                      };

    if (*((DWORD *)config.framerate.speedresetcode2_addr) != 0x0F8831274) {
      uintptr_t addr = (uintptr_t)TZF_Scan (sig, 23, mask);

      if (addr != NULL) {
        config.framerate.speedresetcode2_addr = addr + 3;

        dll_log->Log (L"[FrameLimit] Scanned SpeedResetCode2 Address: %06Xh", addr + 3);

        TICK_ADDR_BASE = *(DWORD *)((uint8_t *)(addr + 11));

        dll_log->Log (L"[FrameLimit]  >> TICK_ADDR_BASE: %06Xh", TICK_ADDR_BASE);
      }
      else {
        dll_log->Log (L"[FrameLimit]  >> ERROR: Unable to find SpeedResetCode2 memory!");
      }
    }

    //
    // original code:
    //
    // ...
    // cmp [ebx+28], eax
    // >> jz after_set
    // >> cmp eax, 2
    // >> jl after_set
    // mov 0217B3D4, eax
    // mov 0217B3D8, eax
    // mov [ebx+28], eax
    // after_set:
    // ...
    //
    // we just want this to be 1 always
    //
    // new code:
    // mov eax, 01
    // nop
    // nop
    //
    uint8_t new_code [7] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90 };

    TZF_InjectByteCode ( (LPVOID)config.framerate.speedresetcode2_addr,
                           new_code,
                             7,
                               PAGE_EXECUTE_READWRITE,
                                 old_speed_reset_code2 );

    variable_speed_installed = true;

    // mov eax, 02 to mov eax, 01
    char scale [32];
    sprintf ( scale,
                "TickScale %li",
                  CalcTickScale (1000.0f * (1.0f / target_fps)) );
    SK_GetCommandProcessor ()->ProcessCommandLine (scale);

    dll_log->LogEx ( false, L"Field=%lu FPS, Battle=%lu FPS (%s), Cutscene=%lu FPS\n",
                       target_fps,
                         config.framerate.battle_target,
                           config.framerate.battle_adaptive ? L"Adaptive" : L"Fixed",
                             config.framerate.cutscene_target );
  }

  variable_speed_installed = true;

  if (config.framerate.disable_limiter) {
    // Replace the original jump (jb) with an unconditional jump (jmp)
    uint8_t new_code [6] = { 0xE9, 0x8B, 0x00, 0x00, 0x00, 0x90 };

    if (*(DWORD *)config.framerate.limiter_branch_addr != 0x8A820F) {
      uint8_t sig [] = { 0x53,                           // push    ebx
                         0x56,                           // push    esi
                         0x57,                           // push    edi
                         0x0F, 0x82, 0x8A, 0x0, 0x0, 0x0 // jb      <dontcare>
                       };
      uintptr_t addr = (uintptr_t)TZF_Scan (sig, 9);

      if (addr != NULL) {
        config.framerate.limiter_branch_addr = addr + 3;

        dll_log->Log (L"[FrameLimit] Scanned Limiter Branch Address: %06Xh", addr + 3);
      }
      else {
        dll_log->Log (L"[FrameLimit]  >> ERROR: Unable to find LimiterBranchAddr memory!");
      }
    }

    TZF_InjectByteCode ( (LPVOID)config.framerate.limiter_branch_addr,
                           new_code,
                             6,
                               PAGE_EXECUTE_READWRITE );
  }

  if (config.lua.fix_priest) {
    uint8_t sig[14] = {  0x8B, 0x4D, 0x08,        // mov ecx, [ebp+08]
                         0x89, 0x4C, 0x24, 0x04,  // mov [esp+04], ecx
                         0x8B, 0x4D, 0x0C,        // mov ecx, [ebp+0C]
                         0x89, 0x4C, 0x24, 0x08   // mov [esp+08], ecx
                      };
    uint8_t new_code[7] = { 0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90 };

    void *addr = TZF_Scan (sig, 14);

    if (addr != NULL) {
      dll_log->Log (L"[  60 FPS  ] Scanned Lua Loader Address: %06Xh", addr);

      DWORD hookOffset = (PtrToUlong (&TZF_LuaHook) - (uintptr_t)addr - 5);
      memcpy (new_code + 1, &hookOffset, 4);
      TZF_InjectByteCode (addr, new_code, 7, PAGE_EXECUTE_READWRITE);
      pLuaReturn = (LPVOID)((uintptr_t)addr + 7);
    }
    else {
      dll_log->Log (L"[  60 FPS  ]  >> ERROR: Unable to find Lua loader address! Priest bug will occur.");
    }
  }

  SK_ICommandProcessor* pCmdProc =
    SK_GetCommandProcessor ();

  // Special K already has something named this ... get it out of there!
  pCmdProc->RemoveVariable ("TargetFPS");

  pCmdProc->AddVariable ("TargetFPS",      TZF_CreateVar (SK_IVariable::Int,      (int *)&config.framerate.target));
  pCmdProc->AddVariable ("BattleFPS",      TZF_CreateVar (SK_IVariable::Int,      (int *)&config.framerate.battle_target));
  pCmdProc->AddVariable ("BattleAdaptive", TZF_CreateVar (SK_IVariable::Boolean, (bool *)&config.framerate.battle_adaptive));
  pCmdProc->AddVariable ("CutsceneFPS",    TZF_CreateVar (SK_IVariable::Int,      (int *)&config.framerate.cutscene_target));

  // No matter which technique we use, these things need to be options
  pCmdProc->AddVariable ("MinimizeLatency",   TZF_CreateVar (SK_IVariable::Boolean, &config.framerate.minimize_latency));

  // Hook this no matter what, because it lowers the _REPORTED_ CPU usage,
  //   and some people would object if we suddenly changed this behavior :P
  TZF_CreateDLLHook2 ( config.system.injector.c_str (), "Sleep_Detour",
                       Sleep_Detour, 
            (LPVOID *)&Sleep_Original,
         (LPVOID *)&pfnSleep );

  TZF_EnableHook       (pfnSleep);

  pCmdProc->AddVariable ("AllowFakeSleep", TZF_CreateVar (SK_IVariable::Boolean, &config.framerate.allow_fake_sleep));
  pCmdProc->AddVariable ("YieldProcessor", TZF_CreateVar (SK_IVariable::Boolean, &config.framerate.yield_processor));

  pCmdProc->AddVariable ("AutoAdjust",     TZF_CreateVar (SK_IVariable::Boolean, &config.framerate.auto_adjust));
}

void
tzf::FrameRateFix::Shutdown (void)
{
  FreeLibrary (kernel32_dll);
  FreeLibrary (bink_dll);

  ////TZF_DisableHook (pfnSK_SetPresentParamsD3D9);

  if (variable_speed_installed) {
    DWORD dwOld;

    VirtualProtect ((LPVOID)config.framerate.speedresetcode_addr, 17, PAGE_EXECUTE_READWRITE, &dwOld);
                *((DWORD *)(config.framerate.speedresetcode_addr +  2)) -= 8;
                *((DWORD *)(config.framerate.speedresetcode_addr +  8)) -= 8;
                *((DWORD *)(config.framerate.speedresetcode_addr + 13)) += 2;
    VirtualProtect ((LPVOID)config.framerate.speedresetcode_addr, 17, dwOld, &dwOld);

    TZF_FlushInstructionCache ((LPCVOID)config.framerate.speedresetcode_addr, 17);

    TZF_InjectByteCode ( (LPVOID)config.framerate.speedresetcode2_addr,
                           old_speed_reset_code2,
                             7,
                               PAGE_EXECUTE_READWRITE );

    SK_GetCommandProcessor ()->ProcessCommandLine ("TickScale 2");

    variable_speed_installed = false;
  }

  ////TZF_DisableHook (pfnSleep);
}

static int fps_before   = 60;
bool       forced_30    = false;

void
tzf::FrameRateFix::Begin30FPSEvent (void)
{
  //EnterCriticalSection (&alter_speed_cs);

  if (variable_speed_installed && (! forced_30)) {
    forced_30    = true;
    fps_before   = target_fps;
    SetFPS (30);
  }

  //LeaveCriticalSection (&alter_speed_cs);
}

void
tzf::FrameRateFix::End30FPSEvent (void)
{
  //EnterCriticalSection (&alter_speed_cs);

  if (variable_speed_installed && (forced_30)) {
    forced_30  = false;
    SetFPS (fps_before);
  }

  //LeaveCriticalSection (&alter_speed_cs);
}

void
tzf::FrameRateFix::SetFPS (int fps)
{
  //EnterCriticalSection (&alter_speed_cs);

  if (variable_speed_installed && (target_fps != fps)) {
    target_fps = fps;
    char szRescale [32];
    sprintf (szRescale, "TickScale %li", CalcTickScale (1000.0f * (1.0f / fps)));
    SK_GetCommandProcessor ()->ProcessCommandLine (szRescale);
  }

  //LeaveCriticalSection (&alter_speed_cs);
}




bool use_accumulator = false;

tzf::FrameRateFix::CommandProcessor* tzf::FrameRateFix::CommandProcessor::pCommProc;

tzf::FrameRateFix::CommandProcessor::CommandProcessor (void)
{
  tick_scale_ = TZF_CreateVar (SK_IVariable::Int, &tick_scale, this);

  SK_ICommandProcessor* pCmdProc =
    SK_GetCommandProcessor ();

  pCmdProc->AddVariable ("TickScale", tick_scale_);

  pCmdProc->AddVariable ("UseAccumulator",   TZF_CreateVar (SK_IVariable::Boolean, &use_accumulator));
  pCmdProc->AddVariable ("MaxFrameLatency",  TZF_CreateVar (SK_IVariable::Int,     &max_latency));
  pCmdProc->AddVariable ("WaitForVBLANK",    TZF_CreateVar (SK_IVariable::Boolean, &wait_for_vblank));
  pCmdProc->AddVariable ("LimiterTolerance", TZF_CreateVar (SK_IVariable::Float,   &limiter_tolerance));
}

bool
tzf::FrameRateFix::CommandProcessor::OnVarChange (SK_IVariable* var, void* val)
{
  DWORD dwOld;

  if (var == tick_scale_) {
    DWORD original0 = *((DWORD *)(TICK_ADDR_BASE    ));
    DWORD original1 = *((DWORD *)(TICK_ADDR_BASE + 4));

    if (val != nullptr) {
      VirtualProtect ((LPVOID)TICK_ADDR_BASE, 8, PAGE_READWRITE, &dwOld);

      if (variable_speed_installed) {
        // Battle Tickrate
        *(DWORD *)(TICK_ADDR_BASE) = *(DWORD *)val;
      }

      if (variable_speed_installed) {
        // World Tickrate
        *(DWORD *)(TICK_ADDR_BASE + 4) = *(DWORD *)val;
      }
      *(DWORD *)val = *(DWORD *)(TICK_ADDR_BASE + 4);

      VirtualProtect ((LPVOID)TICK_ADDR_BASE, 8, dwOld, &dwOld);

      if (variable_speed_installed) {
        // mov eax, 02 to mov eax, <val>
        VirtualProtect ((LPVOID)config.framerate.speedresetcode3_addr, 4, PAGE_EXECUTE_READWRITE, &dwOld);
                      *(DWORD *)config.framerate.speedresetcode3_addr = *(DWORD *)val;
        VirtualProtect ((LPVOID)config.framerate.speedresetcode3_addr, 4, dwOld, &dwOld);

        uint8_t new_code [7] = { 0xB8, (uint8_t)*(DWORD *)val, 0x00, 0x00, 0x00, 0x90, 0x90 };

        TZF_InjectByteCode ( (LPVOID)config.framerate.speedresetcode2_addr,
                               new_code,
                                 7,
                                   PAGE_EXECUTE_READWRITE,
                                     old_speed_reset_code2 );
      }
      //InterlockedExchange ((DWORD *)val, *(DWORD *)config.framerate.speedresetcode3_addr);

      tick_scale      = *(int32_t *)val;
    }
  }

  return true;
}



long
tzf::FrameRateFix::CalcTickScale (double elapsed_ms)
{
  const double tick_ms  = (1.0 / 60.0) * 1000.0;
  const double inv_rate =  1.0 / target_fps;

  long scale = static_cast<long> (std::min (std::max (elapsed_ms / tick_ms, 1.0), 7.0));

  if (scale > 6)
    scale = static_cast<long> (std::max (inv_rate / (1.0 / 60.0), 1.0));

  return scale;
}


void
tzf::FrameRateFix::RenderTick (void)
{
  if (! forced_30) {
    //if (config.framerate.cutscene_target != config.framerate.target)
      if (game_state.inCutscene ())
        SetFPS (config.framerate.cutscene_target);

    //if (config.framerate.battle_target != config.framerate.target)
      else if (game_state.inBattle ())
        SetFPS (config.framerate.battle_target);

    else //if (! (game_state.inBattle () || game_state.inCutscene ()))
      SetFPS (config.framerate.target);
  }

  static LARGE_INTEGER last_time  = { 0 };
  static LARGE_INTEGER freq       = { 0 };

  LARGE_INTEGER time;

  QueryPerformanceFrequency (&freq);

  static uint32_t last_limit = target_fps;

  if (limiter == nullptr) {
    limiter    = new FramerateLimiter ();
    last_limit = 0;
  }

  if (last_limit != target_fps) {
    limiter->change_limit (target_fps);

    last_limit = target_fps;
  }

  // Skip the limiter while loading, it needlessly
  //   prolongs loading screens otherwise.
  if (! game_state.isLoading ())
    limiter->wait ();

  QueryPerformanceCounter_Original (&time);


  if (forced_30) {
    last_time.QuadPart = time.QuadPart;
    return;
  }


  double dt = ((double)(time.QuadPart - last_time.QuadPart) / (double)freq.QuadPart) / (1.0 / 60.0);


#ifdef ACCUMULATOR
  static double accum = 0.0;

  if (use_accumulator) {
    accum +=  dt - floor (dt);

    time.QuadPart +=
      (dt - floor (dt)) * (1.0 / 60.0) * freq.QuadPart;

    dt    -= (dt - floor (dt));

    if (accum >= 1.0) {
      time.QuadPart -=
          (accum - (accum - floor (accum))) * (1.0 / 60.0) * freq.QuadPart;
      dt    +=  accum - (accum - floor (accum));
      accum -=  accum - (accum - floor (accum));
    }
  } else {
    accum = 0.0;
  }
#endif

  long scale = CalcTickScale (dt * (1.0 / 60.0) * 1000.0);

  static bool last_frame_battle = false;

  if (scale != tick_scale) {
    if (config.framerate.auto_adjust || (config.framerate.battle_adaptive && game_state.inBattle ())) {
      if (config.framerate.battle_adaptive && game_state.inBattle ()) {
        last_frame_battle = true;

#if 0
        dll_log->Log ( L"[FrameLimit] ** Adjusting TickScale because of battle framerate change "
                       L"(Expected: ~%4.2f ms, Got: %4.2f ms)",
                         last_scale * 16.666667f, dt * (1.0 / 60.0) * 1000.0 );
#endif
      }

      SK_GetCommandProcessor ()->ProcessCommandFormatted ("TickScale %li", scale);
    }
  }

  if (last_frame_battle && (! game_state.inBattle ()))
    target_fps = 1; // Reset FPS and TickScale on the next frame.

  if (! game_state.inBattle ())
    last_frame_battle = false;

  last_time.QuadPart = time.QuadPart;
}

float
tzf::FrameRateFix::GetTargetFrametime (void)
{
  if (! variable_speed_installed)
    return 33.33333f;

  int32_t* pTickScale =
    (int32_t *)(/*TZF_GetBaseAddr () + */TICK_ADDR_BASE);

  return ( (float)(*pTickScale) * 16.6666666f );
}