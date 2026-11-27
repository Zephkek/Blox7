#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int calls;
static const void* seen_user;
static const char tag_first[] = "first", tag_second[] = "second", tag_third[] = "third", tag_fourth[] = "fourth";

static void (GL_APIENTRY* set_callback)(GLDEBUGPROC, const void*);
static void (GL_APIENTRY* insert_message)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*);
static void (GL_APIENTRY* control_messages)(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean);
static void (GL_APIENTRY* get_pointer)(GLenum, void**);

static void check(int ok, const char* what) {
  printf("%s: %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) failures++;
}

static GLenum take_error(void) {
  const GLenum first = glGetError();
  while (glGetError() != GL_NO_ERROR) {
  }
  return first;
}

static GLint level_param(GLenum target, GLenum pname) {
  GLint value = -1;
  glGetTexLevelParameteriv(target, 0, pname, &value);
  return value;
}

static void GL_APIENTRY record(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                               const GLchar* message, const void* user) {
  (void)source;
  (void)type;
  (void)id;
  (void)severity;
  (void)length;
  (void)message;
  seen_user = user;
  calls++;
}

static const void* message_user(void) {
  seen_user = NULL;
  calls = 0;
  insert_message(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 7, GL_DEBUG_SEVERITY_HIGH, -1, "glbridge_test");
  return seen_user;
}

static void* pointer(GLenum pname) {
  void* value = (void*)&failures;
  get_pointer(pname, &value);
  return value;
}

static void watch_messages(const void* user) {
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  control_messages(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
  set_callback(record, user);
}

static void redone_array(void) {
  static const unsigned char blocks[8 * 8 / 16 * 16 * 2];
  static unsigned char pixels[8 * 8 * 4 * 2];
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
  glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RGBA8_ETC2_EAC, 8, 8, 2, 0, sizeof blocks, blocks);
  check(take_error() == GL_NO_ERROR &&
            level_param(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_INTERNAL_FORMAT) == GL_COMPRESSED_RGBA8_ETC2_EAC,
        "an etc2 array texture reports its etc2 format");
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  check(take_error() == GL_INVALID_OPERATION, "mipmaps of the etc2 array texture are refused");
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 8, 8, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  check(take_error() == GL_NO_ERROR, "glTexImage3D redoes the array texture as rgba8");
  check(level_param(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_INTERNAL_FORMAT) == GL_RGBA8 &&
            level_param(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPRESSED) == GL_FALSE,
        "the redone array texture reports rgba8 and not compressed");
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  check(take_error() == GL_NO_ERROR, "mipmaps of the redone array texture are made");
  glDeleteTextures(1, &texture);
}

static void redone_by_copy(void) {
  static const unsigned char blocks[8 * 8 / 16 * 8];
  GLuint texture = 0, renderbuffer = 0, framebuffer = 0;
  glGenRenderbuffers(1, &renderbuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
  glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 8, 8, 0, sizeof blocks, blocks);
  glGenerateMipmap(GL_TEXTURE_2D);
  check(take_error() == GL_INVALID_OPERATION, "mipmaps of an etc2 texture are refused");
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, 8, 8, 0);
  check(take_error() == GL_NO_ERROR, "glCopyTexImage2D redoes the etc2 texture as rgba8");
  check(level_param(GL_TEXTURE_2D, GL_TEXTURE_INTERNAL_FORMAT) != GL_COMPRESSED_RGB8_ETC2 &&
            level_param(GL_TEXTURE_2D, GL_TEXTURE_COMPRESSED) == GL_FALSE,
        "the copied texture no longer reports etc2");
  glGenerateMipmap(GL_TEXTURE_2D);
  check(take_error() == GL_NO_ERROR, "mipmaps of the copied texture are made");
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteRenderbuffers(1, &renderbuffer);
  glDeleteTextures(1, &texture);
}

static void per_context_callbacks(SDL_Window* window, SDL_GLContext first) {
  watch_messages(tag_first);
  SDL_GLContext second = SDL_GL_CreateContext(window);
  if (!second) {
    check(0, "a second context");
    return;
  }
  watch_messages(tag_second);
  SDL_GL_MakeCurrent(window, first);
  check(message_user() == tag_first && calls == 1, "the first context's callback gets the first context's pointer");
  check(pointer(GL_DEBUG_CALLBACK_FUNCTION) == (void*)record && pointer(GL_DEBUG_CALLBACK_USER_PARAM) == tag_first,
        "the first context reports its own callback and pointer");
  SDL_GL_MakeCurrent(window, second);
  check(message_user() == tag_second && calls == 1, "the second context's callback gets the second context's pointer");
  set_callback(record, tag_third);
  check(message_user() == tag_third && calls == 1, "a callback set again gets the new pointer");
  set_callback(NULL, tag_fourth);
  check(message_user() == NULL && calls == 0, "no callback once it is cleared");
  check(pointer(GL_DEBUG_CALLBACK_FUNCTION) == NULL && pointer(GL_DEBUG_CALLBACK_USER_PARAM) == tag_fourth,
        "a cleared callback reports none and keeps the pointer given with it");
  SDL_GL_MakeCurrent(window, first);
  check(message_user() == tag_first && calls == 1, "the first context still has its own callback");
  SDL_GL_DestroyContext(second);
  SDL_GL_MakeCurrent(window, first);
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    printf("SDL_Init failed: %s\n", SDL_GetError());
    return 2;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
  SDL_Window* window = SDL_CreateWindow("glbridge_test", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 2;
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context) {
    printf("no OpenGL ES 3.1 context: %s\n", SDL_GetError());
    return 2;
  }
  const char* version = (const char*)glGetString(GL_VERSION);
  printf("%s | %s\n", version, (const char*)glGetString(GL_RENDERER));
  if (version && strstr(version, "OpenGL ES 3.2")) {
    set_callback = glDebugMessageCallback;
    insert_message = glDebugMessageInsert;
    control_messages = glDebugMessageControl;
    get_pointer = glGetPointerv;
  } else {
    set_callback = (void (GL_APIENTRY*)(GLDEBUGPROC, const void*))glDebugMessageCallbackKHR;
    insert_message = glDebugMessageInsertKHR;
    control_messages = glDebugMessageControlKHR;
    get_pointer = glGetPointervKHR;
  }
  redone_array();
  redone_by_copy();
  per_context_callbacks(window, context);
  SDL_GL_DestroyContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
