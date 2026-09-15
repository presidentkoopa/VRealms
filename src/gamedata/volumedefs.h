/*
** volumedefs.h
**
** [EMISSIVEVOLUMES] Emissive volume definitions: the VOLUMEDEFS reader and its table.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2j; "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md". What an emissive volume
** looks like -- its shape, life, heat, flicker, class, motion and light -- is written once in a VOLUMEDEFS lump (any mod may
** ship any number, read by the shared definitions reader, defblocks.h) and held here. The keys are in volumedefs.cpp's
** header comment; EmissiveVolumeCore::Definition (hw_emissivevolumecore.h) is what they become.
**
** NETPLAY: presentation only. Script addresses a definition by a HANDLE worked out from the name's text alone
** (EmissiveVolumeDefinitionHandle), the same number on every machine whatever that machine's lumps did. Only the renderer
** and the presentation query (EmissiveVolumeDefinitionEnabled) ask the table what a handle stands for.
**
*/

#pragma once

#include "hw_emissivevolumecore.h"

// Reads every VOLUMEDEFS lump (at start-up, beside PARTICLEDEFS). A later definition of the same name replaces an earlier one;
// a bad block is refused with one console line and nothing stops the game.
void LoadEmissiveVolumeDefinitions();

// The handle of a definition name: EmissiveVolumeCore::HandleFor -- 31 bits of the lower-case name's FNV-1a, never 0.
int EmissiveVolumeDefinitionHandle(const char *name);

// The definition a handle stands for on this machine, or null. `report`: print one console line the first time an unknown
// handle is seen (at most 64 of them).
const EmissiveVolumeCore::Definition *FindEmissiveVolumeDefinition(int handle, bool report);

// How many definitions are loaded (the `emissivevolumes` command lists them).
int EmissiveVolumeDefinitionCount();
