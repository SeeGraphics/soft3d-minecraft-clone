#include "mc.h"
#include <SDL2/SDL.h>

int main(void)
{
  Mc mc = {0};
  if (!mc_init(&mc))
  {
    return 1;
  }

  while (mc.running)
  {
    Uint32 now = SDL_GetTicks();
    float dt = (now - mc.last_ticks) / 1000.0f;
    if (dt > 0.1f)
    {
      dt = 0.1f; // clamp to avoid huge steps during resize/fullscreen toggles
    }
    mc.last_ticks = now;
    if (dt > 0.0f)
    {
      float inst = 1.0f / dt;
      mc.fps = mc.fps * 0.9f + inst * 0.1f;
    }

    while (SDL_PollEvent(&mc.game.event))
    {
      mc_handle_event(&mc, &mc.game.event);
    }

    mc_frame(&mc, now, dt);
  }

  mc_shutdown(&mc);
  return 0;
}
