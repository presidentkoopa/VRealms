// Engine stub (see v_video.h): a one-lump file system holding a buffer.
#pragma once
#include <stdint.h>
#include <vector>
#include <string>

struct FileDataStub
{
	std::vector<uint8_t> b;
	const uint8_t *bytes() const { return b.data(); }
	size_t size() const { return b.size() - 1; }    // FileData adds a 0 byte after the data
};

struct FileSystemStub
{
	std::vector<uint8_t> lump;
	FileDataStub ReadFile(int) const { FileDataStub d; d.b = lump; d.b.push_back(0); return d; }
	const char *GetFileFullName(int) const { return "stub.kvx"; }
};
extern FileSystemStub fileSystem;
