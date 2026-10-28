// The mods and tweaks of the menu (render/settings.h), applied to the game's memory at each vertical
// blank. They change the game's data, not its code, through the data that docs/game.md names.
#pragma once

#include "render/settings.h"

namespace tweaks {

// IKARUGA_TWEAKS=lives,charge,bullets,start switches tweaks on for a run without the window's menu.
render::Tweaks from_environment();

void apply(const render::Tweaks& t);
// The tweaks of the last apply(), for the game functions that the port wraps.
const render::Tweaks& current();

// Moves player 1's ship, while the player plays it, by (dx, dy) in the 640x480 picture: the moved point's ray
// through the stage's camera meets the ship's plane. The game's bounds still hold.
void ship_by(float dx, float dy);

}  // namespace tweaks
