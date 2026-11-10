//
// Where DBASE100.DAT meets the install: find it, read it, load it, report it.
// See roth_dbase100.h. Uses only roth::Install and roth::Log, both of which
// are engine-free, so this also builds in tools/dbase100_test.
//

#include "roth_dbase100.h"
#include "roth_install.h"
#include "roth_log.h"

namespace roth
{

bool LoadDbase100FromInstall(const Install &install, Dbase100 &db, Log *log)
{
	if (log) log->Section("DBASE100");
	if (!install.IsOpen())
	{
		if (log) log->Warn("DBASE100: no install open");
		return false;
	}

	const std::string path = install.DataFile("DBASE100.DAT");
	if (path.empty())
	{
		if (log) log->Warn("DBASE100: DBASE100.DAT not found in %s", install.Path().c_str());
		return false;
	}

	std::vector<uint8_t> bytes = Install::ReadWholeFile(path);
	if (bytes.empty())
	{
		if (log) log->Warn("DBASE100: could not read %s", path.c_str());
		return false;
	}
	const size_t size = bytes.size();

	if (!db.Load(std::move(bytes)))
	{
		if (log) log->Warn("DBASE100: %s: %s", path.c_str(), db.Error().c_str());
		return false;
	}

	if (log)
	{
		const DbHeader &h = db.Header();
		log->Line("file        %s (%u bytes)", path.c_str(), (unsigned)size);
		log->Line("inventory   %u   actions %u   cutscenes %u   interface %u",
			(unsigned)h.inventoryCount, (unsigned)h.actionCount,
			(unsigned)h.cutsceneCount, (unsigned)h.interfaceCount);
		for (const auto &n : db.Notes()) log->Line("  %s", n.c_str());
		// Every record-level problem goes through Count, so it appears in the
		// log's summary of unhandled things and cannot be missed.
		for (const auto &kv : db.Anomalies())
			log->Count((std::string("dbase100: ") + kv.first).c_str(), kv.second);
		if (db.AnomalyTotal() > 0)
			log->Warn("DBASE100: %d record-level problems, see the counts", db.AnomalyTotal());
	}
	return true;
}

Dbase100 &TheDbase100()
{
	static Dbase100 instance;
	return instance;
}

} // namespace roth
