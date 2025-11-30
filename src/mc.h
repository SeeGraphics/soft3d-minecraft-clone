#pragma once

#include "types.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#define GRAVITY 9.81
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.6f
#define JUMP_VELOCITY 5.0f
#define WALK_SPEED 4.0f
#define CHUNK_SIZE 16

typedef struct
{
  u32 window_w;
  u32 window_h;
  u32 render_w;
  u32 render_h;
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Event event;
  SDL_Texture *texture;
  u32 *buffer;
  float *depth;
  u32 pitch;
  bool mouse_grabbed;
} Game;

typedef struct
{
  v3f pos;
  float yaw;
  float pitch;
} Camera;

typedef enum
{
  BLOCK_AIR = 0,
  BLOCK_GRASS,
  BLOCK_DIRT,
  BLOCK_STONE,
  BLOCK_OAK_LOG,
  BLOCK_OAK_PLANKS,
  BLOCK_COBBLESTONE,
  BLOCK_LEAVES,
  BLOCK_GLASS,
} BlockType;

typedef struct
{
  Vertex3D v[3];
  Texture *tex;
} Face;

typedef struct
{
  Game game;
  Camera camera;
  Texture dirt_tex;
  Texture stone_tex;
  Texture grass_side_tex;
  Texture grass_top_tex;
  Texture oak_log_side_tex;
  Texture oak_log_top_tex;
  Texture oak_planks_tex;
  Texture cobblestone_tex;
  Texture leaves_tex;
  Texture glass_tex;
  Texture sky_tex;
  BlockType selected_block;
  bool wireframe;
  bool noclip;
  float fps;
  int culled_faces_count;
  int rendered_faces_count;
  v3f velocity;
  bool grounded;
  Uint32 last_ticks;
  bool running;
  int render_scale;
  float near_plane;
  float far_plane;
  float mouse_sens;
  Face *faces;
  int face_count;
  int face_cap;
  BlockType *blocks;
  int size_x;
  int size_z;
  int y_min;
  int y_max;
  int render_distance_chunks;
  int chunk_cx;
  int chunk_cz;
  bool mesh_dirty;
} Mc;

bool mc_init(Mc *mc);
void mc_shutdown(Mc *mc);
void mc_handle_event(Mc *mc, const SDL_Event *event);
void mc_frame(Mc *mc, Uint32 now, float dt);
