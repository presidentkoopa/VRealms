/*
** rothlowhealthfixed.fp
**
** VRealms TEST ONLY (roth_lowedge_test 3): rothlowhealth.fp's edge at full
** strength with NO uniform, so a capture can tell a pass that does not run
** from a uniform that does not arrive.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

void main()
{
	vec3 colour = texture(InputTexture, TexCoord).rgb;
	vec2 d = TexCoord * 2.0 - 1.0;
	float edge = smoothstep(0.62, 1.30, length(d));
	FragColor = vec4(mix(colour, vec3(0.45, 0.0, 0.0), edge), 1.0);
}
