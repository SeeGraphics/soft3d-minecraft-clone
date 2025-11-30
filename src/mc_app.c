#include "mc.h"
#include "world.h"
#include "colors.h"
#include "math.h"
#include "render.h"
#include "shapes.h"
#include "text.h"
#include "utils.h"
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  v3f view_pos;
  v2f uv;
} ClipVert;

typedef struct
{
  Face *face;
  float depth;
} TransparentFace;

static v3f camera_forward(const Camera *cam)
{
  float cy = cosf(cam->yaw);
  float sy = sinf(cam->yaw);
  float cp = cosf(cam->pitch);
  float sp = sinf(cam->pitch);
  return v3_normalize((v3f){sy * cp, sp, -cy * cp});
}

static void clear_depth(float *depth, size_t count)
{
  for (size_t i = 0; i < count; i++)
  {
    depth[i] = 1.0f;
  }
}

static void resize_render(Game *game, int window_w, int window_h,
                          int render_scale)
{
  game->window_w = (u32)window_w;
  game->window_h = (u32)window_h;
  game->render_w = (u32)(window_w / render_scale);
  game->render_h = (u32)(window_h / render_scale);
  if (game->render_w == 0)
    game->render_w = 1;
  if (game->render_h == 0)
    game->render_h = 1;

  buffer_reallocate(&game->buffer, game->render_w, game->render_h,
                    sizeof(u32));
  if (game->depth)
  {
    free(game->depth);
  }
  game->depth = malloc(game->render_w * game->render_h * sizeof(float));
  pitch_update(&game->pitch, game->render_w, sizeof(u32));
  if (game->renderer)
  {
    texture_recreate(&game->texture, game->renderer, game->render_w,
                     game->render_h);
  }
}

static bool load_texture(Texture *tex, const char *path)
{
  return texture_load(tex, path);
}

static u32 sample_sky(const Texture *tex, v3f dir)
{
  // dir is normalized
  float u = (atan2f(dir.x, -dir.z) + (float)M_PI) * (0.5f / (float)M_PI);
  float v = 0.5f - asinf(dir.y) / (float)M_PI;
  int tx = (int)(u * (float)(tex->w - 1)) % tex->w;
  int ty = (int)(v * (float)(tex->h - 1));
  if (tx < 0)
    tx += tex->w;
  if (ty < 0)
    ty = 0;
  if (ty >= tex->h)
    ty = tex->h - 1;
  return tex->pixels[ty * tex->w + tx] | 0xFF000000u;
}

static void draw_sky(Mc *mc, float fov, float aspect, v3f forward, v3f right,
                     v3f up)
{
  if (!mc->sky_tex.pixels)
  {
    memset(mc->game.buffer, 0x70, mc->game.render_w * mc->game.render_h *
                                    sizeof(u32)); // fallback dull blue-ish
    return;
  }
  Game *game = &mc->game;
  float tan_half = tanf(fov * 0.5f);
  int w = (int)game->render_w;
  int h = (int)game->render_h;
  for (int y = 0; y < h; y++)
  {
    float ny = 1.0f - ((2.0f * ((float)y + 0.5f)) / (float)h);
    for (int x = 0; x < w; x++)
    {
      float nx = ((2.0f * ((float)x + 0.5f)) / (float)w) - 1.0f;
      v3f dir_cam = v3_normalize((v3f){nx * aspect * tan_half, ny * tan_half, -1.0f});
      v3f dir_world = v3_normalize(v3_add(v3_add(v3_scale(right, dir_cam.x),
                                                 v3_scale(up, dir_cam.y)),
                                          v3_scale(forward, dir_cam.z)));
      game->buffer[y * w + x] = sample_sky(&mc->sky_tex, dir_world);
    }
  }
}

static float hash2i(int x, int z)
{
  // Integer hash, returns [0,1)
  uint32_t h = (uint32_t)(x * 374761393u + z * 668265263u);
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  return (float)h / 4294967295.0f;
}

static float value_noise(float x, float z)
{
  int xi = (int)floorf(x);
  int zi = (int)floorf(z);
  float xf = x - (float)xi;
  float zf = z - (float)zi;
  float v00 = hash2i(xi, zi);
  float v10 = hash2i(xi + 1, zi);
  float v01 = hash2i(xi, zi + 1);
  float v11 = hash2i(xi + 1, zi + 1);
  float tx = xf * xf * (3.0f - 2.0f * xf);
  float tz = zf * zf * (3.0f - 2.0f * zf);
  float xa = v00 * (1.0f - tx) + v10 * tx;
  float xb = v01 * (1.0f - tx) + v11 * tx;
  return xa * (1.0f - tz) + xb * tz;
}

static float fbm2(float x, float z, int octaves, float lacunarity, float gain)
{
  float amp = 1.0f;
  float freq = 1.0f;
  float sum = 0.0f;
  float norm = 0.0f;
  for (int i = 0; i < octaves; i++)
  {
    sum += value_noise(x * freq, z * freq) * amp;
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

// Checks that all blocks between y0 and y1 (inclusive) are air; order of y0/y1
// does not matter.
static bool column_is_clear(const Mc *mc, int x, int y0, int y1, int z)
{
  int step = (y0 <= y1) ? 1 : -1;
  for (int y = y0; y != y1 + step; y += step)
  {
    if (block_get(mc, x, y, z) != BLOCK_AIR)
    {
      return false;
    }
  }
  return true;
}

static void place_tree(Mc *mc, int x, int y, int z, int trunk_h)
{
  for (int i = 0; i < trunk_h; i++)
  {
    block_set(mc, x, y - i, z, BLOCK_OAK_LOG);
  }
  int trunk_top = y - (trunk_h - 1);
  int canopy_base = trunk_top - 1;
  for (int ly = 0; ly <= 2; ly++)
  {
    int yy = canopy_base - ly;
    if (yy < mc->y_min)
      break;
    int radius = (ly == 2) ? 1 : 2;
    for (int dx = -radius; dx <= radius; dx++)
    {
      for (int dz = -radius; dz <= radius; dz++)
      {
        if (abs(dx) + abs(dz) > radius + 1)
          continue;
        int px = x + dx;
        int pz = z + dz;
        if (block_get(mc, px, yy, pz) == BLOCK_AIR)
        {
          block_set(mc, px, yy, pz, BLOCK_LEAVES);
        }
      }
    }
  }
  int top_leaf_y = canopy_base - 3;
  if (top_leaf_y >= mc->y_min && block_get(mc, x, top_leaf_y, z) == BLOCK_AIR)
  {
    block_set(mc, x, top_leaf_y, z, BLOCK_LEAVES);
  }
}

static void try_place_tree(Mc *mc, int x, int z, float chance)
{
  float roll = hash2i(x * 31, z * 17);
  if (roll >= chance)
  {
    return;
  }

  int top_y = -1;
  for (int y = mc->y_min; y <= mc->y_max; y++)
  {
    BlockType t = block_get(mc, x, y, z);
    if (t != BLOCK_AIR)
    {
      top_y = y;
      break;
    }
  }
  if (top_y < 0)
    return;

  BlockType ground = block_get(mc, x, top_y, z);
  if (ground != BLOCK_GRASS)
    return;

  int trunk_h = 4 + (int)(hash2i(x * 13, z * 29) * 3.0f); // 4-6 tall
  int clear_to = top_y - trunk_h - 2;
  if (clear_to < mc->y_min)
    return;

  if (!column_is_clear(mc, x, top_y - 1, clear_to, z))
    return;

  place_tree(mc, x, top_y - 1, z, trunk_h);
}

static const char *block_name(BlockType t)
{
  switch (t)
  {
  case BLOCK_GRASS:
    return "GRASS";
  case BLOCK_DIRT:
    return "DIRT";
  case BLOCK_STONE:
    return "STONE";
  case BLOCK_OAK_LOG:
    return "OAK LOG";
  case BLOCK_OAK_PLANKS:
    return "OAK PLANKS";
  case BLOCK_COBBLESTONE:
    return "COBBLESTONE";
  case BLOCK_LEAVES:
    return "LEAVES";
  case BLOCK_GLASS:
    return "GLASS";
  default:
    return "UNKNOWN";
  }
}

static float face_view_depth(const Face *face, const mat4 *mv)
{
  v3f center = {(face->v[0].pos.x + face->v[1].pos.x + face->v[2].pos.x) / 3.0f,
                (face->v[0].pos.y + face->v[1].pos.y + face->v[2].pos.y) / 3.0f,
                (face->v[0].pos.z + face->v[1].pos.z + face->v[2].pos.z) / 3.0f};
  v4f view_pos = mat4_mul_v4(*mv, (v4f){center.x, center.y, center.z, 1.0f});
  return -view_pos.z;
}

static int compare_transparent_face(const void *a, const void *b)
{
  float da = ((const TransparentFace *)a)->depth;
  float db = ((const TransparentFace *)b)->depth;
  if (da < db)
    return 1;
  if (da > db)
    return -1;
  return 0;
}

static void cycle_block(Mc *mc, bool forward)
{
  static const BlockType order[] = {BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE,
                                    BLOCK_OAK_LOG, BLOCK_OAK_PLANKS, BLOCK_COBBLESTONE,
                                    BLOCK_LEAVES, BLOCK_GLASS};
  const int count = (int)(sizeof(order) / sizeof(order[0]));
  int idx = 0;
  for (int i = 0; i < count; i++)
  {
    if (order[i] == mc->selected_block)
    {
      idx = i;
      break;
    }
  }
  idx = forward ? (idx + 1) % count : (idx - 1 + count) % count;
  mc->selected_block = order[idx];
}

static bool project_vertex(const ClipVert *cv, const mat4 *proj, int render_w,
                           int render_h, VertexPC *out, int *mask_out)
{
  v4f clip = mat4_mul_v4(*proj,
                         (v4f){cv->view_pos.x, cv->view_pos.y, cv->view_pos.z,
                               1.0f});
  if (clip.w == 0.0f)
  {
    return false;
  }

  int mask = 0;
  if (clip.x < -clip.w)
    mask |= 1;
  if (clip.x > clip.w)
    mask |= 2;
  if (clip.y < -clip.w)
    mask |= 4;
  if (clip.y > clip.w)
    mask |= 8;
  if (clip.z < 0.0f)
    mask |= 16;
  if (clip.z > clip.w)
    mask |= 32;

  float inv_w = 1.0f / clip.w;
  v3f ndc = {clip.x * inv_w, clip.y * inv_w, clip.z * inv_w};
  out->pos = norm_to_screen((v2f){ndc.x, ndc.y}, render_w, render_h);
  out->uv = cv->uv;
  out->inv_w = inv_w;
  out->depth = 0.5f * (ndc.z + 1.0f);
  *mask_out = mask;
  return true;
}

bool mc_init(Mc *mc)
{
  *mc = (Mc){0};
  mc->game.window_w = 960;
  mc->game.window_h = 540;
  mc->render_scale = 2;
  mc->near_plane = 0.1f;
  mc->far_plane = 500.0f; // extend draw distance to cover the larger world
  mc->mouse_sens = 0.0025f;
  mc->camera = (Camera){.pos = {0.0f, 1.5f, 6.0f}, .yaw = 0.0f, .pitch = 0.0f};
  resize_render(&mc->game, (int)mc->game.window_w,
                (int)mc->game.window_h, mc->render_scale);
  mc->size_x = CHUNK_SIZE * 32; 
  mc->size_z = CHUNK_SIZE * 32;
  mc->y_min = 0;
  mc->y_max = 31;
  mc->render_distance_chunks = 4; // initial render distance
  mc->chunk_cx = -1;
  mc->chunk_cz = -1;
  mc->selected_block = BLOCK_DIRT;

  if (SDL_Init(SDL_INIT_VIDEO) != 0)
  {
    SDL_Log("Failed to Initialize SDL: %s\n", SDL_GetError());
    return false;
  }

  if (!(IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP) &
        (IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP)))
  {
    SDL_Log("Failed to init SDL_image: %s\n", IMG_GetError());
    SDL_Quit();
    return false;
  }

  if (!load_texture(&mc->dirt_tex, "assets/dirt.png") ||
      !load_texture(&mc->stone_tex, "assets/stone.png") ||
      !load_texture(&mc->grass_side_tex, "assets/grass_side.png") ||
      !load_texture(&mc->grass_top_tex, "assets/grass_top.png") ||
      !load_texture(&mc->oak_log_side_tex, "assets/oak_log_side.png") ||
      !load_texture(&mc->oak_log_top_tex, "assets/oak_log_top.png") ||
      !load_texture(&mc->oak_planks_tex, "assets/oak_planks.png") ||
      !load_texture(&mc->cobblestone_tex, "assets/cobblestone.png") ||
      !load_texture(&mc->leaves_tex, "assets/leaves.png") ||
      !load_texture(&mc->glass_tex, "assets/glass.png") ||
      !load_texture(&mc->sky_tex, "assets/sky.png"))
  {
    IMG_Quit();
    SDL_Quit();
    return false;
  }

  const char *title = "Chunk";
  mc->game.window = SDL_CreateWindow(
      title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      (int)mc->game.window_w, (int)mc->game.window_h,
      SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_RESIZABLE);
  if (mc->game.window == NULL)
  {
    SDL_Log("Failed to create Window: %s\n", SDL_GetError());
    texture_destroy(&mc->dirt_tex);
    texture_destroy(&mc->stone_tex);
    IMG_Quit();
    SDL_Quit();
    return false;
  }
  SDL_RaiseWindow(mc->game.window);

  mc->game.renderer =
      SDL_CreateRenderer(mc->game.window, -1, SDL_RENDERER_ACCELERATED);
  if (mc->game.renderer == NULL)
  {
    SDL_Log("Failed to create Renderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(mc->game.window);
    texture_destroy(&mc->dirt_tex);
    texture_destroy(&mc->stone_tex);
    IMG_Quit();
    SDL_Quit();
    return false;
  }

  texture_recreate(&mc->game.texture, mc->game.renderer,
                   mc->game.render_w, mc->game.render_h);
  SDL_SetRelativeMouseMode(SDL_TRUE);
  SDL_ShowCursor(SDL_DISABLE);
  mc->game.mouse_grabbed = true;
  mc->fps = 0.0f;
  mc->culled_faces_count = 0;
  mc->rendered_faces_count = 0;
  mc->velocity = (v3f){0};
  mc->grounded = false;
  mc->last_ticks = SDL_GetTicks();
  mc->running = true;

  int block_count = mc->size_x * (mc->y_max - mc->y_min + 1) * mc->size_z;
  mc->blocks = calloc((size_t)block_count, sizeof(BlockType));
  const float scale = 0.08f;
  const int dirt_depth = 3;
  const int stone_start = 12;
  for (int x = 0; x < mc->size_x; x++)
  {
    for (int z = 0; z < mc->size_z; z++)
    {
      float h = fbm2((float)x * scale, (float)z * scale, 4, 2.0f, 0.5f);
      int surface = stone_start + (int)(h * 8.0f); // 1..9 ish
      if (surface > mc->y_max)
        surface = mc->y_max;
      if (surface < 1)
        surface = 1;
      int dirt_end = surface + dirt_depth;
      if (dirt_end > mc->y_max)
        dirt_end = mc->y_max;
      for (int y = surface; y <= mc->y_max; y++)
      {
        BlockType t;
        if (y == surface)
        {
          t = BLOCK_GRASS;
        }
        else if (y <= dirt_end)
        {
          t = BLOCK_DIRT;
        }
        else
        {
          t = BLOCK_STONE;
        }
        block_set(mc, x, y, z, t);
      }
    }
  }

  const float tree_chance = 0.01f; // denser trees
  for (int x = 0; x < mc->size_x; x++)
  {
    for (int z = 0; z < mc->size_z; z++)
    {
      try_place_tree(mc, x, z, tree_chance);
    }
  }
  rebuild_faces(mc);
  return true;
}

void mc_shutdown(Mc *mc)
{
  if (mc->faces)
  {
    free(mc->faces);
    mc->faces = NULL;
  }
  if (mc->blocks)
  {
    free(mc->blocks);
    mc->blocks = NULL;
  }
  if (mc->game.buffer)
  {
    free(mc->game.buffer);
    mc->game.buffer = NULL;
  }
  if (mc->game.depth)
  {
    free(mc->game.depth);
    mc->game.depth = NULL;
  }
  texture_destroy(&mc->dirt_tex);
  texture_destroy(&mc->stone_tex);
  texture_destroy(&mc->grass_side_tex);
  texture_destroy(&mc->grass_top_tex);
  texture_destroy(&mc->oak_log_side_tex);
  texture_destroy(&mc->oak_log_top_tex);
  texture_destroy(&mc->oak_planks_tex);
  texture_destroy(&mc->cobblestone_tex);
  texture_destroy(&mc->leaves_tex);
  texture_destroy(&mc->glass_tex);
  texture_destroy(&mc->sky_tex);
  if (mc->game.texture)
  {
    SDL_DestroyTexture(mc->game.texture);
    mc->game.texture = NULL;
  }
  if (mc->game.renderer)
  {
    SDL_DestroyRenderer(mc->game.renderer);
    mc->game.renderer = NULL;
  }
  if (mc->game.window)
  {
    SDL_DestroyWindow(mc->game.window);
    mc->game.window = NULL;
  }
  IMG_Quit();
  SDL_Quit();
}

void mc_handle_event(Mc *mc, const SDL_Event *event)
{
  Game *game = &mc->game;
  switch (event->type)
  {
  case SDL_QUIT:
    mc->running = false;
    break;
  case SDL_WINDOWEVENT:
    if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
    {
      resize_render(&mc->game, event->window.data1, event->window.data2,
                    mc->render_scale);
      mc->mesh_dirty = true;
    }
    break;
  case SDL_MOUSEMOTION:
    if (game->mouse_grabbed)
    {
      mc->camera.yaw += (float)event->motion.xrel * mc->mouse_sens;
      mc->camera.pitch -= (float)event->motion.yrel * mc->mouse_sens;
    }
    break;
  case SDL_KEYDOWN:
    if (event->key.keysym.sym == SDLK_ESCAPE)
    {
      mc->running = false;
    }
    if (event->key.keysym.sym == SDLK_1)
    {
      mc->selected_block = BLOCK_GRASS;
    }
    if (event->key.keysym.sym == SDLK_2)
    {
      mc->selected_block = BLOCK_DIRT;
    }
    if (event->key.keysym.sym == SDLK_3)
    {
      mc->selected_block = BLOCK_STONE;
    }
    if (event->key.keysym.sym == SDLK_4)
    {
      mc->selected_block = BLOCK_OAK_LOG;
    }
    if (event->key.keysym.sym == SDLK_5)
    {
      mc->selected_block = BLOCK_OAK_PLANKS;
    }
    if (event->key.keysym.sym == SDLK_6)
    {
      mc->selected_block = BLOCK_COBBLESTONE;
    }
    if (event->key.keysym.sym == SDLK_7)
    {
      mc->selected_block = BLOCK_LEAVES;
    }
    if (event->key.keysym.sym == SDLK_8)
    {
      mc->selected_block = BLOCK_GLASS;
    }
    if (event->key.keysym.sym == SDLK_v)
    {
      mc->noclip = !mc->noclip;
      mc->velocity = (v3f){0};
      mc->grounded = true;
    }
    if (event->key.keysym.sym == SDLK_r)
    {
      mc->wireframe = !mc->wireframe;
    }
    if (event->key.keysym.sym == SDLK_q)
    {
      game->mouse_grabbed = !game->mouse_grabbed;
      SDL_SetRelativeMouseMode(game->mouse_grabbed ? SDL_TRUE : SDL_FALSE);
      SDL_ShowCursor(game->mouse_grabbed ? SDL_DISABLE : SDL_ENABLE);
    }
    if (event->key.keysym.sym == SDLK_f)
    {
      Uint32 flags = SDL_GetWindowFlags(game->window);
      bool is_full = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
      if (SDL_SetWindowFullscreen(
              game->window,
              is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
      {
        int w, h;
        SDL_GetWindowSize(game->window, &w, &h);
        resize_render(&mc->game, w, h, mc->render_scale);
      }
    }
    break;
  case SDL_MOUSEWHEEL:
    if (event->wheel.y > 0)
    {
      cycle_block(mc, true);
    }
    else if (event->wheel.y < 0)
    {
      cycle_block(mc, false);
    }
    break;
  case SDL_MOUSEBUTTONDOWN:
    if (event->button.button == SDL_BUTTON_LEFT ||
        event->button.button == SDL_BUTTON_RIGHT)
    {
      int hx, hy, hz;
      v3f normal;
      if (raycast_block(mc, mc->camera.pos,
                        camera_forward(&mc->camera), 6.0f, &hx, &hy, &hz,
                        &normal))
      {
        if (event->button.button == SDL_BUTTON_LEFT)
        {
          if (block_get(mc, hx, hy, hz) != BLOCK_AIR)
          {
            block_set(mc, hx, hy, hz, BLOCK_AIR);
          }
        }
        else
        {
          int tx = hx + (int)normal.x;
          int ty = hy + (int)normal.y;
          int tz = hz + (int)normal.z;
          if (block_get(mc, tx, ty, tz) == BLOCK_AIR)
          {
            block_set(mc, tx, ty, tz, mc->selected_block);
          }
        }
      }
    }
    break;
  default:
    break;
  }
}

void mc_frame(Mc *mc, Uint32 now, float dt)
{
  Game *game = &mc->game;
  const float world_up_y = 1.0f;
  mc->culled_faces_count = 0;
  mc->rendered_faces_count = 0;

  const Uint8 *state = SDL_GetKeyboardState(NULL);
  v3f forward_move = camera_forward(&mc->camera);
  v3f world_up = {0.0f, world_up_y, 0.0f};
  // Build a stable camera basis that doesn't spin when looking straight up/down
  v3f right_move = v3_cross(forward_move, world_up);
  float right_len_sq = v3_dot(right_move, right_move);
  if (right_len_sq < 1e-6f)
  {
    right_move = (v3f){1.0f, 0.0f, 0.0f};
  }
  else
  {
    right_move = v3_scale(right_move, 1.0f / sqrtf(right_len_sq));
  }

  if (mc->noclip)
  {
    float move_speed = 20.0f * dt; 
    if (state[SDL_SCANCODE_W])
    {
      mc->camera.pos = v3_add(mc->camera.pos, v3_scale(forward_move, move_speed));
    }
    if (state[SDL_SCANCODE_S])
    {
      mc->camera.pos = v3_sub(mc->camera.pos, v3_scale(forward_move, move_speed));
    }
    if (state[SDL_SCANCODE_A])
    {
      mc->camera.pos = v3_sub(mc->camera.pos, v3_scale(right_move, move_speed));
    }
    if (state[SDL_SCANCODE_D])
    {
      mc->camera.pos = v3_add(mc->camera.pos, v3_scale(right_move, move_speed));
    }
    if (state[SDL_SCANCODE_SPACE])
    {
      mc->camera.pos.y += move_speed;
    }
    if (state[SDL_SCANCODE_LCTRL])
    {
      mc->camera.pos.y -= move_speed;
    }
    mc->velocity = (v3f){0};
    mc->grounded = true;
  }
  else
  {
    // flatten forward for ground movement so looking up/down doesnt move vertically
    v3f forward_flat = {forward_move.x, 0.0f, forward_move.z};
    if (v3_dot(forward_flat, forward_flat) > 0.0f)
    {
      forward_flat = v3_normalize(forward_flat);
    }

    float remaining = dt;
    const float max_step = 0.02f; // smaller steps to avoid tunneling through blocks
    while (remaining > 0.0f)
    {
      float step = (remaining > max_step) ? max_step : remaining;
      remaining -= step;

      v3f move_dir = {0};
      if (state[SDL_SCANCODE_W])
      {
        move_dir = v3_add(move_dir, forward_flat);
      }
      if (state[SDL_SCANCODE_S])
      {
        move_dir = v3_sub(move_dir, forward_flat);
      }
      if (state[SDL_SCANCODE_A])
      {
        move_dir = v3_sub(move_dir, right_move);
      }
      if (state[SDL_SCANCODE_D])
      {
        move_dir = v3_add(move_dir, right_move);
      }
      if (v3_dot(move_dir, move_dir) > 0.0f)
      {
        move_dir = v3_normalize(move_dir);
        mc->camera.pos = v3_add(mc->camera.pos, v3_scale(move_dir, WALK_SPEED * step));
      }

      // Jump only allow when grounded
      if (mc->grounded && state[SDL_SCANCODE_SPACE])
      {
        mc->velocity.y = JUMP_VELOCITY;
        mc->grounded = false;
      }

      // Apply gravity and vertical velocity
      mc->velocity.y -= GRAVITY * step;
      mc->camera.pos.y += mc->velocity.y * step;

      resolve_collisions(mc);
    }
  }

  float look_speed = 1.5f * dt;
  if (state[SDL_SCANCODE_LEFT])
  {
    mc->camera.yaw -= look_speed;
  }
  if (state[SDL_SCANCODE_RIGHT])
  {
    mc->camera.yaw += look_speed;
  }
  if (state[SDL_SCANCODE_UP])
  {
    mc->camera.pitch += look_speed;
  }
  if (state[SDL_SCANCODE_DOWN])
  {
    mc->camera.pitch -= look_speed;
  }
  float max_pitch = (float)M_PI_2 - 0.1f;
  if (mc->camera.pitch > max_pitch)
    mc->camera.pitch = max_pitch;
  if (mc->camera.pitch < -max_pitch)
    mc->camera.pitch = -max_pitch;

  // Recompute camera basis after look input for rendering (sky + view)
  v3f forward = camera_forward(&mc->camera);
  v3f right = v3_cross(forward, world_up);
  float right_len_sq_render = v3_dot(right, right);
  if (right_len_sq_render < 1e-6f)
  {
    right = (v3f){1.0f, 0.0f, 0.0f};
  }
  else
  {
    right = v3_scale(right, 1.0f / sqrtf(right_len_sq_render));
  }

  if (!mc->noclip)
  {
    // Jump only allow when grounded
    if (mc->grounded && state[SDL_SCANCODE_SPACE])
    {
      mc->velocity.y = JUMP_VELOCITY;
      mc->grounded = false;
    }

    // Apply gravity and vertical velocity
    mc->velocity.y -= GRAVITY * dt;
    mc->camera.pos.y += mc->velocity.y * dt;

    resolve_collisions(mc);
  }

  // Trigger mesh rebuild when crossing chunk boundaries
  int cam_block_x = (int)floorf(mc->camera.pos.x + (float)mc->size_x * 0.5f);
  int cam_block_z = (int)floorf(mc->camera.pos.z + (float)mc->size_z * 0.5f);
  int cam_chunk_x = cam_block_x / CHUNK_SIZE;
  int cam_chunk_z = cam_block_z / CHUNK_SIZE;
  if (cam_chunk_x != mc->chunk_cx || cam_chunk_z != mc->chunk_cz)
  {
    mc->chunk_cx = cam_chunk_x;
    mc->chunk_cz = cam_chunk_z;
    mc->mesh_dirty = true;
  }

  const float fov = (float)M_PI / 3.0f;
  float aspect = (float)game->render_w / (float)game->render_h;
  float yaw = mc->camera.yaw;
  v3f sky_forward = {sinf(yaw), 0.0f, -cosf(yaw)};
  v3f sky_right = {cosf(yaw), 0.0f, sinf(yaw)};
  v3f sky_up = {0.0f, 1.0f, 0.0f};
  draw_sky(mc, fov, aspect, sky_forward, sky_right, sky_up);
  clear_depth(game->depth, (size_t)game->render_w * (size_t)game->render_h);
  if (mc->mesh_dirty)
  {
    rebuild_faces(mc);
  }

  mat4 model = mat4_identity();
  mat4 view = mat4_look_at(
      mc->camera.pos,
      v3_add(mc->camera.pos, camera_forward(&mc->camera)), (v3f){0.0f, world_up_y, 0.0f});
  mat4 proj = mat4_perspective(fov, aspect, mc->near_plane, mc->far_plane);
  mat4 mv = mat4_mul(view, model);

  Face **render_faces = NULL;
  TransparentFace *transparent_faces = NULL;
  int total_faces = mc->face_count;
  if (mc->face_count > 0)
  {
    render_faces = malloc((size_t)mc->face_count * sizeof(Face *));
    transparent_faces = malloc((size_t)mc->face_count * sizeof(TransparentFace));
  }
  int opaque_count = 0;
  int transparent_count = 0;
  bool have_order = render_faces && transparent_faces;
  if (have_order)
  {
  for (int face_idx = 0; face_idx < total_faces; face_idx++)
    {
      Face *face = &mc->faces[face_idx];
      bool is_transparent = (face->tex == &mc->glass_tex) || (face->tex == &mc->leaves_tex);
      if (is_transparent)
      {
        transparent_faces[transparent_count].face = face;
        transparent_faces[transparent_count].depth = face_view_depth(face, &mv);
        transparent_count++;
      }
      else
      {
        render_faces[opaque_count++] = face;
      }
    }
    if (transparent_count > 1)
    {
      qsort(transparent_faces, (size_t)transparent_count, sizeof(TransparentFace),
            compare_transparent_face);
    }
    for (int i = 0; i < transparent_count; i++)
    {
      render_faces[opaque_count + i] = transparent_faces[i].face;
    }
    total_faces = opaque_count + transparent_count;
  }
  else
  {
    free(render_faces);
    free(transparent_faces);
    render_faces = NULL;
    transparent_faces = NULL;
  }

  typedef struct
  {
    v2i screen;
    v2f uv;
    v3f view_pos;
    float inv_w;
    float depth;
    int clip_mask;
    bool depth_ok;
  } CachedVertex;
  CachedVertex tri[3];

  for (int face_idx = 0; face_idx < total_faces; face_idx++)
  {
    Face *face =
        render_faces ? render_faces[face_idx] : &mc->faces[face_idx];
    bool is_transparent = (face->tex == &mc->glass_tex) || (face->tex == &mc->leaves_tex);

      bool skip = false;
      for (int j = 0; j < 3; j++)
      {
        v4f world = {face->v[j].pos.x, face->v[j].pos.y, face->v[j].pos.z,
                     1.0f};
        v4f view_pos4 = mat4_mul_v4(mv, world);
        v4f clip = mat4_mul_v4(proj, view_pos4);

        tri[j].uv = face->v[j].uv;
        tri[j].view_pos = (v3f){view_pos4.x, view_pos4.y, view_pos4.z};

        int mask = 0;
        if (clip.w == 0.0f)
        {
          tri[j].clip_mask = 0x3F; // force cull
          tri[j].depth_ok = false;
          skip = true;
          break;
        }
        if (clip.x < -clip.w)
          mask |= 1;
        if (clip.x > clip.w)
          mask |= 2;
        if (clip.y < -clip.w)
          mask |= 4;
        if (clip.y > clip.w)
          mask |= 8;
        if (clip.z < 0.0f)
          mask |= 16;
        if (clip.z > clip.w)
          mask |= 32;
        tri[j].clip_mask = mask;

        float inv_w = 1.0f / clip.w;
        tri[j].inv_w = inv_w;
        v3f ndc = {clip.x * inv_w, clip.y * inv_w, clip.z * inv_w};
        tri[j].depth_ok = ndc.z >= -1.0f && ndc.z <= 1.0f;
        tri[j].screen =
            norm_to_screen((v2f){ndc.x, ndc.y}, game->render_w, game->render_h);
        tri[j].depth = 0.5f * (ndc.z + 1.0f);
      }
      if (skip)
      {
        continue;
      }

      if ((tri[0].clip_mask & tri[1].clip_mask & tri[2].clip_mask) != 0)
      {
        mc->culled_faces_count++;
        continue; // frustum culled
      }

      bool near_in[3] = {tri[0].view_pos.z <= -mc->near_plane,
                         tri[1].view_pos.z <= -mc->near_plane,
                         tri[2].view_pos.z <= -mc->near_plane};
      bool needs_clip = !(near_in[0] && near_in[1] && near_in[2]);

      if (!needs_clip)
      {
        if (!tri[0].depth_ok || !tri[1].depth_ok || !tri[2].depth_ok)
        {
          continue;
        }
        v3f edge1 = v3_sub(tri[1].view_pos, tri[0].view_pos);
        v3f edge2 = v3_sub(tri[2].view_pos, tri[0].view_pos);
        v3f normal = v3_cross(edge1, edge2);
        if (v3_dot(normal, tri[0].view_pos) >= 0.0f)
        {
          continue;
        }
        VertexPC pv[3] = {
            {.pos = tri[0].screen, .uv = tri[0].uv, .inv_w = tri[0].inv_w, .depth = tri[0].depth},
            {.pos = tri[1].screen, .uv = tri[1].uv, .inv_w = tri[1].inv_w, .depth = tri[1].depth},
            {.pos = tri[2].screen, .uv = tri[2].uv, .inv_w = tri[2].inv_w, .depth = tri[2].depth},
        };

        if (mc->wireframe)
        {
          draw_triangle(game->buffer, game->render_w, game->render_h, pv[0].pos,
                        pv[1].pos, pv[2].pos, WHITE, WIREFRAME);
        }
        else if (is_transparent)
        {
          draw_textured_triangle_alpha(game->buffer, game->depth, game->render_w,
                                       game->render_h, face->tex, pv[0], pv[1],
                                       pv[2], false);
        }
        else
        {
          draw_textured_triangle(game->buffer, game->depth, game->render_w,
                                 game->render_h, face->tex, pv[0], pv[1], pv[2]);
        }
        mc->rendered_faces_count++;
      }
      else
      {
        ClipVert in_poly[4] = {
            {.view_pos = tri[0].view_pos, .uv = tri[0].uv},
            {.view_pos = tri[1].view_pos, .uv = tri[1].uv},
            {.view_pos = tri[2].view_pos, .uv = tri[2].uv},
        };
        int in_count = 3;
        ClipVert out_poly[4];
        int out_count = 0;

        for (int v = 0; v < in_count; v++)
        {
          ClipVert a = in_poly[v];
          ClipVert b = in_poly[(v + 1) % in_count];
          bool a_in = a.view_pos.z <= -mc->near_plane;
          bool b_in = b.view_pos.z <= -mc->near_plane;

          if (a_in && b_in)
          {
            out_poly[out_count++] = b;
          }
          else if (a_in && !b_in)
          {
            float t = (-mc->near_plane - a.view_pos.z) /
                      (b.view_pos.z - a.view_pos.z);
            ClipVert inter = {
                .view_pos = {a.view_pos.x + (b.view_pos.x - a.view_pos.x) * t,
                             a.view_pos.y + (b.view_pos.y - a.view_pos.y) * t,
                             -mc->near_plane},
                .uv = {a.uv.x + (b.uv.x - a.uv.x) * t,
                       a.uv.y + (b.uv.y - a.uv.y) * t}};
            out_poly[out_count++] = inter;
          }
          else if (!a_in && b_in)
          {
            float t = (-mc->near_plane - a.view_pos.z) /
                      (b.view_pos.z - a.view_pos.z);
            ClipVert inter = {
                .view_pos = {a.view_pos.x + (b.view_pos.x - a.view_pos.x) * t,
                             a.view_pos.y + (b.view_pos.y - a.view_pos.y) * t,
                             -mc->near_plane},
                .uv = {a.uv.x + (b.uv.x - a.uv.x) * t,
                       a.uv.y + (b.uv.y - a.uv.y) * t}};
            out_poly[out_count++] = inter;
            out_poly[out_count++] = b;
          }
        }

        if (out_count < 3)
        {
          continue;
        }

        int tri_sets[2][3] = {{0, 1, 2}, {0, 2, 3}};
        int tri_total = (out_count == 4) ? 2 : 1;

        for (int t = 0; t < tri_total; t++)
        {
          ClipVert *a = &out_poly[tri_sets[t][0]];
          ClipVert *b = &out_poly[tri_sets[t][1]];
          ClipVert *c = &out_poly[tri_sets[t][2]];

          v3f edge1 = v3_sub(b->view_pos, a->view_pos);
          v3f edge2 = v3_sub(c->view_pos, a->view_pos);
          v3f normal = v3_cross(edge1, edge2);
          if (v3_dot(normal, a->view_pos) >= 0.0f)
          {
            continue;
          }

          VertexPC pv[3];
          int masks[3];
          if (!project_vertex(a, &proj, game->render_w, game->render_h, &pv[0],
                              &masks[0]) ||
              !project_vertex(b, &proj, game->render_w, game->render_h, &pv[1],
                              &masks[1]) ||
              !project_vertex(c, &proj, game->render_w, game->render_h, &pv[2],
                              &masks[2]))
          {
            continue;
          }
          if ((masks[0] & masks[1] & masks[2]) != 0)
          {
            continue;
          }

          if (mc->wireframe)
          {
            draw_triangle(game->buffer, game->render_w, game->render_h, pv[0].pos,
                          pv[1].pos, pv[2].pos, WHITE, WIREFRAME);
          }
          else if (is_transparent)
          {
            draw_textured_triangle_alpha(game->buffer, game->depth, game->render_w,
                                         game->render_h, face->tex, pv[0], pv[1],
                                         pv[2], false);
          }
          else
          {
            draw_textured_triangle(game->buffer, game->depth, game->render_w,
                                   game->render_h, face->tex, pv[0], pv[1], pv[2]);
          }
          mc->rendered_faces_count++;
        }
      }
  }

  free(render_faces);
  free(transparent_faces);

  char fps_text[32];
  snprintf(fps_text, sizeof(fps_text), "FPS: %d", (int)(mc->fps + 0.5f));
  draw_text(game->buffer, game->render_w, (v2i){5, 5}, fps_text, WHITE);

  char culled_text[256];
  snprintf(culled_text, sizeof(culled_text), "CULLED FACES: %d", mc->culled_faces_count);
  draw_text(game->buffer, game->render_w, (v2i){5, 20}, culled_text, WHITE);

  char rendered_text[256];
  snprintf(rendered_text, sizeof(rendered_text), "RENDERED FACES: %d", mc->rendered_faces_count);
  draw_text(game->buffer, game->render_w, (v2i){5, 35}, rendered_text, WHITE);

  char block_text[64];
  snprintf(block_text, sizeof(block_text), "BLOCK: %s", block_name(mc->selected_block));
  draw_text(game->buffer, game->render_w, (v2i){5, 50}, block_text, WHITE);

  // Crosshair at the render center
  v2i center = {(int)(game->render_w / 2), (int)(game->render_h / 2)};
  int len = 6;
  draw_linei(game->buffer, game->render_w, game->render_h,
             (v2i){center.x - len, center.y}, (v2i){center.x + len, center.y},
             WHITE);
  draw_linei(game->buffer, game->render_w, game->render_h,
             (v2i){center.x, center.y - len}, (v2i){center.x, center.y + len},
             WHITE);

  SDL_UpdateTexture(game->texture, NULL, game->buffer, game->pitch);
  SDL_RenderClear(game->renderer);
  SDL_Rect dest = {0, 0, (int)game->window_w, (int)game->window_h};
  SDL_RenderCopy(game->renderer, game->texture, NULL, &dest);
  SDL_RenderPresent(game->renderer);
}
