//
// The RAW map commands that reach the game core. See roth_itemcmds.h.
//

#include "roth_itemcmds.h"

namespace roth
{

int32_t ItemCmdTailConsume(ItemCmdState &st, ItemCmdRecord &rec, int32_t ret)
{
	if (rec.fireFlags & 0x10) rec.modifier |= 0x08;         // raw_commands.c:4639-4640
	st.pendingRecord = 0;                                   // :4641
	return ret;
}

int32_t ItemCmdTailIfNext(GameCore &core, ItemCmdState &st, ItemCmdHost &host)
{
	// raw_commands.c:4651-4658. The pending record runs only while
	// g_item_autoselected_flag is clear; set, the record stays latched and the
	// (non-zero) latch is the answer.
	uint32_t pending = st.pendingRecord;
	if (pending != 0 && !core.Items().AutoSelected())
	{
		ItemCmdRecord *p = host.PendingRecord(pending);
		if (p != nullptr)
		{
			RunCommandDbase100Record(core, st, *p);         // :4653; it may clear st.pendingRecord itself
			host.PendingRecordRan(pending);
		}
		pending = 0;
		st.pendingRecord = 0;                               // :4655
	}
	st.interrupt = 2;                                       // :4657
	return pending != 0 ? 1 : 0;
}

int32_t RunCommandDbase100Record(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec)
{
	if (!st.dialogueLatch)                                  // raw_commands.c:4701-4704
	{
		st.dialogueLatch = true;
		core.Interp().State().g_dbase100_queue_active = 1;
	}
	const uint32_t r = core.Interp().EvalDialogueRecordById(rec.key);    // :4705
	if (r != 0) return ItemCmdTailConsume(st, rec, -1);                   // :4706-4707
	return 0;
}

namespace
{

// rawcmd_route_35580, raw_commands.c:4743-4749.
int32_t Route(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec, ItemCmdHost &host, uint32_t found)
{
	const bool f1 = (rec.fireFlags & 1) != 0;
	if (found != 0) return f1 ? ItemCmdTailConsume(st, rec, 0) : ItemCmdTailIfNext(core, st, host);
	if (core.Items().AutoSelected()) { st.interrupt = 1; return 0; }     // :4747
	return f1 ? ItemCmdTailIfNext(core, st, host) : ItemCmdTailConsume(st, rec, -1);
}

} // namespace

int32_t CmdIfNotItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec, ItemCmdHost &host)
{
	const uint8_t f6 = rec.fireFlags;
	const int mode = ((f6 & 2) ? 1 : 0) + ((f6 & 0x20) ? 2 : 0);         // raw_commands.c:4776, 4785
	if (f6 & 8)
	{
		if (!(f6 & 4))                                       // :4763-4771, append to the list
		{
			if (st.listCount < 0x10) st.list[st.listCount++] = rec.key;
			return 0;
		}
		if (st.listCount == 0) return ItemCmdTailIfNext(core, st, host);  // :4774
		return Route(core, st, rec, host, (uint32_t)core.FindOrAutoselect(st.list, st.listCount, mode));
	}
	if (f6 & 4)                                              // :4780-4784, compare the last item
	{
		if ((uint16_t)core.Items().LastItem() == rec.key) return ItemCmdTailConsume(st, rec, 0);
		return ItemCmdTailIfNext(core, st, host);
	}
	return Route(core, st, rec, host, (uint32_t)core.Query(rec.key, mode));   // :4785-4788
}

int32_t CmdGiveItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec, const GiveContext *ctx)
{
	const int32_t r = core.Give(rec.key, ctx);               // raw_commands.c:4824-4825
	core.Items().SetLastItem(r);                             // :4826
	if (r != 0) return -1;
	st.interrupt = 2;                                        // :4828
	return 0;
}

int32_t CmdRemoveItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec)
{
	int32_t id = rec.key;                                    // raw_commands.c:4920
	if (id == 0) id = core.Items().LastItem();               // :4921
	core.Items().SetLastItem(id);                            // :4922
	if (core.Remove((uint16_t)id) != 0) return ItemCmdTailConsume(st, rec, -1);   // :4923-4929
	return 0;
}

int32_t CmdSetInventoryFilter(GameCore &core, ItemCmdRecord &rec)
{
	core.SetFilter((rec.fireFlags & 1) ? 1 : 0);             // raw_commands.c:4942-4943
	if (rec.fireFlags & 0x10) rec.modifier |= 0x08;          // :4944-4945
	return -1;
}

} // namespace roth
