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

#include <Windows.h>

#include "steam.h"
#include "log.h"
#include "config.h"
#include "hook.h"

#include <stdint.h>

HMODULE tzf::SteamFix::steam_dll = 0;

typedef uint32_t AppId_t;

class ISteamVideo
{
public:

  // Get a URL suitable for streaming the given Video app ID's video
  virtual void GetVideoURL( AppId_t unVideoAppID ) = 0;

  // returns true if user is uploading a live broadcast
  virtual bool IsBroadcasting( int *pnNumViewers ) = 0;
};

enum { k_iClientVideoCallbacks = 4600 };

#define k_iClientVideo_BroadcastUploadStart (k_iClientVideoCallbacks + 4)

class SteamVideoFake : public ISteamVideo {
public:
  SteamVideoFake (ISteamVideo* real) : real_ (real) { };

  // Get a URL suitable for streaming the given Video app ID's video
  virtual void GetVideoURL( AppId_t unVideoAppID ) {
    return real_->GetVideoURL (unVideoAppID);
  }

  // returns true if user is uploading a live broadcast
  virtual bool IsBroadcasting( int *pnNumViewers ) {
    *pnNumViewers = 0;
    return false;
  }

protected:
private:
  ISteamVideo* real_;
};

SteamVideoFake* faker = nullptr;

#define S_CALLTYPE __cdecl

typedef ISteamVideo* (S_CALLTYPE *SteamVideo_t)(void);

LPVOID       SteamVideo          = nullptr;
SteamVideo_t SteamVideo_Original = nullptr;

ISteamVideo*
S_CALLTYPE
SteamVideo_Detour (void)
{
  ISteamVideo* pVideo = SteamVideo_Original ();

  // Recent changes to SteamAPI store a non-zero value in the int* passed to
  //   IsBroadcasting (...) even if the return is false.  (4/17/17)
  //
  if (pVideo != nullptr /*&& pVideo->IsBroadcasting (&x) == true*/) {
    if (faker != nullptr) {
      delete faker;
    }

    faker = new SteamVideoFake (pVideo);

    return faker;
  }

  return pVideo;
}


void
tzf::SteamFix::Init (void)
{
  CommandProcessor* comm_proc = CommandProcessor::getInstance ();

  if (! config.steam.allow_broadcasts)
    return;

  steam_dll = LoadLibrary (L"steam_api.dll");

  TZF_CreateDLLHook ( L"steam_api.dll", "SteamVideo",
                      SteamVideo_Detour,
           (LPVOID *)&SteamVideo_Original,
           (LPVOID *)&SteamVideo );

  TZF_EnableHook (SteamVideo);
}

void
tzf::SteamFix::Shutdown (void)
{
  if (! config.steam.allow_broadcasts)
    return;

  //TZF_RemoveHook (SteamVideo);

  FreeLibrary (steam_dll);
}

void
tzf::SteamFix::SetOverlayState (bool active)
{
  // Avoid duplicating a SK feature
  static HMODULE hD3D9 = GetModuleHandle (config.system.injector.c_str ());

  typedef void (__stdcall *SK_SteamAPI_SetOverlayState_pfn)(bool);
  static SK_SteamAPI_SetOverlayState_pfn SK_SteamAPI_SetOverlayState =
    (SK_SteamAPI_SetOverlayState_pfn)GetProcAddress ( hD3D9,
                                                       "SK_SteamAPI_SetOverlayState" );

  SK_SteamAPI_SetOverlayState (active);
}




tzf::SteamFix::CommandProcessor::CommandProcessor (void)
{
  SK_ICommandProcessor& command =
    *SK_GetCommandProcessor ();

  allow_broadcasts_ = TZF_CreateVar (SK_IVariable::Boolean, &config.steam.allow_broadcasts, this);

  command.AddVariable ("AllowBroadcasts", allow_broadcasts_);
}

bool
tzf::SteamFix::CommandProcessor::OnVarChange (SK_IVariable* var, void* val)
{
  if (var == allow_broadcasts_) {
    if (*(bool *)val == true) {
      config.steam.allow_broadcasts = true;
      SteamFix::Init ();
    }

    if (*(bool *)val == false) {
      SteamFix::Shutdown ();
      config.steam.allow_broadcasts = false;
    }
  }
  return true;
}


tzf::SteamFix::CommandProcessor* tzf::SteamFix::CommandProcessor::pCommProc;