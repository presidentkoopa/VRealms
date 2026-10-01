//
// Locating the player's Realms of the Haunting install and reading its manifest.
//

#include "roth_install.h"

#include <stdio.h>
#include <string.h>

namespace roth
{

namespace
{

std::string ToUpper(std::string s)
{
	for (auto &c : s) c = (char)toupper((unsigned char)c);
	return s;
}

// ROTH.RES writes paths in DOS style ("m\study1"); we want just the stem.
std::string Stem(std::string s)
{
	size_t slash = s.find_last_of("\\/");
	if (slash != std::string::npos) s = s.substr(slash + 1);
	size_t dot = s.find_last_of('.');
	if (dot != std::string::npos) s = s.substr(0, dot);
	return ToUpper(s);
}

std::string Trim(std::string s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

} // namespace

std::vector<uint8_t> Install::ReadWholeFile(const std::string &path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0)
	{
		out.resize((size_t)n);
		if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
	}
	fclose(f);
	return out;
}

bool Install::Open(const char *rothDir)
{
	mOpen = false;
	mError.clear();
	mMapToPack.clear();
	if (!rothDir || !*rothDir)
	{
		mError = "no path set";
		return false;
	}

	mPath = rothDir;
	while (!mPath.empty() && (mPath.back() == '\\' || mPath.back() == '/'))
		mPath.pop_back();

	auto bytes = ReadWholeFile(mPath + "/ROTH.RES");
	if (bytes.empty())
	{
		mError = "ROTH.RES not found in " + mPath;
		return false;
	}

	// The manifest is plain text: some key=value lines, then a maps block of
	// "<map> <artwork pack>" pairs.
	std::string text((const char *)bytes.data(), bytes.size());
	size_t pos = 0;
	bool inMaps = false;
	while (pos < text.size())
	{
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos) eol = text.size();
		std::string line = Trim(text.substr(pos, eol - pos));
		pos = eol + 1;
		if (line.empty()) continue;

		if (!inMaps)
		{
			if (line.rfind("maps", 0) == 0) { inMaps = true; continue; }
			size_t eq = line.find('=');
			if (eq != std::string::npos)
			{
				std::string key = Trim(line.substr(0, eq));
				std::string val = Trim(line.substr(eq + 1));
				// das2 names the sprite pack every map shares.
				if (key == "das2" && !val.empty()) mSharedPack = Stem(val);
			}
			continue;
		}

		if (line[0] == '}') { inMaps = false; continue; }
		size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos) continue;
		std::string mapName = Stem(line.substr(0, sp));
		std::string pack = Stem(Trim(line.substr(sp + 1)));
		if (!mapName.empty() && !pack.empty()) mMapToPack[mapName] = pack;
	}

	if (mMapToPack.empty())
	{
		mError = "ROTH.RES lists no maps";
		return false;
	}

	mOpen = true;
	return true;
}

bool Install::HasMap(const char *name) const
{
	if (!mOpen || !name) return false;
	return mMapToPack.find(ToUpper(name)) != mMapToPack.end();
}

std::string Install::MapFile(const char *name) const
{
	if (!HasMap(name)) return "";
	return mPath + "/M/" + ToUpper(name) + ".RAW";
}

std::string Install::PackFor(const char *name) const
{
	if (!name) return "";
	auto it = mMapToPack.find(ToUpper(name));
	return it == mMapToPack.end() ? std::string() : it->second;
}

std::string Install::PackFile(const char *pack) const
{
	if (!mOpen || !pack || !*pack) return "";
	return mPath + "/M/" + ToUpper(pack) + ".DAS";
}

std::vector<std::string> Install::MapNames() const
{
	std::vector<std::string> out;
	out.reserve(mMapToPack.size());
	for (auto &kv : mMapToPack) out.push_back(kv.first);
	return out;
}

Install &TheInstall()
{
	static Install instance;
	return instance;
}

} // namespace roth
