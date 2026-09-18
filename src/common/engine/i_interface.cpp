/*
** i_interface.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2019-2025 GZDoom Maintainers and Contributors
** Copyright 2020 Christoph Oelckers
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "i_interface.h"
#include "st_start.h"
#include "gamestate.h"
#include "startupinfo.h"
#include "c_cvars.h"
#include "gstrings.h"
#include "version.h"
#include "m_argv.h"
#include "m_random.h"

#ifdef HAS_UPDATER
#include "curl_loader.h"
#endif

static_assert(sizeof(void*) == 8,
	"Only LP64/LLP64 builds are officially supported. "
	"Please do not attempt to build for other platforms; "
	"even if the program succeeds in a MAP01 smoke test, "
	"there are e.g. known visual artifacts "
	"<https://forum.zdoom.org/viewtopic.php?f=7&t=75673> "
	"that lead to a bad user experience.");

// Some global engine variables taken out of the backend code.
FStartupScreen* StartWindow;
SystemCallbacks sysCallbacks;
FString endoomName;
bool batchrun;
float menuBlurAmount;

bool AppActive = true;
int chatmodeon;
gamestate_t 	gamestate = GS_STARTUP;
bool ToggleFullscreen;
int 			paused;
bool			pauseext;

FStartupInfo GameStartupInfo;

CVAR(Bool, vid_fps, false, 0)
CVAR(Bool, queryiwad, QUERYIWADDEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, saveargs, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, savenetfile, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, savenetargs, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultiwad, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultargs, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultnetiwad, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultnetargs, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetplayers, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnethostport, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetticdup, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetgamemode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, defaultnetaltdm, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultnetaddress, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetjoinport, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetpage, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnethostteam, 255, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, defaultnetjointeam, 255, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, defaultnetextratic, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, defaultnetsavefile, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, ui_colors, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, ui_color_mix, .35, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

EXTERN_CVAR(Bool, ui_generic)
EXTERN_CVAR(Int, vid_preferbackend)
EXTERN_CVAR(Bool, vid_fullscreen)
EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Bool, r_dynlights)
EXTERN_CVAR(Bool, gl_light_shadowmap)
EXTERN_CVAR(Int, ui_preferred_theme)

#ifdef HAS_UPDATER
EXTERN_CVAR(Int, updater_update_interval)
EXTERN_CVAR(Bool, updater_auto_updates)
EXTERN_CVAR(Bool, updater_check_updates)
#endif

CUSTOM_CVAR(String, language, "auto", CVAR_ARCHIVE | CVAR_NOINITCALL | CVAR_GLOBALCONFIG)
{
	GStrings.UpdateLanguage(self);
	UpdateGenericUI(ui_generic);
	if (sysCallbacks.LanguageChanged) sysCallbacks.LanguageChanged(self);
}

FARG(pride, "Launcher", "Show pride colors", "",
	 "Show pride colors in launcher.");

// Some of this info has to be passed and managed from the front end since it's game-engine specific.
FStartupSelectionInfo::FStartupSelectionInfo(const TArray<WadStuff>& wads, FArgs& args, int startFlags) : Wads(&wads), Args(&args), DefaultStartFlags(startFlags)
{
	DefaultQueryIWAD = queryiwad;
	DefaultLanguage = language;
	DefaultBackend = vid_preferbackend;
	DefaultFullscreen = vid_fullscreen;
	DefaultVsync = vid_vsync;
	DefaultDynLights = r_dynlights;
	DefaultShadowmaps = gl_light_shadowmap;
	DefaultPreferredTheme = ui_preferred_theme;

	if (defaultiwad[0] != '\0')
	{
		for (int i = 0; i < wads.SSize(); ++i)
		{
			if (!wads[i].Name.CompareNoCase(defaultiwad))
			{
				DefaultIWAD = i;
				break;
			}
		}
	}
	DefaultArgs = defaultargs;
	bSaveArgs = saveargs;

	if (defaultnetiwad[0] != '\0')
	{
		for (int i = 0; i < wads.SSize(); ++i)
		{
			if (!wads[i].Name.CompareNoCase(defaultnetiwad))
			{
				DefaultNetIWAD = i;
				break;
			}
		}
	}
	DefaultNetArgs = defaultnetargs;
	DefaultNetPage = defaultnetpage;
	DefaultNetSaveFile = defaultnetsavefile;
	bSaveNetFile = savenetfile;
	bSaveNetArgs = savenetargs;

	DefaultNetPlayers = defaultnetplayers;
	DefaultNetHostPort = defaultnethostport;
	DefaultNetTicDup = defaultnetticdup;
	DefaultNetGameMode = defaultnetgamemode;
	DefaultNetAltDM = defaultnetaltdm;
	DefaultNetHostTeam = defaultnethostteam;
	DefaultNetExtraTic = defaultnetextratic;

	DefaultNetAddress = defaultnetaddress;
	DefaultNetJoinPort = defaultnetjoinport;
	DefaultNetJoinTeam = defaultnetjointeam;

	prideColors = Args->CheckParm(FArg_pride)? "list": ui_colors;
	prideMix = ui_color_mix;

#ifdef HAS_UPDATER
	DefaultUpdateInterval = updater_update_interval;
	bAutoUpdate = updater_auto_updates;
	bCheckUpdate = updater_check_updates;
#endif

	// RS FORK -- everything above is what the launcher opens with; SaveInfo() compares against it (see Opened).
	Opened = std::make_shared<const FStartupSelectionInfo>(*this);
}

// Return whatever IWAD the user selected.
int FStartupSelectionInfo::SaveInfo()
{
	DefaultLanguage.StripLeftRight();

	DefaultArgs.StripLeftRight();

	DefaultNetArgs.StripLeftRight();
	AdditionalNetArgs.StripLeftRight();
	DefaultNetAddress.StripLeftRight();
	DefaultNetSaveFile.StripLeftRight();

	// RS FORK -- WRITE BACK ONLY WHAT THE LAUNCHER CHANGED.
	//
	// Every value below used to be written back unconditionally. That is harmless
	// only while the launcher's copy still equals the cvar, and it does not always:
	//  - defaultiwad / defaultnetiwad are stored as NAMES, but the launcher holds an
	//    INDEX into the IWADs it was given, 0 when the stored name is not among them.
	//    -iwad with -showlauncher (or a GAMEINFO IWAD with the query key held) gives
	//    it just the one IWAD, and Play wrote that name over both remembered IWADs --
	//    the multiplayer one without the Multiplayer tab ever being opened. With
	//    "don't ask again" and two or more IWADs this runs with no window at all: a
	//    remembered IWAD that is missing for one run (a drive not mounted) was
	//    replaced by the first IWAD found and never came back.
	//  - a language the settings page cannot show (settingspage.h, languagePicked).
	//  - a cvar changed by anything else while the launcher is open was reverted.
	//
	// So each value is compared with what the launcher opened with (Opened), trimmed
	// the same way, and written only where it differs. A value the launcher did not
	// change stays exactly as the ini had it; a changed one is written as before. An
	// empty remembered IWAD still learns the one played (nothing there to keep).
	// Cvar callbacks are not enabled yet at this point of startup (D_InitGame does
	// that), so a skipped identical write had no side effect to lose.
	const FStartupSelectionInfo &opened = *Opened;
	const auto changedString = [](const FString &now, const FString &was)
	{
		FString trimmed = was;
		trimmed.StripLeftRight();
		return now.Compare(trimmed) != 0;
	};

#ifdef HAS_UPDATER
	if(IsCurlLoaded())
	{
		if (DefaultUpdateInterval != opened.DefaultUpdateInterval)
			updater_update_interval = DefaultUpdateInterval;
		if (bAutoUpdate != opened.bAutoUpdate)
			updater_auto_updates = bAutoUpdate;
		if (bCheckUpdate != opened.bCheckUpdate)
			updater_check_updates = bCheckUpdate;
	}
#endif

	if (DefaultQueryIWAD != opened.DefaultQueryIWAD)
		queryiwad = DefaultQueryIWAD;
	if (changedString(DefaultLanguage, opened.DefaultLanguage))
		language = DefaultLanguage.GetChars();
	if (DefaultFullscreen != opened.DefaultFullscreen)
		vid_fullscreen = DefaultFullscreen;
	if (DefaultVsync != opened.DefaultVsync)
		vid_vsync = DefaultVsync;
	if (DefaultDynLights != opened.DefaultDynLights)
		r_dynlights = DefaultDynLights;
	if (DefaultShadowmaps != opened.DefaultShadowmaps)
		gl_light_shadowmap = DefaultShadowmaps;
	if (DefaultPreferredTheme != opened.DefaultPreferredTheme)
		ui_preferred_theme = DefaultPreferredTheme;
	if (DefaultBackend != opened.DefaultBackend && DefaultBackend != vid_preferbackend)
		vid_preferbackend = DefaultBackend;

	if (bSaveNetFile != opened.bSaveNetFile)
		savenetfile = bSaveNetFile;
	if (bSaveNetArgs != opened.bSaveNetArgs)
		savenetargs = bSaveNetArgs;

	if (DefaultNetIWAD != opened.DefaultNetIWAD || defaultnetiwad[0] == '\0')
		defaultnetiwad = (*Wads)[DefaultNetIWAD].Name.GetChars();
	if (DefaultNetPage != opened.DefaultNetPage)
		defaultnetpage = DefaultNetPage;
	// The save file and the parameters are stored only while their "remember" box is ticked.
	if (bSaveNetFile != opened.bSaveNetFile || changedString(DefaultNetSaveFile, opened.DefaultNetSaveFile))
		defaultnetsavefile = bSaveNetFile ? DefaultNetSaveFile.GetChars() : "";
	if (bSaveNetArgs != opened.bSaveNetArgs || changedString(DefaultNetArgs, opened.DefaultNetArgs))
		defaultnetargs = bSaveNetArgs ? DefaultNetArgs.GetChars() : "";

	if (DefaultNetPlayers != opened.DefaultNetPlayers)
		defaultnetplayers = DefaultNetPlayers;
	if (DefaultNetHostPort != opened.DefaultNetHostPort)
		defaultnethostport = DefaultNetHostPort;
	if (DefaultNetTicDup != opened.DefaultNetTicDup)
		defaultnetticdup = DefaultNetTicDup;
	if (DefaultNetGameMode != opened.DefaultNetGameMode)
		defaultnetgamemode = DefaultNetGameMode;
	if (DefaultNetAltDM != opened.DefaultNetAltDM)
		defaultnetaltdm = DefaultNetAltDM;
	if (DefaultNetHostTeam != opened.DefaultNetHostTeam)
		defaultnethostteam = DefaultNetHostTeam;
	if (DefaultNetExtraTic != opened.DefaultNetExtraTic)
		defaultnetextratic = DefaultNetExtraTic;

	if (changedString(DefaultNetAddress, opened.DefaultNetAddress))
		defaultnetaddress = DefaultNetAddress.GetChars();
	if (DefaultNetJoinPort != opened.DefaultNetJoinPort)
		defaultnetjoinport = DefaultNetJoinPort;
	if (DefaultNetJoinTeam != opened.DefaultNetJoinTeam)
		defaultnetjointeam = DefaultNetJoinTeam;

	if (DefaultIWAD != opened.DefaultIWAD || defaultiwad[0] == '\0')
		defaultiwad = (*Wads)[DefaultIWAD].Name.GetChars();
	if (bSaveArgs != opened.bSaveArgs)
		saveargs = bSaveArgs;
	if (bSaveArgs != opened.bSaveArgs || changedString(DefaultArgs, opened.DefaultArgs))
		defaultargs = bSaveArgs ? DefaultArgs.GetChars() : "";

	if (bNetStart)
	{
		if (!DefaultNetArgs.IsEmpty())
			Args->AppendRawArgsString(DefaultNetArgs);
		if (!AdditionalNetArgs.IsEmpty())
			Args->AppendRawArgsString(AdditionalNetArgs);

		return DefaultNetIWAD;
	}

	if (!DefaultArgs.IsEmpty())
		Args->AppendRawArgsString(DefaultArgs);

	return DefaultIWAD;
}

FString GameUUID;
static FRandom pr_uuid("GameUUID");

FString GenerateUUID()
{
	FString uuid;
	uuid.AppendFormat("%08X-%04X-4%03X-9%03X-%08X%04X", pr_uuid.GenRand32(), pr_uuid(UINT16_MAX), pr_uuid(4095), pr_uuid(4095), pr_uuid.GenRand32(), pr_uuid(UINT16_MAX));
	return uuid;
}
