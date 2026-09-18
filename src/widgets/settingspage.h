/*
** settingspage.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2024 Magnus Norddahl
** Copyright 2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <zwidget/core/widget.h>

#define RENDER_BACKENDS

class LauncherWindow;
class TextLabel;
class CheckboxLabel;
class ListView;
class Dropdown;
struct FStartupSelectionInfo;

class SettingsPage : public Widget
{
public:
	SettingsPage(LauncherWindow* launcher, const FStartupSelectionInfo& info);
	void UpdateLanguage();
	void UpdateUpdaterValues(bool autoUpdate, bool check, int interval);
	void SetValues(FStartupSelectionInfo& info) const;

private:
	void OnLanguageChanged(int i);
	void OnGeometryChanged() override;

	LauncherWindow* Launcher = nullptr;

	TextLabel* LangLabel = nullptr;
	TextLabel* GeneralLabel = nullptr;
	TextLabel* ExtrasLabel = nullptr;
	TextLabel* LoadLabel = nullptr;
	TextLabel* ThemeLabel = nullptr;
	CheckboxLabel* FullscreenCheckbox = nullptr;
	CheckboxLabel* VsyncCheckbox = nullptr;
	CheckboxLabel* DisableAutoloadCheckbox = nullptr;
	CheckboxLabel* DontAskAgainCheckbox = nullptr;
	CheckboxLabel* LightsCheckbox = nullptr;
	CheckboxLabel* BrightmapsCheckbox = nullptr;
	CheckboxLabel* WidescreenCheckbox = nullptr;
	CheckboxLabel* SupportWadsCheckbox = nullptr;
	CheckboxLabel* DynLightsCheckbox = nullptr;
	CheckboxLabel* ShadowmapCheckbox = nullptr;
	Dropdown* ThemeDropdown = nullptr;
#ifdef RENDER_BACKENDS
	TextLabel* BackendLabel = nullptr;
	CheckboxLabel* VulkanCheckbox = nullptr;
	CheckboxLabel* OpenGLCheckbox = nullptr;
	CheckboxLabel* GLESCheckbox = nullptr;
#endif
	ListView* LangList = nullptr;
	Dropdown* LoadList = nullptr;
#ifdef HAS_UPDATER
	TextLabel* UpdaterSettingsLabel = nullptr;
	TextLabel* UpdaterIntervalLabel = nullptr;
	Dropdown* UpdaterSettingsDropdown = nullptr;
	Dropdown* UpdaterIntervalDropdown = nullptr;
#endif

	TArray<std::pair<FString, FString>> languages;
	bool hideLanguage = false;
	// RS FORK -- set once the user picks a row in the language list. OnLanguageChanged is
	// called only for a pick: the constructor selects the matching row before it attaches
	// the handler. SetValues reports a language only then. A language the list does not
	// offer (a mod's own code, set from the console) matches no row, so row 0 used to be
	// reported and Play wrote "auto" over it; an empty list was indexed out of range.
	bool languagePicked = false;

	int ExtraWadFlags = 0;
};
