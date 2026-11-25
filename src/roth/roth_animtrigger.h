#pragma once
// THE TEXTURE-ANIMATION TRIGGER's renderer side (0x25; roth_runtime.h RegisterAnimFrames). A drawn wall, flat or sprite
// says which engine texture it drew; the runtime, on a map with a 0x25 record, notes a watched picture coming to its
// last frame on screen (texture_anim_command_hook, raw_commands.c:1341-1399). Small on purpose: the renderer includes it.
namespace roth
{
extern int g_animTriggerWatching;
void AnimTriggerDrawnSlow(int textureIndex);
inline void AnimTriggerDrawn(int textureIndex) { if (g_animTriggerWatching) AnimTriggerDrawnSlow(textureIndex); }
}
