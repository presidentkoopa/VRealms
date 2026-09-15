// RS FORK -- DATA VALIDATORS (src/gamedata/datavalidation.cpp).
//
// A mod's check of its own data lumps, run only by a compile check: `-norun -validatedata`. After the scripts compile and
// before any level exists, the engine makes one of every concrete DataValidator class (in name order), calls Validate(),
// and destroys it. Each DataValidation.Refuse prints "DATA REFUSED <where> -- <why>"; any refusal makes the run exit
// with code 1339 instead of 1337, so the check fails.
//
// WHAT A VALIDATOR MAY TOUCH: lumps (Wads), class defaults (GetDefaultByType), CVars and Console. No level, no actors, no
// players' pawns and no event handlers exist when it runs.
//
// WHY A MOD WANTS ONE: a reader that runs when a level loads never runs in a compile check, so data it will refuse passes
// the check and is found only in play. Run that same reader in Validate, and route its refusals through
// DataValidation.Refuse behind DataValidation.Running(), which is false everywhere but inside a validation run.
class DataValidator abstract
{
	virtual void Validate() {}
}

struct DataValidation native
{
	// One problem: counted and printed while a validation run is on; ignored otherwise.
	native static void Refuse(String where, String why);
	// True only while the engine is running the validators.
	native static bool Running();
}
