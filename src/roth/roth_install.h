#pragma once
//
// Locating the player's Realms of the Haunting installation, and working out
// which artwork pack a map uses.
//
// The engine reads the player's own game files directly. Nothing derived from
// them is ever generated, cached or shipped -- that is the whole point of
// reading the install rather than converting it in advance.
//
// The install path comes from the `roth_path` cvar (or -rothpath). Everything
// else is read from the game's own ROTH.RES manifest.
//

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

namespace roth
{

class Install
{
public:
	// Point at the folder containing ROTH.RES and M\. Returns false and sets
	// Error() when that is not a usable install.
	bool Open(const char *rothDir);

	bool IsOpen() const { return mOpen; }
	const std::string &Error() const { return mError; }
	const std::string &Path() const { return mPath; }

	// True when the manifest lists this map, i.e. `map STUDY1` should load from
	// the install rather than from a wad.
	bool HasMap(const char *name) const;

	// Absolute path to a map's .RAW, or empty when it isn't listed.
	std::string MapFile(const char *name) const;

	// The artwork pack a map uses, and the shared sprite pack used by all maps.
	std::string PackFor(const char *name) const;
	const std::string &SharedPack() const { return mSharedPack; }
	std::string PackFile(const char *pack) const;

	std::vector<std::string> MapNames() const;

	// Whole-file read; empty on failure.
	static std::vector<uint8_t> ReadWholeFile(const std::string &path);

private:
	bool mOpen = false;
	std::string mPath;
	std::string mError;
	std::string mSharedPack = "ADEMO";
	std::map<std::string, std::string> mMapToPack;   // upper-case names
};

// The one install the engine is using, if any.
Install &TheInstall();

} // namespace roth
