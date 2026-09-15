/*
** datavalidation.cpp
**
** RS FORK -- DATA VALIDATORS: a mod's check of its own data, run by the compile check.
**
** WHY THIS EXISTS. A -norun compile check proves a mod's scripts compile, and nothing about its DATA. A mod that reads
** its own lumps (weapon cards, effect profiles) does so when a level loads, and a -norun check exits long before any
** level -- so a lump the mod will refuse passes the check and is found only in play. This lets a mod run that same
** reader inside the check, and fail it.
**
** HOW. A mod ships a ZScript class derived from DataValidator (zscript/engine/datavalidator.zs) and overrides
** Validate(). With -norun -validatedata, D_DoomMain calls D_RunDataValidators at its early -norun exit: the scripts are
** compiled, class defaults exist, every lump is loaded, and no level, actor or event handler exists. Each concrete
** validator class is made once, validated and destroyed, in name order so two runs print alike. A validator calls
** DataValidation.Refuse(where, why) for each problem; any refusal makes the run exit GAMEEXIT_DATAREFUSED (1339)
** instead of 1337.
**
** FOUND BY BASE CLASS, NOT BY A LIST: shipping the class is the whole opt-in, and nothing here names a mod.
**
** INERT UNTIL ASKED. Without -validatedata nothing here runs, and DataValidation.Running() is false, so a reader that
** routes its refusals here behind that test costs one native call in play and does nothing. Refuse outside a run is
** ignored.
**
** A VM ABORT inside a validator is caught and counted as that validator's refusal, so one broken validator fails the
** check under its own name instead of stopping the run at an error dialog.
*/

#include <algorithm>
#include <cstring>

#include "datavalidation.h"
#include "dobject.h"
#include "dobjtype.h"
#include "printf.h"
#include "vm.h"

static bool DataValidationActive = false;
static int  DataValidationRefused = 0;

static void DataValidationRefuse(const char *where, const char *why)
{
	DataValidationRefused++;
	Printf("DATA REFUSED %s -- %s\n", where, why);
}

int D_RunDataValidators()
{
	PClass *base = PClass::FindClass("DataValidator");
	if (base == nullptr)
	{
		Printf("data validation: these scripts have no DataValidator class -- nothing checked\n");
		return 0;
	}

	TArray<PClass *> validators;
	for (PClass *cls : PClass::AllClasses)
	{
		if (cls != base && !cls->bAbstract && cls->IsDescendantOf(base)) validators.Push(cls);
	}
	std::sort(validators.Data(), validators.Data() + validators.Size(),
		[](PClass *a, PClass *b) { return strcmp(a->TypeName.GetChars(), b->TypeName.GetChars()) < 0; });

	// The base class declares Validate, so its index is always there.
	const unsigned vindex = GetVirtualIndex(base, "Validate");

	DataValidationActive = true;
	DataValidationRefused = 0;
	for (PClass *cls : validators)
	{
		const char *name = cls->TypeName.GetChars();
		DObject *validator = cls->CreateNew();
		if (validator == nullptr)
		{
			DataValidationRefuse(name, "the validator could not be created");
			continue;
		}
		VMFunction *func = cls->Virtuals.Size() > vindex ? cls->Virtuals[vindex] : nullptr;
		if (func != nullptr)
		{
			try
			{
				VMValue params[1] = { validator };
				VMCall(func, params, 1, nullptr, 0);
			}
			catch (CVMAbortException &err)
			{
				err.MaybePrintMessage();
				Printf("%s", CVMAbortException::stacktrace.GetChars());
				CVMAbortException::stacktrace = "";
				DataValidationRefuse(name, "its Validate() aborted in the VM (the message is above)");
			}
		}
		validator->Destroy();
	}
	DataValidationActive = false;

	Printf("data validation: %u validator(s), %d refused\n", validators.Size(), DataValidationRefused);
	return DataValidationRefused;
}

// DataValidation.Refuse(where, why): one problem, counted and printed -- only while a validation run is on.
DEFINE_ACTION_FUNCTION(_DataValidation, Refuse)
{
	PARAM_PROLOGUE;
	PARAM_STRING(where);
	PARAM_STRING(why);
	if (DataValidationActive) DataValidationRefuse(where.GetChars(), why.GetChars());
	return 0;
}

// DataValidation.Running(): true only while D_RunDataValidators is running the validators.
DEFINE_ACTION_FUNCTION(_DataValidation, Running)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(DataValidationActive);
}
