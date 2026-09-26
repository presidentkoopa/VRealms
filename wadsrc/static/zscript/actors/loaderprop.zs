//
// A thing a native map loader places, which is only ever drawn.
//
// A loader that builds a level from a format Doom knows nothing about has no
// DECORATE, no MODELDEF and no sprite lumps to declare actors with -- it builds
// its artwork in memory while the level loads, from data that only exists at
// that moment. It still needs an actor class to hang a sprite or a model on, so
// here is one that carries no behaviour at all: the loader spawns it and then
// sets its sprite (or its model binding), scale, angle, render flags and render
// radius itself.
//
// Named for what it does, not for who asked. Any loader that needs "place a
// decoration I will describe entirely from C++" can use it, and a second caller
// needs no change here.
//
// The spawn state has tics -1, so the actor never changes state again and never
// overwrites the sprite the loader set on it.
//
class LoaderProp : Actor
{
	Default
	{
		// Positions come from the source map and are absolute. Gravity and floor
		// clipping would move the thing away from where the format said it was,
		// so they do not apply; a loader that wants a prop to fall or to block
		// clears these itself after spawning it.
		+NOGRAVITY;
		+NOTELEPORT;
		+DONTSPLASH;
		+NOTONAUTOMAP;
		+SYNCHRONIZED;
		+NODAMAGE;
		// Radius and Height are left at Actor's defaults deliberately: radius
		// doubles as the render radius, which decides whether the prop is drawn
		// when a neighbouring sector is the visible one. The loader raises
		// renderradius for anything bigger than that.
	}

	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}
}
