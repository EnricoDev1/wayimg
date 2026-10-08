#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include "protocol.h"
#include "wlr-layer-shell-unstable-v1.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define UNUSED(x) ((void)x)

#define info(fmt, ...) fprintf(stderr, "[+] " fmt "\n", ##__VA_ARGS__)
#define err(fmt, ...)  fprintf(stderr, "[!] error: " fmt "\n", ##__VA_ARGS__)
#define fatal(fmt, ...)                                         \
    do {                                                        \
        fprintf(stderr, "[!] fatal: " fmt "\n", ##__VA_ARGS__); \
        exit(1);                                                \
    } while (0)

struct image {
    uint8_t *pixels;
    int width;
    int heigth;
    int channels;
};

struct state {
    /* globals */
    struct wl_registry *wl_registry;
    struct wl_display *wl_display;
    struct wl_compositor *wl_compositor;
    struct wl_shm *wl_shm;
    struct zwlr_layer_shell_v1 *zwlr_layer_shell;

    /* objects */
    struct wl_surface *wl_surface;
    struct wl_output *wl_output;
    struct zwlr_layer_surface_v1 *zwlr_layer_surface;

    struct image img;

    int running;
} state = {0};

static void
rand_filename(char *buf)
{
    memcpy(buf, "/wl_shm-XXXXXX", 15);
    uint8_t rnd[6];
    getrandom(rnd, sizeof rnd, 0);
    for (size_t i = 0; i < 6; ++i)
        buf[i + 8] = (rnd[i] % 26) + 'a';
}

static int
create_shm_file(void)
{
    int retries = 100;
    do {
        char filename[15] = {0};
        rand_filename(filename);

        int fd = shm_open(filename, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(filename);
            return fd;
        }
    } while (retries--, errno == EEXIST && retries > 0);

    return -1;
}

int
allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0)
        return -1;

    if (ftruncate(fd, size) < 0)
        return -1;

    return fd;
}

static struct wl_buffer *
draw_frame()
{
    const int stride = state.img.width * 4;
    const int shm_pool_size = state.img.heigth * stride * 2;

    int fd = allocate_shm_file(shm_pool_size);
    if (fd < 0)
        fatal("Unable to create shared buffer");

    uint8_t *pool_data =
        mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (pool_data == MAP_FAILED)
        fatal("Unable to mmap pool data");

    struct wl_shm_pool *pool =
        wl_shm_create_pool(state.wl_shm, fd, shm_pool_size);

    int idx = 0;
    int offset = state.img.heigth * stride * idx;
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, offset, state.img.width, state.img.heigth, stride,
        WL_SHM_FORMAT_XRGB8888);

    if (buffer == NULL)
        fatal("Unable to create pool buffer");

    memcpy(pool_data, state.img.pixels, state.img.width * state.img.heigth * 4);

    close(fd);
    wl_shm_pool_destroy(pool);
    munmap(pool_data, shm_pool_size);

    return buffer;
}

static void
zwlr_surface_configure(void *data,
                       struct zwlr_layer_surface_v1 *zwlr_layer_surface,
                       uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface, serial);

    wl_surface_attach(state.wl_surface, draw_frame(), 0, 0);
    wl_surface_damage(state.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(state.wl_surface);

    UNUSED(width);
    UNUSED(height);
    UNUSED(data);
}

static void
zwlr_surface_closed(void *data,
                    struct zwlr_layer_surface_v1 *zwlr_layer_surface)
{
    zwlr_layer_surface_v1_destroy(zwlr_layer_surface);
    state.zwlr_layer_surface = NULL;

    wl_surface_destroy(state.wl_surface);
    state.wl_surface = NULL;

    state.running = 0;

    UNUSED(data);
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_listener =
    {.configure = zwlr_surface_configure, .closed = zwlr_surface_closed};

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                       const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state.wl_compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, version);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state.wl_shm =
            wl_registry_bind(registry, name, &wl_shm_interface, version);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state.zwlr_layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, version);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        state.wl_output = wl_registry_bind(state.wl_registry, name,
                                           &wl_output_interface, version);
    }
    UNUSED(data);
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
    UNUSED(data);
    UNUSED(registry);
    UNUSED(name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void
load_image(const char *path)
{
    state.img.pixels = stbi_load(path, &state.img.width, &state.img.heigth,
                                 &state.img.channels, 4);
    if (state.img.pixels == NULL)
        fatal("Unable to load image");
}

static void
wl_setup()
{
    state.wl_display = wl_display_connect(NULL);
    if (!state.wl_display)
        fatal("Unable to connect to display");
    info("Successfully connected to wayland display");

    state.wl_registry = wl_display_get_registry(state.wl_display);
    if (!state.wl_registry)
        fatal("Unable to get registry from display");
    info("Successfully got display registry");

    wl_registry_add_listener(state.wl_registry, &registry_listener, NULL);
    wl_display_roundtrip(state.wl_display);

    if (!state.wl_compositor)
        fatal("Compositor does not support wl_compositor");

    if (!state.wl_shm)
        fatal("Compositor does not support wl_shm");

    if (!state.zwlr_layer_shell)
        fatal("Compositor does not support xwlr_layer_shell");

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    if (!state.wl_surface)
        fatal("Unable to create surface");
    info("Successfully created surface");
}

static void
zwlr_setup()
{
    state.zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.zwlr_layer_shell, state.wl_surface, state.wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "test");

    zwlr_layer_surface_v1_add_listener(state.zwlr_layer_surface,
                                       &zwlr_layer_surface_listener, NULL);

    zwlr_layer_surface_v1_set_size(state.zwlr_layer_surface, state.img.width,
                                   state.img.heigth);
    zwlr_layer_surface_v1_set_anchor(state.zwlr_layer_surface, 0);

    wl_surface_commit(state.wl_surface);
}

int
main(int argc, char **argv)
{
    if (argc != 2)
        fatal("Usage: %s <image_path>", argv[0]);

    load_image(argv[1]);
    wl_setup();
    zwlr_setup();

    info("Successfully rendered image");
    state.running = 1;

    while (state.running && wl_display_dispatch(state.wl_display) != -1)
        ;

    stbi_image_free(state.img.pixels);

    return 0;
}
