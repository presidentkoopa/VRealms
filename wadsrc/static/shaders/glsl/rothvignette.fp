/*
** rothvignette.fp
**
** VRealms: the comfort vignette while moving or turning, from Godot XR Tools' effects/vignette.gdshader (MIT,
** 6c6189b): black outside `uRadius` (1.0 fully open, 0.0 closed), ramping in over `uFade` toward the centre.
** Its inner ring is centred on the eye's projection centre; here, on each eye picture's centre.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

void main()
{
	vec3 colour = texture(InputTexture, TexCoord).rgb;
	float dist = length(TexCoord * 2.0 - 1.0);
	float a = clamp((dist - uRadius) / max(uFade, 0.0001), 0.0, 1.0);
	FragColor = vec4(mix(colour, vec3(0.0), a), 1.0);
}
