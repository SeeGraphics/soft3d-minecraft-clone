#pragma once

#include "mc.h"
#include <stdbool.h>

BlockType block_get(const Mc *mc, int x, int y, int z);
void block_set(Mc *mc, int x, int y, int z, BlockType t);
void rebuild_faces(Mc *mc);
void resolve_collisions(Mc *mc);
bool raycast_block(Mc *mc, v3f origin, v3f dir, float max_dist, int *hx,
                   int *hy, int *hz, v3f *hnormal);
