#include "world.h"
#include "colors.h"
#include "math.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline int mc_height(const Mc *mc)
{
  return mc->y_max - mc->y_min + 1;
}

static inline int block_index(const Mc *mc, int x, int y, int z)
{
  return ((y - mc->y_min) * mc->size_z + z) * mc->size_x + x;
}

static inline bool block_is_opaque(BlockType t)
{
  return t != BLOCK_AIR && t != BLOCK_GLASS && t != BLOCK_LEAVES;
}

static void grow_y(Mc *mc, int new_y)
{
  int new_y_min = (new_y < mc->y_min) ? new_y : mc->y_min;
  int new_y_max = (new_y > mc->y_max) ? new_y : mc->y_max;
  int new_height = new_y_max - new_y_min + 1;

  size_t new_count = (size_t)mc->size_x * (size_t)new_height * (size_t)mc->size_z;
  BlockType *new_blocks = malloc(new_count * sizeof(BlockType));
  if (!new_blocks)
  {
    return;
  }
  for (size_t i = 0; i < new_count; i++)
  {
    new_blocks[i] = BLOCK_AIR;
  }

  for (int y = mc->y_min; y <= mc->y_max; y++)
  {
    int dst_y = y - new_y_min;
    int src_y = y - mc->y_min;
    size_t src_offset = (size_t)src_y * (size_t)mc->size_z * (size_t)mc->size_x;
    size_t dst_offset = (size_t)dst_y * (size_t)mc->size_z * (size_t)mc->size_x;
    memcpy(new_blocks + dst_offset, mc->blocks + src_offset,
           (size_t)mc->size_z * (size_t)mc->size_x * sizeof(BlockType));
  }

  free(mc->blocks);
  mc->blocks = new_blocks;
  mc->y_min = new_y_min;
  mc->y_max = new_y_max;
  mc->mesh_dirty = true;
}

BlockType block_get(const Mc *mc, int x, int y, int z)
{
  if (x < 0 || x >= mc->size_x || y < mc->y_min || y > mc->y_max || z < 0 ||
      z >= mc->size_z)
  {
    return BLOCK_AIR;
  }
  return mc->blocks[block_index(mc, x, y, z)];
}

void block_set(Mc *mc, int x, int y, int z, BlockType t)
{
  if (x < 0 || x >= mc->size_x || z < 0 || z >= mc->size_z)
  {
    return;
  }
  if (y < mc->y_min || y > mc->y_max)
  {
    grow_y(mc, y);
  }
  if (y < mc->y_min || y > mc->y_max)
  {
    return;
  }
  mc->blocks[block_index(mc, x, y, z)] = t;
  mc->mesh_dirty = true;
}

static void add_face(Face *faces, int *count, Texture *tex, v3f p0, v3f p1,
                     v3f p2, v3f p3)
{
  faces[*count].v[0] = (Vertex3D){p0, {0.0f, 1.0f}};
  faces[*count].v[1] = (Vertex3D){p1, {1.0f, 1.0f}};
  faces[*count].v[2] = (Vertex3D){p2, {1.0f, 0.0f}};
  faces[*count].tex = tex;
  (*count)++;

  faces[*count].v[0] = (Vertex3D){p0, {0.0f, 1.0f}};
  faces[*count].v[1] = (Vertex3D){p2, {1.0f, 0.0f}};
  faces[*count].v[2] = (Vertex3D){p3, {0.0f, 0.0f}};
  faces[*count].tex = tex;
  (*count)++;
}

void rebuild_faces(Mc *mc)
{
  mc->face_count = 0;
  const int max_faces = mc->size_x * mc_height(mc) * mc->size_z * 12;
  if (!mc->faces || mc->face_cap < max_faces)
  {
    free(mc->faces);
    mc->faces = malloc((size_t)max_faces * sizeof(Face));
    mc->face_cap = max_faces;
  }

#define IS_OPAQUE(ix, iy, iz) block_is_opaque(block_get(mc, (ix), (iy), (iz)))

  int cam_x = (int)floorf(mc->camera.pos.x + (float)mc->size_x * 0.5f);
  int cam_z = (int)floorf(mc->camera.pos.z + (float)mc->size_z * 0.5f);
  int radius = mc->render_distance_chunks * CHUNK_SIZE;
  int x0i = cam_x - radius;
  int x1i = cam_x + radius;
  int z0i = cam_z - radius;
  int z1i = cam_z + radius;
  if (x0i < 0)
    x0i = 0;
  if (z0i < 0)
    z0i = 0;
  if (x1i >= mc->size_x)
    x1i = mc->size_x - 1;
  if (z1i >= mc->size_z)
    z1i = mc->size_z - 1;

  for (int x = x0i; x <= x1i; x++)
  {
    for (int z = z0i; z <= z1i; z++)
    {
      for (int y = mc->y_min; y <= mc->y_max; y++)
      {
        BlockType type = block_get(mc, x, y, z);
        if (type == BLOCK_AIR)
        {
          continue;
        }

        Texture *top_tex = NULL;
        Texture *side_tex = NULL;
        Texture *bottom_tex = NULL;
        switch (type)
        {
        case BLOCK_GRASS:
          top_tex = &mc->grass_top_tex;
          side_tex = &mc->grass_side_tex;
          bottom_tex = &mc->dirt_tex;
          break;
        case BLOCK_DIRT:
          top_tex = &mc->dirt_tex;
          side_tex = &mc->dirt_tex;
          bottom_tex = side_tex;
          break;
        case BLOCK_STONE:
          top_tex = &mc->stone_tex;
          side_tex = &mc->stone_tex;
          bottom_tex = side_tex;
          break;
        case BLOCK_OAK_LOG:
          top_tex = &mc->oak_log_top_tex;
          side_tex = &mc->oak_log_side_tex;
          bottom_tex = top_tex;
          break;
        case BLOCK_OAK_PLANKS:
          top_tex = &mc->oak_planks_tex;
          side_tex = &mc->oak_planks_tex;
          bottom_tex = side_tex;
          break;
        case BLOCK_COBBLESTONE:
          top_tex = &mc->cobblestone_tex;
          side_tex = &mc->cobblestone_tex;
          bottom_tex = side_tex;
          break;
        case BLOCK_LEAVES:
          top_tex = &mc->leaves_tex;
          side_tex = &mc->leaves_tex;
          bottom_tex = side_tex;
          break;
        case BLOCK_GLASS:
          top_tex = &mc->glass_tex;
          side_tex = &mc->glass_tex;
          bottom_tex = side_tex;
          break;
        default:
          top_tex = &mc->stone_tex;
          side_tex = &mc->stone_tex;
          bottom_tex = side_tex;
          break;
        }

        float bx = (float)x - (mc->size_x * 0.5f) + 0.5f;
        float by = -(float)y - 0.5f;
        float bz = (float)z - (mc->size_z * 0.5f) + 0.5f;

        float x0 = bx - 0.5f, x1 = bx + 0.5f;
        float y0 = by - 0.5f, y1 = by + 0.5f;
        float z0 = bz - 0.5f, z1 = bz + 0.5f;

        if (!IS_OPAQUE(x, y - 1, z))
        { // top (+y in world)
          Texture *tex = top_tex;
          add_face(mc->faces, &mc->face_count, tex,
                   (v3f){x0, y1, z1}, (v3f){x1, y1, z1}, (v3f){x1, y1, z0},
                   (v3f){x0, y1, z0});
        }
        if (!IS_OPAQUE(x, y + 1, z))
        { // bottom (-y in world)
          add_face(mc->faces, &mc->face_count, bottom_tex,
                   (v3f){x0, y0, z0}, (v3f){x1, y0, z0}, (v3f){x1, y0, z1},
                   (v3f){x0, y0, z1});
        }
        if (!IS_OPAQUE(x, y, z + 1))
        { // front (+z)
          add_face(mc->faces, &mc->face_count, side_tex,
                   (v3f){x0, y0, z1}, (v3f){x1, y0, z1}, (v3f){x1, y1, z1},
                   (v3f){x0, y1, z1});
        }
        if (!IS_OPAQUE(x, y, z - 1))
        { // back (-z)
          add_face(mc->faces, &mc->face_count, side_tex,
                   (v3f){x1, y0, z0}, (v3f){x0, y0, z0}, (v3f){x0, y1, z0},
                   (v3f){x1, y1, z0});
        }
        if (!IS_OPAQUE(x - 1, y, z))
        { // left (-x)
          add_face(mc->faces, &mc->face_count, side_tex,
                   (v3f){x0, y0, z0}, (v3f){x0, y0, z1}, (v3f){x0, y1, z1},
                   (v3f){x0, y1, z0});
        }
        if (!IS_OPAQUE(x + 1, y, z))
        { // right (+x)
          add_face(mc->faces, &mc->face_count, side_tex,
                   (v3f){x1, y0, z1}, (v3f){x1, y0, z0}, (v3f){x1, y1, z0},
                   (v3f){x1, y1, z1});
        }
      }
    }
  }
#undef IS_OPAQUE
  mc->mesh_dirty = false;
}

void resolve_collisions(Mc *mc)
{
  float pmin_x = mc->camera.pos.x - PLAYER_RADIUS;
  float pmax_x = mc->camera.pos.x + PLAYER_RADIUS;
  float pmin_y = mc->camera.pos.y - PLAYER_HEIGHT;
  float pmax_y = mc->camera.pos.y;
  float pmin_z = mc->camera.pos.z - PLAYER_RADIUS;
  float pmax_z = mc->camera.pos.z + PLAYER_RADIUS;

  float pcx = (pmin_x + pmax_x) * 0.5f;
  float pcy = (pmin_y + pmax_y) * 0.5f;
  float pcz = (pmin_z + pmax_z) * 0.5f;

  int ix_min = (int)floorf(pmin_x + (float)mc->size_x * 0.5f);
  int ix_max = (int)floorf(pmax_x + (float)mc->size_x * 0.5f);
  int iz_min = (int)floorf(pmin_z + (float)mc->size_z * 0.5f);
  int iz_max = (int)floorf(pmax_z + (float)mc->size_z * 0.5f);
  int iy_min = (int)floorf(-pmax_y);
  int iy_max = (int)floorf(-pmin_y);

  if (ix_min < 0)
    ix_min = 0;
  if (iy_min < mc->y_min)
    iy_min = mc->y_min;
  if (iz_min < 0)
    iz_min = 0;
  if (ix_max >= mc->size_x)
    ix_max = mc->size_x - 1;
  if (iy_max > mc->y_max)
    iy_max = mc->y_max;
  if (iz_max >= mc->size_z)
    iz_max = mc->size_z - 1;

  mc->grounded = false;

  for (int x = ix_min; x <= ix_max; x++)
  {
    for (int z = iz_min; z <= iz_max; z++)
    {
      for (int y = iy_min; y <= iy_max; y++)
      {
        if (block_get(mc, x, y, z) == BLOCK_AIR)
        {
          continue;
        }

        float bmin_x = (float)x - (float)mc->size_x * 0.5f;
        float bmax_x = bmin_x + 1.0f;
        float bmin_z = (float)z - (float)mc->size_z * 0.5f;
        float bmax_z = bmin_z + 1.0f;
        float bmin_y = -(float)(y + 1);
        float bmax_y = -(float)y;

        float ox = fminf(pmax_x, bmax_x) - fmaxf(pmin_x, bmin_x);
        float oy = fminf(pmax_y, bmax_y) - fmaxf(pmin_y, bmin_y);
        float oz = fminf(pmax_z, bmax_z) - fmaxf(pmin_z, bmin_z);

        if (ox > 0.0f && oy > 0.0f && oz > 0.0f)
        {
          if (ox <= oy && ox <= oz)
          {
            float dir = (pcx < (bmin_x + bmax_x) * 0.5f) ? -ox : ox;
            mc->camera.pos.x += dir;
            pmin_x += dir;
            pmax_x += dir;
            pcx += dir;
          }
          else if (oy <= ox && oy <= oz)
          {
            float dir = (pcy < (bmin_y + bmax_y) * 0.5f) ? -oy : oy;
            mc->camera.pos.y += dir;
            pmin_y += dir;
            pmax_y += dir;
            pcy += dir;
            mc->velocity.y = 0.0f;
            if (dir > 0.0f)
            {
              mc->grounded = true;
            }
          }
          else
          {
            float dir = (pcz < (bmin_z + bmax_z) * 0.5f) ? -oz : oz;
            mc->camera.pos.z += dir;
            pmin_z += dir;
            pmax_z += dir;
            pcz += dir;
          }
        }
      }
    }
  }
}

bool raycast_block(Mc *mc, v3f origin, v3f dir, float max_dist, int *hx,
                   int *hy, int *hz, v3f *hnormal)
{
  float gx = origin.x + (float)mc->size_x * 0.5f;
  float gy = -origin.y;
  float gz = origin.z + (float)mc->size_z * 0.5f;

  float gdx = dir.x;
  float gdy = -dir.y;
  float gdz = dir.z;

  int ix = (int)floorf(gx);
  int iy = (int)floorf(gy);
  int iz = (int)floorf(gz);

  int stepX = (gdx > 0.0f) ? 1 : -1;
  int stepY = (gdy > 0.0f) ? 1 : -1;
  int stepZ = (gdz > 0.0f) ? 1 : -1;

  float invX = (gdx != 0.0f) ? (1.0f / fabsf(gdx)) : INFINITY;
  float invY = (gdy != 0.0f) ? (1.0f / fabsf(gdy)) : INFINITY;
  float invZ = (gdz != 0.0f) ? (1.0f / fabsf(gdz)) : INFINITY;

  float tMaxX = (gdx != 0.0f)
                    ? ((stepX > 0 ? ((float)(ix + 1) - gx) : (gx - (float)ix)) *
                       invX)
                    : INFINITY;
  float tMaxY = (gdy != 0.0f)
                    ? ((stepY > 0 ? ((float)(iy + 1) - gy) : (gy - (float)iy)) *
                       invY)
                    : INFINITY;
  float tMaxZ = (gdz != 0.0f)
                    ? ((stepZ > 0 ? ((float)(iz + 1) - gz) : (gz - (float)iz)) *
                       invZ)
                    : INFINITY;

  v3f normal = {0};
  float t = 0.0f;
  while (t <= max_dist)
  {
    if (ix >= 0 && ix < mc->size_x && iy >= mc->y_min && iy <= mc->y_max &&
        iz >= 0 && iz < mc->size_z)
    {
      if (block_get(mc, ix, iy, iz) != BLOCK_AIR)
      {
        *hx = ix;
        *hy = iy;
        *hz = iz;
        *hnormal = normal;
        return true;
      }
    }

    if (tMaxX < tMaxY)
    {
      if (tMaxX < tMaxZ)
      {
        t = tMaxX;
        tMaxX += invX;
        ix += stepX;
        normal = (v3f){(float)-stepX, 0.0f, 0.0f};
      }
      else
      {
        t = tMaxZ;
        tMaxZ += invZ;
        iz += stepZ;
        normal = (v3f){0.0f, 0.0f, (float)-stepZ};
      }
    }
    else
    {
      if (tMaxY < tMaxZ)
      {
        t = tMaxY;
        tMaxY += invY;
        iy += stepY;
        normal = (v3f){0.0f, (float)-stepY, 0.0f};
      }
      else
      {
        t = tMaxZ;
        tMaxZ += invZ;
        iz += stepZ;
        normal = (v3f){0.0f, 0.0f, (float)-stepZ};
      }
    }
    if (t > max_dist)
    {
      break;
    }
  }
  return false;
}
