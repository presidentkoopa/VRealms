/*
** rothlowhealth.fp
**
** VRealms: the low-health cue's red screen edge (docs/VR_CHANGES_LEDGER.md
** row 34). Runs over each eye's picture; the edge darkens toward red from a
** little inside the picture's border to its corners. uStrength 0..1.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

void main()
{
	vec3 colour = texture(InputTexture, TexCoord).rgb;
	vec2 d = TexCoord * 2.0 - 1.0;
	float edge = smoothstep(0.62, 1.30, length(d));
	vec3 red = vec3(0.45, 0.0, 0.0);
	FragColor = vec4(mix(colour, red, clamp(edge * uStrength, 0.0, 1.0)), 1.0);
}
