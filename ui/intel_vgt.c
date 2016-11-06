/*
 * QEMU Intel GVT-g indirect display support
 *
 * Copyright (c) Intel
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#define __user
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <SDL.h>
#include <SDL_syswm.h>

//#define EGL_EGLEXT_PROTOTYPES
#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libudev.h>

#include "sysemu/sysemu.h"
#include "qemu-common.h"
#include "ui/console.h"
#include "hw/xen/xen.h"

static GLuint textureId = 0;
static GLuint cursortextureId = 0;
static GLfloat widthoverstride = 0.;
static int fbWidth = 0;
static int fbHeight = 0;

static int vm_pipe = UINT_MAX;
static bool dma_buf_mode = false;

static int winWidth = 1024;
static int winHeight = 768;

static int fd = 0;

const int PRIMARY_LIST_LEN = 3;
const int CURSOR_LIST_LEN = 4;

static uint32_t current_primary_fb_addr = 0;
static uint32_t current_cursor_fb_addr = 0;
static GLuint current_textureId;
static GLuint current_cursor_textureId;
static bool cursor_ready = false;

typedef struct buffer_rec{
    uint32_t start;
    GLuint textureId;
    int age;
    uint8_t tiled;
    uint32_t size;
} buffer_rec;

typedef struct buffer_list{
    struct buffer_rec *l;
    int len;
	bool firstcreated;
} buffer_list;

struct buffer_list primary_list;
struct buffer_list cursor_list;

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface sur;

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

struct drm_i915_gem_vgtbuffer {
        __u32 vmid;
        __u32 plane_id;
#define I915_VGT_PLANE_PRIMARY 1
#define I915_VGT_PLANE_SPRITE 2
#define I915_VGT_PLANE_CURSOR 3
        __u32 pipe_id;
        __u32 phys_pipe_id;
        __u8  enabled;
        __u8  tiled;
        __u32 bpp;
        __u32 hw_format;
        __u32 drm_format;
        __u32 start;
        __u32 x_pos;
        __u32 y_pos;
        __u32 x_offset;
        __u32 y_offset;
        __u32 size;
        __u32 width;
        __u32 height;
        __u32 stride;
        __u64 user_ptr;
        __u32 user_size;
        __u32 flags;
#define I915_VGTBUFFER_READ_ONLY (1<<0)
#define I915_VGTBUFFER_QUERY_ONLY (1<<1)
#define I915_VGTBUFFER_CHECK_CAPABILITY (1<<2)
#define I915_VGTBUFFER_UNSYNCHRONIZED 0x80000000
        /**
         * Returned handle for the object.
         *
         * Object handles are nonzero.
         */
        __u32 handle;
};

#define DRM_I915_GEM_VGTBUFFER          0x36
#define DRM_IOCTL_I915_GEM_VGTBUFFER    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_VGTBUFFER, struct drm_i915_gem_vgtbuffer)

static void drawRect(void)
{
    /*
     * The framebuffer is top-down flipped.
     * Set flipped texture coords to correct it.
     */

    glBegin(GL_QUADS);
    glTexCoord2f(0, 1);  glVertex3f(-1, -1, 0);
    glTexCoord2f(1, 1);  glVertex3f(1, -1, 0);
    glTexCoord2f(1, 0);  glVertex3f(1, 1, 0);
    glTexCoord2f(0, 0);  glVertex3f(-1, 1, 0);
    glEnd();
}

static void draw(int x, int y)
{
    float fx, fy;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    /* Draw the primary plane in a rectangle. */
    glBindTexture(GL_TEXTURE_2D, current_textureId);
    glDisable(GL_BLEND);
    drawRect();

    if (cursor_ready) {
        /* Calcuate the cursor position. */
        fx = -1 + 2 * (float)(x + 32) / (float)fbWidth;
        fy = 1 - 2 * (float)(y + 32) / (float)fbHeight;
        glTranslatef(fx, fy, 0);
        glScalef(64 / (float)fbWidth, 64 / (float)fbHeight, 1);

        /* Draw the cursor plane in another rectangle */
        glBindTexture(GL_TEXTURE_2D, current_cursor_textureId);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        drawRect();
    }

    glFlush();
}

/* new window size or exposure */
static void reshape(int width, int height)
{
    glViewport(0, 0, width, height);
}

static int find_rec(struct buffer_list *l, struct drm_i915_gem_vgtbuffer
                    *vgtbuffer)
{
    int i, r;

    r = -1;
    for (i = 0; i < l->len; i++) {
        if (l->l[i].start == vgtbuffer->start &&
            l->l[i].tiled == vgtbuffer->tiled &&
            l->l[i].size == vgtbuffer->size) {
            r = i;
            break;
		} else if ((l->l[i].tiled != vgtbuffer->tiled) ||
					(l->l[i].size != vgtbuffer->size)) {
			l->firstcreated = false;
			break;
        }
    }

    return r;
}

static int oldest_rec(struct buffer_list *l)
{
    int i = 1, a = l->l[0].age, r = 0;

    for (i = 1; i < l->len; i++) {
        if (l->l[i].age > a) {
            a = l->l[i].age;
            r = i;
        }
    }

    return r;
}

static void age_list(struct buffer_list *l)
{
    int i;

    for (i = 0; i < l->len; i++) {
        if (l->l[i].age != INT_MAX) {
            l->l[i].age++;
        }
    }
}

static void clear_rec(struct buffer_list *l, int i)
{
    l->l[i].age = INT_MAX;
    l->l[i].start = 0;
    l->l[i].textureId = 0;
}

static void create_cursor_buffer(void)
{
    int width = 0, height = 0, stride = 0;
    struct drm_i915_gem_vgtbuffer vcreate;
	int i, r;
    EGLImageKHR namedimage;
    unsigned long name;

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    width = 64;
    height = 64;
    stride = width * 4;
    vcreate.vmid = get_guest_domid();
    vcreate.plane_id = I915_VGT_PLANE_CURSOR;
    vcreate.phys_pipe_id = vm_pipe;

    drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);

    current_cursor_fb_addr = vcreate.start;

	if (!cursor_list.firstcreated) {
		for (i = 0; i < cursor_list.len; i++) {
			clear_rec(&cursor_list, i);
			cursor_list.l[i].tiled = 0;
			cursor_list.l[i].size  = vcreate.size;
		}
		cursor_list.firstcreated = true;
	}

    r = oldest_rec(&cursor_list);
    glGenTextures(1, &cursortextureId);
    glBindTexture(GL_TEXTURE_2D, cursortextureId);

    cursor_list.l[r].start = current_cursor_fb_addr;
    cursor_list.l[r].textureId = cursortextureId;
    cursor_list.l[r].age = 0;
    cursor_list.l[r].tiled = 0;
    cursor_list.l[r].size = vcreate.size;
    current_cursor_textureId = cursortextureId;

    if (dma_buf_mode) {
        struct drm_prime_handle prime;
        prime.handle = vcreate.handle;
        prime.flags = DRM_CLOEXEC;
        drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
        name = prime.fd;
    } else {
        struct drm_gem_flink flink;
        flink.handle = vcreate.handle;
        drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink);
        name = flink.name;
    }

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

    if (dma_buf_mode) {
        EGLint attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT,
            vcreate.drm_format > 0 ? vcreate.drm_format : DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, (int)name,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
            EGL_NONE
        };
        namedimage = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                       NULL, attribs);
    } else {
        /* Only EGL_DRM_BUFFER_FORMAT_ARGB32_MESA format is supported so if
           some app(like Heaven) use their own buffer format rather than
           the ARGB32 then the color display maybe go wrong.
        */
        EGLint attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_DRM_BUFFER_STRIDE_MESA, stride / 4,
            EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
            EGL_NONE
        };
        namedimage = eglCreateImageKHR(dpy, ctx, EGL_DRM_BUFFER_MESA,
                                       (EGLClientBuffer)name, attribs);
    }
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, namedimage);
    eglDestroyImageKHR(dpy, namedimage);

    if (dma_buf_mode) {
        close(name);
    }
}

static void create_primary_buffer(void)
{
    struct drm_i915_gem_vgtbuffer vcreate;
    int width = 0, height = 0 ,stride = 0;
	int i, r;
    EGLImageKHR namedimage;
    unsigned long name;

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    width = 0;
    height = 0;
    stride = width * 4;
    vcreate.vmid = get_guest_domid();
    vcreate.plane_id = I915_VGT_PLANE_PRIMARY;
    vcreate.phys_pipe_id = vm_pipe;

    drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    width = vcreate.width;
    height = vcreate.height;
    stride = vcreate.stride;
    widthoverstride = (float)width / (float)(stride / 4);
    fbWidth = width;
    fbHeight = height;
    current_primary_fb_addr = vcreate.start;

	if (!primary_list.firstcreated) {
		for (i = 0; i < primary_list.len; i++) {
			clear_rec(&primary_list, i);
			primary_list.l[i].tiled = vcreate.tiled;
			primary_list.l[i].size  = vcreate.size;
		}
		primary_list.firstcreated = true;
	}

    r = oldest_rec(&primary_list);

    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    primary_list.l[r].start = current_primary_fb_addr;
    primary_list.l[r].textureId = textureId;
    primary_list.l[r].age = 0;
    primary_list.l[r].tiled = vcreate.tiled;
    primary_list.l[r].size = vcreate.size;
    current_textureId = textureId;

    if (dma_buf_mode) {
        struct drm_prime_handle prime;
        prime.handle = vcreate.handle;
        prime.flags = DRM_CLOEXEC;
        drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
        name = prime.fd;
    } else {
        struct drm_gem_flink flink;
        flink.handle = vcreate.handle;
        drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink);
        name = flink.name;
    }

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

    if (dma_buf_mode) {
        EGLint attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT,
            vcreate.drm_format > 0 ? vcreate.drm_format : DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, name,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
            EGL_NONE
        };
        namedimage = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                       NULL, attribs);
    } else {
        /* Only EGL_DRM_BUFFER_FORMAT_ARGB32_MESA format is supported so if
           some app(like Heaven) use their own buffer format rather than
           the ARGB32 then the color display maybe go wrong.
        */
        EGLint attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_DRM_BUFFER_STRIDE_MESA, stride / 4,
            EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
            EGL_NONE
        };
        namedimage = eglCreateImageKHR(dpy, ctx, EGL_DRM_BUFFER_MESA,
                                       (EGLClientBuffer)name, attribs);
    }
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, namedimage);
    eglDestroyImageKHR(dpy, namedimage);

    if (dma_buf_mode) {
        close(name);
    }
}

static void check_for_new_primary_buffer(void)
{
    struct drm_i915_gem_vgtbuffer vcreate;
	int r = 0;
    uint32_t start;

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.plane_id = I915_VGT_PLANE_PRIMARY;
    vcreate.vmid = get_guest_domid();
    vcreate.phys_pipe_id = vm_pipe;
    vcreate.flags = I915_VGTBUFFER_QUERY_ONLY;

    r = drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    if (r != 0 || vcreate.start == 0) {
        current_primary_fb_addr = 0;
		primary_list.firstcreated = false;
        return;
    }

    start = vcreate.start;
    if ((start != current_primary_fb_addr)) {
        r = find_rec(&primary_list, &vcreate);
        age_list(&primary_list);

        if (r >= 0) {
            primary_list.l[r].age = 0;
            current_textureId = primary_list.l[r].textureId;
            current_primary_fb_addr = start;
        } else {
            create_primary_buffer();
        }
    }
}

static void check_for_new_cursor_buffer(int *x, int *y)
{
    struct drm_i915_gem_vgtbuffer vcreate;
	int r;
    uint32_t cursorstart;

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.vmid = get_guest_domid();
    vcreate.plane_id = I915_VGT_PLANE_CURSOR;
    vcreate.phys_pipe_id = vm_pipe;
    vcreate.flags = I915_VGTBUFFER_QUERY_ONLY;
    r = drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    if (r != 0 || vcreate.start <= 0) {
        current_cursor_fb_addr = 0;
		cursor_list.firstcreated = false;
        cursor_ready = false;
        return;
    }
    cursorstart = vcreate.start;
    cursor_ready = true;

    if (cursorstart != current_cursor_fb_addr) {
        current_cursor_fb_addr = cursorstart;
        r = find_rec(&cursor_list, &vcreate);
        age_list(&cursor_list);
        if (r >= 0) {
            cursor_list.l[r].age = 0;
            current_cursor_textureId = cursor_list.l[r].textureId;
        } else {
            create_cursor_buffer();
        }
    }

    *x = vcreate.x_pos;
    *y = vcreate.y_pos;
}

/********** UDEV Part ************/
static struct udev * udev;
static struct udev_monitor * mon;

static int udev_init(void) {
    int ret;

    udev = udev_new();
    if (!udev) {
        return -1;
    }

    mon = udev_monitor_new_from_netlink(udev, "kernel");
    if (!mon) {
        ret = -2;
        goto release_udev;
    }

    ret = udev_monitor_filter_add_match_subsystem_devtype(mon, "vgt", NULL);
    if (ret < 0) {
        ret = -3;
        goto release_mon;
    }

    ret = udev_monitor_enable_receiving(mon);
    if (ret < 0) {
        ret = -4;
        goto release_mon;
    }

    return 0;

release_mon:
    udev_monitor_unref(mon);
release_udev:
    udev_unref(udev);

    return ret;
}

static void udev_destroy(void) {
    udev_monitor_unref(mon);
    udev_unref(udev);
}

static int check_vgt_uevent(void) {
    int ret = 0;
    const char * value;
    struct udev_device * dev;

    dev = udev_monitor_receive_device(mon);
    if (!dev) {
        goto out;
    }

    value = udev_device_get_property_value(dev, "VGT_DISPLAY_READY");
    if (!value || strcmp(value, "1")) {
        goto out;
    }

    value = udev_device_get_property_value(dev, "VMID");
    if (!value || 1 != sscanf(value, "%d", &ret) || ret != get_guest_domid()) {
        goto out;
    }
    ret = 1;

out:
    udev_device_unref(dev);

    return ret;
}

/********** SDL Part ************/
static DisplayChangeListener *dcl;

extern int last_vm_running;

void sdl_update_caption(void);
void handle_keydown(SDL_Event *ev);
void handle_keyup(SDL_Event *ev);
void handle_mousemotion(SDL_Event *ev);
void handle_mousebutton(SDL_Event *ev);
void handle_activation(SDL_Event *ev);

static void intel_vgt_refresh(DisplayChangeListener *dcl)
{
    SDL_Event event;
    int x =0, y = 0;

    if (last_vm_running != runstate_is_running()) {
        last_vm_running = runstate_is_running();
        sdl_update_caption();
    }

    SDL_EnableUNICODE(!qemu_console_is_graphic(NULL));

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_VIDEORESIZE:
            SDL_SetVideoMode(event.resize.w, event.resize.h, 16,
                             SDL_OPENGL | SDL_RESIZABLE);
            reshape(event.resize.w, event.resize.h);
            break;
        case SDL_KEYDOWN:
            handle_keydown(&event);
            break;
        case SDL_KEYUP:
            handle_keyup(&event);
            break;
        case SDL_QUIT:
            if (!no_quit) {
                no_shutdown = 0;
                qemu_system_shutdown_request();
            }
            break;
        case SDL_MOUSEMOTION:
            handle_mousemotion(&event);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            handle_mousebutton(&event);
            break;
        case SDL_ACTIVEEVENT:
            handle_activation(&event);
            break;
        default:
            break;
        }
    }

    check_for_new_primary_buffer();
    check_for_new_cursor_buffer(&x, &y);
    draw(x, y);
    eglSwapBuffers(dpy, sur);
}

static void vgt_init(void)
{
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
    eglGetProcAddress("glEGLImageTargetTexture2DOES");
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
    eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
    eglGetProcAddress("eglDestroyImageKHR");
    int i;
    EGLint attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, /* may be changed later */
    EGL_RED_SIZE, 1,
    EGL_GREEN_SIZE, 1,
    EGL_BLUE_SIZE, 1,
    EGL_DEPTH_SIZE, 1,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE
    };
    EGLint num_conf;
    SDL_SysWMinfo info;
    EGLConfig conf;

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetVideoMode(winWidth, winHeight, 32, SDL_OPENGL | SDL_RESIZABLE);

    SDL_VERSION(&info.version);
    SDL_GetWMInfo(&info);

    eglBindAPI(EGL_OPENGL_API);
    dpy = eglGetDisplay(info.info.x11.display);
    eglInitialize(dpy, NULL, NULL);

    eglChooseConfig(dpy, attribs, &conf, 1, &num_conf);
    ctx = eglCreateContext(dpy, conf, EGL_NO_CONTEXT, NULL);
    sur = eglCreateWindowSurface(dpy, conf, info.info.x11.window, NULL);
    eglMakeCurrent(dpy, sur, sur, ctx);

    primary_list.l = malloc(PRIMARY_LIST_LEN*sizeof(struct buffer_rec));
    if (primary_list.l == NULL) {
        fprintf(stderr, "allocate primary list failed\n");
        exit(1);
    }
    primary_list.len = PRIMARY_LIST_LEN;
	primary_list.firstcreated = false;
    cursor_list.l = malloc(CURSOR_LIST_LEN*sizeof(struct buffer_rec));
    if (cursor_list.l == NULL) {
        fprintf(stderr, "allocate cursor list failed\n");
        exit(1);
    }
    cursor_list.len = CURSOR_LIST_LEN;
	cursor_list.firstcreated = false;
    for (i = 0; i < primary_list.len; i++) {
        primary_list.l[i].start = 0;
        primary_list.l[i].textureId = 0;
        primary_list.l[i].age = INT_MAX;
    }
    for (i = 0; i < cursor_list.len; i++) {
        cursor_list.l[i].start = 0;
        cursor_list.l[i].textureId = 0;
        cursor_list.l[i].age = INT_MAX;
    }

    glEnable(GL_TEXTURE_2D);
    glClearColor(0, 0, 0, 0);
    glColor3f(1, 1, 1);

    glMatrixMode(GL_PROJECTION);
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);

    reshape(winWidth, winHeight);
    }

static void intel_vgt_detect(DisplayChangeListener *dcl);
static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name = "intel_vgt",
    .dpy_refresh = intel_vgt_detect,
};

static const DisplayChangeListenerOps dcl_ops2 = {
    .dpy_name = "intel_vgt2",
    .dpy_refresh = intel_vgt_refresh,
};

static void intel_vgt_detect(DisplayChangeListener *dcl)
{
    if (check_vgt_uevent()) {
        udev_destroy();
        unregister_displaychangelistener(dcl->next.le_next);
        vgt_init();
        dcl->ops = &dcl_ops2;
    }
}

static bool check_egl(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    bool ret = true;
    char *egl_ext;

    if (!eglInitialize(d, NULL, NULL)) {
        fprintf(stderr, "eglInitialize failed\n");
        exit(1);
    }
    egl_ext = (char *)eglQueryString(d, EGL_EXTENSIONS);
    if (!strstr(egl_ext, "EGL_KHR_image_base")) {
        fprintf(stderr, "no egl extensions found. Intel GVT-g indirect display will be disabled\n");
        ret = false;
    } else {
        fprintf(stderr, "egl extensions found. Intel GVT-g indirect display will be enabled\n");
        if (strstr(egl_ext, "EGL_EXT_image_dma_buf_import") &&
            strstr(egl_ext, "EGL_MESA_image_dma_buf_export")) {
            dma_buf_mode = true;
            fprintf(stderr, "Use dma-buf to get guest framebuffer\n");
        } else {
            dma_buf_mode = false;
            fprintf(stderr, "Use flink to get guest framebuffer\n");
        }
    }
    eglTerminate(d);

    return ret;
}

bool intel_vgt_check_composite_display(void)
{
    struct drm_i915_gem_vgtbuffer vcreate;

    fd = open("/dev/dri/card0", O_RDWR);
    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.flags = I915_VGTBUFFER_CHECK_CAPABILITY;
    if (!drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate)) {
        return true;
    } else {
        return false;
    }
}

void intel_vgt_display_init(DisplayState *ds, int full_screen, int no_frame)
{
    sdl_display_init(ds, full_screen, no_frame);
    if (!check_egl()) {
        return;
    }
    udev_init();
    dcl = g_malloc0(sizeof(DisplayChangeListener));
    dcl->ops = &dcl_ops;
    register_displaychangelistener(dcl);
}
