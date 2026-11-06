// sdl3 bridge smoke test: window, gles 3, a beep from sdl's own audio thread
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

static volatile int g_callbacks;
static volatile int g_callback_tid_ok;
static float g_phase;

static void SDLCALL audio_callback(void* userdata, SDL_AudioStream* stream, int additional, int total) {
  (void)userdata; (void)total;
  g_callback_tid_ok = pthread_self() != 0;
  float buf[2048];
  int frames = additional / (int)sizeof(float);
  if (frames > 2048) frames = 2048;
  for (int i = 0; i < frames; i++) {
    buf[i] = 0.1f * sinf(g_phase);
    g_phase += 2.0f * 3.14159265f * 440.0f / 48000.0f;
  }
  SDL_PutAudioStreamData(stream, buf, frames * (int)sizeof(float));
  g_callbacks++;
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
  printf("SDL %d, video driver %s, audio driver %s\n", SDL_GetVersion(), SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver());
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_Window* w = SDL_CreateWindow("Blox7 port: SDL3 bridge test", 640, 360, SDL_WINDOW_OPENGL);
  if (!w) { printf("SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
  SDL_GLContext gl = SDL_GL_CreateContext(w);
  if (!gl) { printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return 1; }
  printf("GL_VERSION %s\nGL_RENDERER %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
  SDL_GL_SetSwapInterval(1);

  SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, 48000};
  SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
  if (s) SDL_ResumeAudioStreamDevice(s);
  else printf("no audio: %s\n", SDL_GetError());

  Uint64 start = SDL_GetTicks();
  int frames = 0;
  while (SDL_GetTicks() - start < 3000) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}
    float t = (SDL_GetTicks() - start) / 1000.0f;
    glClearColor(0.5f + 0.5f * sinf(t * 3), 0.5f + 0.5f * sinf(t * 2 + 1), 0.5f + 0.5f * sinf(t + 2), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(w);
    frames++;
    if (frames == 60 && s) SDL_PauseAudioStreamDevice(s);
  }
  printf("%d frames in 3 s (%.1f fps), %d audio callbacks, callback thread ok: %d\n", frames, frames / 3.0, g_callbacks,
         g_callback_tid_ok);
  if (s) SDL_DestroyAudioStream(s);
  SDL_GL_DestroyContext(gl);
  SDL_DestroyWindow(w);
  SDL_Quit();
  printf("SDL TEST DONE\n");
  return 0;
}
