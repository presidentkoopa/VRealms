/*
** datavalidation.h
**
** RS FORK -- DATA VALIDATORS: mods' checks of their own data, run by a `-norun -validatedata` compile check.
** See datavalidation.cpp.
*/

#pragma once

// Makes one of every concrete ZScript DataValidator class, in name order, calls its Validate(), and destroys it.
// Prints a "DATA REFUSED <where> -- <why>" line per refused problem and one summary line. Returns how many problems
// were refused: 0 means the data passed. Called only from D_DoomMain's early -norun exit, with -validatedata.
int D_RunDataValidators();
