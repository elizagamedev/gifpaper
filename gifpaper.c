#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <time.h>
#include <gif_lib.h>

enum {
    STYLE_CENTER,
    STYLE_TILE,
    STYLE_STRETCH,
    STYLE_FIT,
    STYLE_FILL,
};

typedef struct {
    int x, y, w, h;
} Monitor;

int nMonitors = 0;
Monitor *monitors = NULL;

Display *display = NULL;
Screen *screen = NULL;
Window root = NULL;
int depth = 0;

GifFileType *gif = NULL;

unsigned char *data = NULL;
unsigned char *dataScaled = NULL;
GC gc = NULL;

int antialiasing;

#define PIXMAP_BUFF_SIZE 16
int iPixbuff = 0;
Pixmap pixbuff[PIXMAP_BUFF_SIZE] = {(Pixmap)NULL};

void deinit()
{
    for (int i = 0; i < PIXMAP_BUFF_SIZE; i++) {
        if (i != iPixbuff && pixbuff[i])
            XFreePixmap(display, pixbuff[i]);
    }
    free(data);
    free(dataScaled);
    free(monitors);
    if (gc)
        XFreeGC(display, gc);
    if (gif)
        DGifCloseFile(gif);
    if (display)
        XCloseDisplay(display);
}

int setWallpaper(Pixmap pixmap)
{
    Atom prop_root, prop_esetroot, type;
    int format;
    unsigned long length, after;
    unsigned char *data_root, *data_esetroot;

    prop_root = XInternAtom(display, "_XROOTPMAP_ID", True);
    prop_esetroot = XInternAtom(display, "ESETROOT_PMAP_ID", True);

    // The current pixmap being used in the atom; may or may not be spawned from this process
    Pixmap pixmapCurrent = NULL;

    // If someone other than us owns the wallpaper first, we must kill them.
    if (prop_root != None && prop_esetroot != None) {
        XGetWindowProperty(display, root, prop_root, 0L, 1L, False, AnyPropertyType, &type, &format, &length, &after, &data_root);
        if (type == XA_PIXMAP) {
            XGetWindowProperty(display, root, prop_esetroot, 0L, 1L, False, AnyPropertyType, &type, &format, &length, &after, &data_esetroot);
            if (data_root && data_esetroot) {
                if (type == XA_PIXMAP && *((Pixmap *)data_root) == *((Pixmap *)data_esetroot)) {
                    // Record it
                    pixmapCurrent = *((Pixmap *)data_root);
                }
            }
            XFree(data_esetroot);
        }
        XFree(data_root);
    }

    // Let's overwrite the properties with our pixmap now
    prop_root = XInternAtom(display, "_XROOTPMAP_ID", False);
    prop_esetroot = XInternAtom(display, "ESETROOT_PMAP_ID", False);

    if (prop_root == None || prop_esetroot == None) {
        fprintf(stderr, "error: creation of wallpaper property failed.");
        return 1;
    }

    // Change the properties for the compositor
    XChangeProperty(display, root, prop_root, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pixmap, 1);
    XChangeProperty(display, root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pixmap, 1);

    // Change the root window
    XSetWindowBackgroundPixmap(display, root, pixmap);
    XClearWindow(display, root);
    XFlush(display);

    // If this pixmap is ours, free it. Otherwise, kill the client.
    int kill = 0;
    if (pixmapCurrent) {
        kill = 1;
        for (int i = 0; i < PIXMAP_BUFF_SIZE; i++) {
            if (pixmapCurrent == pixbuff[i]) {
                kill = 0;
                break;
            }
        }
    }

    if (kill)
        XKillClient(display, pixmapCurrent);

    // Store the pixmap so we can free it later
    if (++iPixbuff >= PIXMAP_BUFF_SIZE)
        iPixbuff = 0;
    if (pixbuff[iPixbuff])
        XFreePixmap(display, pixbuff[iPixbuff]);
    pixbuff[iPixbuff] = pixmap;

    return 0;
}

void scale(unsigned char *dst, int dstWidth, int dstX, int dstY, int dstW, int dstH, unsigned char *src, int srcWidth, int srcX, int srcY, int srcW, int srcH)
{
    for (int y = 0; y < dstH; y++) {
        for (int x = 0; x < dstW; x++) {
            int indexDst = ((dstY + y) * dstWidth + (dstX + x)) * 4;
            if (antialiasing) {
                float x2f = srcX + (x * srcW / (float)dstW);
                float y2f = srcY + (y * srcH / (float)dstH);
                int x2 = (int)x2f;
                int y2 = (int)y2f;
                float dx = x2f - x2;
                float dy = y2f - y2;

                int index1 = (y2 * srcWidth + x2) * 4;
                int index2 = (y2 * srcWidth + (x2 + 1)) * 4;
                int index3 = ((y2 + 1) * srcWidth + x2) * 4;
                int index4 = ((y2 + 1) * srcWidth + (x2 + 1)) * 4;

                for (int i = 0; i < 3; i++)
                    dst[indexDst + i] = (int)((float)src[index1 + i] * (1.0f - dx) * (1.0f - dy) + (float)src[index2 + i] * (dx) * (1.0f - dy) + (float)src[index3 + i] * (1.0f - dx) * (dy) + (float)src[index4 + i] * (dx) * (dy));
            } else {
                int indexSrc = ((srcY + (y * srcH / dstH)) * srcWidth + (srcX + (x * srcW / dstW))) * 4;
                dst[indexDst + 0] = src[indexSrc + 0];
                dst[indexDst + 1] = src[indexSrc + 1];
                dst[indexDst + 2] = src[indexSrc + 2];
            }
        }
    }
}

unsigned long getTicks()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        goto usage;

    int style;

    // Determine wallpaper style first
    if (!strcmp(argv[1], "center"))
        style = STYLE_CENTER;
    else if (!strcmp(argv[1], "tile"))
        style = STYLE_TILE;
    else if (!strcmp(argv[1], "stretch"))
        style = STYLE_STRETCH;
    else if (!strcmp(argv[1], "fit"))
        style = STYLE_FIT;
    else if (!strcmp(argv[1], "fill"))
        style = STYLE_FILL;
    else
        goto usage;

    // Antialiasing?
    if (!strcmp(argv[2], "off"))
        antialiasing = 0;
    else if (!strcmp(argv[2], "on"))
        antialiasing = 1;
    else
        goto usage;

    // INITIALIZE

    // Ready the display
    if (!(display = XOpenDisplay(NULL))) {
        fprintf(stderr, "error: could not open X display.\n");
        return 1;
    }

    // Get the xrandr screen configuration
    XRRScreenResources *randrScreen = XRRGetScreenResources(display, DefaultRootWindow(display));
    monitors = malloc(sizeof(Monitor) * randrScreen->ncrtc);
    for (int i = 0; i < randrScreen->ncrtc; i++) {
        XRRCrtcInfo *randrCrtcInfo = XRRGetCrtcInfo(display, randrScreen, randrScreen->crtcs[i]);
        if (randrCrtcInfo->width && randrCrtcInfo->height) {
            monitors[nMonitors].x = randrCrtcInfo->x;
            monitors[nMonitors].y = randrCrtcInfo->y;
            monitors[nMonitors].w = randrCrtcInfo->width;
            monitors[nMonitors].h = randrCrtcInfo->height;
            nMonitors++;
        }
        XRRFreeCrtcInfo(randrCrtcInfo);
    }
    XRRFreeScreenResources(randrScreen);

    // Cleanup code
    atexit(deinit);

    // Ready X11 stuff
    XSetCloseDownMode(display, RetainPermanent);
    screen = DefaultScreenOfDisplay(display);
    root = DefaultRootWindow(display);
    depth = DefaultDepthOfScreen(screen);
    if (depth < 24) {
        fprintf(stderr, "error: color depth must be at least 24.\n");
        return 1;
    }

    // Load the gif
    gif = DGifOpenFileName(argv[3], NULL);
    if (!gif || DGifSlurp(gif) != GIF_OK || gif->ImageCount <= 1) {
        fprintf(stderr, "error: %s: not an animated gif.\n", argv[3]);
        goto usage;
    }

    // Buffer for RGBA image data
    data = malloc(gif->SWidth * gif->SHeight * 4);
    memset(data, 0, gif->SWidth * gif->SHeight * 4);

    // Rescaled wallpaper which encompasses the entire x11 screen;
    // not necessary for tiled mode
    if (style != STYLE_TILE) {
        dataScaled = malloc(screen->width * screen->height * 4);
        memset(dataScaled, 0, screen->width * screen->height * 4);
    }

    // Graphics context for converting image
    gc = XCreateGC(display, root, 0, 0);

    unsigned long ticks = getTicks();

    int i = 0;
    for (;;) {
        unsigned long wait = 100;
        int transparent = -1;
        SavedImage *si = &gif->SavedImages[i];

        // Get the frame wait/transparent color, combine/replace
        for (int j = 0; j < si->ExtensionBlockCount; j++) {
            if (si->ExtensionBlocks[j].Function == GRAPHICS_EXT_FUNC_CODE) {
                GraphicsControlBlock gcb;
                DGifExtensionToGCB(si->ExtensionBlocks[j].ByteCount, si->ExtensionBlocks[j].Bytes, &gcb);
                wait = gcb.DelayTime;
                transparent = gcb.TransparentColor;

                if (gcb.DisposalMode == DISPOSE_BACKGROUND)
                    memset(data, 255, gif->SWidth * gif->SHeight * 4);
                break;
            }
        }

        // Get the local or global colormap
        ColorMapObject *colormap;
        if (si->ImageDesc.ColorMap)
            colormap = si->ImageDesc.ColorMap;
        else
            colormap = gif->SColorMap;

        int bottom = si->ImageDesc.Top + si->ImageDesc.Height;
        int right = si->ImageDesc.Left + si->ImageDesc.Width;

        // Render GIF frame in BGRA
        for (int y = si->ImageDesc.Top; y < bottom; y++) {
            for (int x = si->ImageDesc.Left; x < right; x++) {
                int index = si->RasterBits[(y - si->ImageDesc.Top) * si->ImageDesc.Width + (x - si->ImageDesc.Left)];
                if (index != transparent) {
                    GifColorType color = colormap->Colors[index];
                    data[(y * gif->SWidth + x) * 4 + 0] = color.Blue;
                    data[(y * gif->SWidth + x) * 4 + 1] = color.Green;
                    data[(y * gif->SWidth + x) * 4 + 2] = color.Red;
                }
            }
        }

        switch (style) {
        case STYLE_CENTER: {
            for (int j = 0; j < nMonitors; j++) {
                for (int y = 0; y < gif->SHeight; y++) {
                    for (int x = 0; x < gif->SWidth; x++) {
                        int indexScaled = ((y + monitors[j].y + (monitors[j].h - gif->SHeight) / 2) * screen->width + (x + monitors[j].x + (monitors[j].w - gif->SWidth) / 2)) * 4;
                        int index = (y * gif->SWidth + x) * 4;
                        dataScaled[indexScaled + 0] = data[index + 0];
                        dataScaled[indexScaled + 1] = data[index + 1];
                        dataScaled[indexScaled + 2] = data[index + 2];
                    }
                }
            }
        } break;

        case STYLE_STRETCH: {
            for (int j = 0; j < nMonitors; j++)
                scale(dataScaled, screen->width, monitors[j].x, monitors[j].y, monitors[j].w, monitors[j].h, data, gif->SWidth, 0, 0, gif->SWidth, gif->SHeight);
        } break;

        case STYLE_TILE: {
            // First we render to the gif-sized pixmap, then we tile it onto a new pixmap.
            Pixmap pixmap1 = XCreatePixmap(display, root, gif->SWidth, gif->SHeight, depth);
            XImage *img = XCreateImage(display, CopyFromParent, depth, ZPixmap, 0, (char *)data, gif->SWidth, gif->SHeight, 32, 0);
            XPutImage(display, pixmap1, gc, img, 0, 0, 0, 0, gif->SWidth, gif->SHeight);
            img->data = NULL;
            XDestroyImage(img);

            // Now tile it onto the real pixmap
            Pixmap pixmap = XCreatePixmap(display, root, screen->width, screen->height, depth);
            XGCValues gc2values;
            gc2values.fill_style = FillTiled;
            gc2values.tile = pixmap1;
            GC gc2 = XCreateGC(display, pixmap, GCFillStyle | GCTile, &gc2values);
            XFillRectangle(display, pixmap, gc2, 0, 0, screen->width, screen->height);
            XFreeGC(display, gc2);
            // XSync(display, False);
            XFreePixmap(display, pixmap1);

            if (setWallpaper(pixmap))
                return 1;
        } break;

        case STYLE_FIT: {
            for (int j = 0; j < nMonitors; j++) {
                int dstX = monitors[j].x;
                int dstY = monitors[j].y;
                int dstW = monitors[j].w;
                int dstH = monitors[j].h;
                if (monitors[j].h * gif->SWidth / gif->SHeight > monitors[j].w) {
                    // Center the image vertically
                    dstH = monitors[j].w * gif->SHeight / gif->SWidth;
                    dstY += (monitors[j].h - dstH) / 2;
                } else {
                    // Center the image horizontally
                    dstW = monitors[j].h * gif->SWidth / gif->SHeight;
                    dstX += (monitors[j].w - dstW) / 2;
                }

                scale(dataScaled, screen->width, dstX, dstY, dstW, dstH, data, gif->SWidth, 0, 0, gif->SWidth, gif->SHeight);
            }
        } break;

        case STYLE_FILL: {
            for (int j = 0; j < nMonitors; j++) {
                int srcX = 0;
                int srcY = 0;
                int srcW = gif->SWidth;
                int srcH = gif->SHeight;
                if (gif->SHeight * monitors[j].w / monitors[j].h > gif->SWidth) {
                    // Center the source frame vertically
                    srcH = gif->SWidth * monitors[j].h / monitors[j].w;
                    srcY = (gif->SHeight - srcH) / 2;
                } else {
                    // Center the source frame horizontally
                    srcW = gif->SHeight * monitors[j].w / monitors[j].h;
                    srcX += (gif->SWidth - srcW) / 2;
                }

                scale(dataScaled, screen->width, monitors[j].x, monitors[j].y, monitors[j].w, monitors[j].h, data, gif->SWidth, srcX, srcY, srcW, srcH);
            }
        } break;
        }

        if (style != STYLE_TILE) {
            // Convert to a pixmap
            Pixmap pixmap = XCreatePixmap(display, root, screen->width, screen->height, depth);
            XImage *img = XCreateImage(display, CopyFromParent, depth, ZPixmap, 0, (char *)dataScaled, screen->width, screen->height, 32, 0);
            XPutImage(display, pixmap, gc, img, 0, 0, 0, 0, screen->width, screen->height);
            img->data = NULL;
            XDestroyImage(img);

            if (setWallpaper(pixmap))
                return 1;
        }

        if (++i >= gif->ImageCount)
            i = 0;

        // Regulate framerate
        unsigned long ticksDelta = getTicks() - ticks;
        if (ticksDelta < 10000 * wait)
            usleep(10000 * wait - ticksDelta);
        ticks = getTicks();
    }

    return 0;

usage:
    fprintf(stderr, "usage: %s <center|tile|stretch|fit|fill> <anti-aliasing off|on> <animated gif image>\n", argv[0]);
    return 1;
}
