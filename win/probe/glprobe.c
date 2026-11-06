#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <GL/gl.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#define WGL_CONTEXT_ES2_PROFILE_BIT_EXT 0x0004
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_EXTENSIONS 0x1F03

typedef HGLRC(WINAPI *CreateContextAttribs)(HDC, HGLRC, const int *);
typedef const char *(WINAPI *GetExtensionsString)(HDC);
typedef const unsigned char *(WINAPI *GetStringi)(unsigned, unsigned);
typedef void(WINAPI *GetIntegerv)(unsigned, int *);

static void describe(HDC dc, HGLRC rc, const char *what) {
    GetStringi gsi;
    GetIntegerv giv = (GetIntegerv)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetIntegerv");
    int n = 0, i, es3 = 0;
    if (!rc) {
        printf("%s: not available (error %lu)\n", what, GetLastError());
        return;
    }
    wglMakeCurrent(dc, rc);
    printf("%s:\n  vendor   %s\n  renderer %s\n  version  %s\n  glsl     %s\n", what, glGetString(GL_VENDOR),
           glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    gsi = (GetStringi)wglGetProcAddress("glGetStringi");
    if (gsi && giv) {
        giv(GL_NUM_EXTENSIONS, &n);
        for (i = 0; i < n; i++) {
            const char *e = (const char *)gsi(GL_EXTENSIONS, (unsigned)i);
            if (e && (!strcmp(e, "GL_ARB_ES3_compatibility") || !strcmp(e, "GL_ARB_ES3_1_compatibility") ||
                      !strcmp(e, "GL_OES_EGL_image") || !strcmp(e, "GL_EXT_texture_compression_s3tc") ||
                      !strcmp(e, "GL_KHR_texture_compression_astc_ldr") || !strcmp(e, "GL_ARB_buffer_storage"))) {
                printf("  has %s\n", e);
                es3 = 1;
            }
        }
        printf("  %d extensions\n", n);
    }
    (void)es3;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
}

int main(void) {
    WNDCLASSA wc = {0};
    HWND w;
    HDC dc;
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    HGLRC legacy;
    CreateContextAttribs create;
    GetExtensionsString exts;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "glprobe";
    RegisterClassA(&wc);
    w = CreateWindowA("glprobe", "glprobe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    dc = GetDC(w);
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    printf("legacy context: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    create = (CreateContextAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    exts = (GetExtensionsString)wglGetProcAddress("wglGetExtensionsStringARB");
    printf("WGL extensions: %s\n", exts ? exts(dc) : "(none)");
    wglMakeCurrent(NULL, NULL);
    if (create) {
        int es30[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 0, WGL_CONTEXT_PROFILE_MASK_ARB,
                      WGL_CONTEXT_ES2_PROFILE_BIT_EXT, 0};
        int es31[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 1, WGL_CONTEXT_PROFILE_MASK_ARB,
                      WGL_CONTEXT_ES2_PROFILE_BIT_EXT, 0};
        int core43[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                        WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        int core33[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                        WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        describe(dc, create(dc, NULL, es30), "OpenGL ES 3.0 context");
        describe(dc, create(dc, NULL, es31), "OpenGL ES 3.1 context");
        describe(dc, create(dc, NULL, core43), "GL 4.3 core context");
        describe(dc, create(dc, NULL, core33), "GL 3.3 core context");
    }
    wglDeleteContext(legacy);
    return 0;
}
