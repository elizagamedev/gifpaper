#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <gif_lib.h>

enum {
    STYLE_CENTER,
    STYLE_TILE,
    STYLE_STRETCH,
    STYLE_FIT,
    STYLE_FILL,
};

typedef struct {
    int wait; // Hundredths of a second
    Pixmap pixmap;
} Frame;

typedef struct {
    int x, y, w, h;
} Monitor;

int frame = -1;

int nFrames = 0;
Frame *frames = NULL;

int nMonitors = 0;
Monitor *monitors = NULL;

Display *display = NULL;
Screen *screen = NULL;
Window root = NULL;
int depth = 0;

int antialiasing;

void deinit()
{
    if (monitors)
        free(monitors);
    if (frames) {
        for (int i = 0; i < nFrames; i++) {
            // Make sure not to free the current wallpaper
            if (i != frame && frames[i].pixmap)
                XFreePixmap(display, frames[i].pixmap);
        }
        free(frames);
    }
    if (display)
        XCloseDisplay(display);
}

int setWallpaper(Pixmap pixmap)
{
    int in, out, w, h;
    Atom prop_root, prop_esetroot, type;
    int format;
    unsigned long length, after;
    unsigned char *data_root, *data_esetroot;

    prop_root = XInternAtom(display, "_XROOTPMAP_ID", True);
    prop_esetroot = XInternAtom(display, "ESETROOT_PMAP_ID", True);

    // If someone other than us owns the wallpaper first, we must kill them.
    if (prop_root != None && prop_esetroot != None) {
        XGetWindowProperty(display, root, prop_root, 0L, 1L, False, AnyPropertyType, &type, &format, &length, &after, &data_root);
        if (type == XA_PIXMAP) {
            XGetWindowProperty(display, root, prop_esetroot, 0L, 1L, False, AnyPropertyType, &type, &format, &length, &after, &data_esetroot);
            if (data_root && data_esetroot) {
                if (type == XA_PIXMAP && *((Pixmap *)data_root) == *((Pixmap *)data_esetroot)) {
                    // Someone's got XROOTPMAP and ESETROOT_PMAP already. Make sure it isn't us!
                    Pixmap data_pixmap = *((Pixmap *)data_root);

                    int kill = 1;
                    for (int i = 0; i < nFrames; i++) {
                        if (data_pixmap == frames[i].pixmap) {
                            kill = 0;
                            break;
                        }
                    }

                    // If it isn't us, kill it.
                    if (kill)
                        XKillClient(display, data_pixmap);
                }
            }
        }
    }

    /* This will locate the property, creating it if it doesn't exist */
    prop_root = XInternAtom(display, "_XROOTPMAP_ID", False);
    prop_esetroot = XInternAtom(display, "ESETROOT_PMAP_ID", False);

    if (prop_root == None || prop_esetroot == None) {
        fprintf(stderr, "error: creation of pixmap property failed.");
        return 1;
    }

    XChangeProperty(display, root, prop_root, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pixmap, 1);
    XChangeProperty(display, root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pixmap, 1);

    XSetWindowBackgroundPixmap(display, root, pixmap);
    XClearWindow(display, root);
    XFlush(display);
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

                for (int i = 0; i < 3; i++) {
                    int dstTest = (int)((float)src[index1 + i] * (1.0f - dx) * (1.0f - dy) + (float)src[index2 + i] * (dx) * (1.0f - dy) + (float)src[index3 + i] * (1.0f - dx) * (dy) + (float)src[index4 + i] * (dx) * (dy));
                    if (dstTest > 255)
                        dst[indexDst + i] = 255;
                    else if (dstTest < 0)
                        dst[indexDst + i] = 0;
                    else
                        dst[indexDst + i] = dstTest;
                }
            } else {
                int indexSrc = ((srcY + (y * srcH / dstH)) * srcWidth + (srcX + (x * srcW / dstW))) * 4;
                dst[indexDst + 0] = src[indexSrc + 0];
                dst[indexDst + 1] = src[indexSrc + 1];
                dst[indexDst + 2] = src[indexSrc + 2];
            }
        }
    }
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
    GifFileType *gif = DGifOpenFileName(argv[3], NULL);
    if (!gif || DGifSlurp(gif) != GIF_OK || gif->ImageCount <= 1) {
        if (gif)
            DGifCloseFile(gif);
        fprintf(stderr, "error: %s: not an animated gif.\n", argv[3]);
        goto usage;
    }

    // Gif info
    nFrames = gif->ImageCount;

    // Allocate frame array
    frames = malloc(sizeof(Frame) * nFrames);
    memset(frames, 0, sizeof(Frame) * nFrames);

    // Buffer for RGBA image data
    unsigned char *data = malloc(gif->SWidth * gif->SHeight * 4);
    memset(data, 0, gif->SWidth * gif->SHeight * 4);

    // Rescaled wallpaper which encompasses the entire x11 screen;
    // not necessary for tiled mode
    unsigned char *dataScaled;
    if (style != STYLE_TILE) {
        dataScaled = malloc(screen->width * screen->height * 4);
        memset(dataScaled, 0, screen->width * screen->height * 4);
    }

    // Graphics context for converting image
    GC gc = XCreateGC(display, root, 0, 0);

    for (int i = 0; i < nFrames; i++) {
        int transparent = -1;
        SavedImage *si = &gif->SavedImages[i];

        // Get the frame wait/transparent color, combine/replace
        for (int j = 0; j < si->ExtensionBlockCount; j++) {
            if (si->ExtensionBlocks[j].Function == GRAPHICS_EXT_FUNC_CODE) {
                GraphicsControlBlock gcb;
                DGifExtensionToGCB(si->ExtensionBlocks[j].ByteCount, si->ExtensionBlocks[j].Bytes, &gcb);
                frames[i].wait = gcb.DelayTime;
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
            Pixmap pixmap = XCreatePixmap(display, root, gif->SWidth, gif->SHeight, depth);
            XImage *img = XCreateImage(display, CopyFromParent, depth, ZPixmap, 0, data, gif->SWidth, gif->SHeight, 32, 0);
            XPutImage(display, pixmap, gc, img, 0, 0, 0, 0, gif->SWidth, gif->SHeight);
            img->data = NULL;
            XDestroyImage(img);

            // Now tile it onto the real pixmap
            frames[i].pixmap = XCreatePixmap(display, root, screen->width, screen->height, depth);
            XGCValues gc2values;
            gc2values.fill_style = FillTiled;
            gc2values.tile = pixmap;
            GC gc2 = XCreateGC(display, frames[i].pixmap, GCFillStyle | GCTile, &gc2values);
            XFillRectangle(display, frames[i].pixmap, gc2, 0, 0, screen->width, screen->height);
            XFreeGC(display, gc2);
            XSync(display, False);
            XFreePixmap(display, pixmap);
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

        default:
            fprintf(stderr, "error: not yet implemented\n");
            free(dataScaled);
            free(data);
            XFreeGC(display, gc);
            DGifCloseFile(gif);
            return 1;
        }

        if (style != STYLE_TILE) {
            // Convert to a pixmap
            frames[i].pixmap = XCreatePixmap(display, root, screen->width, screen->height, depth);
            XImage *img = XCreateImage(display, CopyFromParent, depth, ZPixmap, 0, dataScaled, screen->width, screen->height, 32, 0);
            XPutImage(display, frames[i].pixmap, gc, img, 0, 0, 0, 0, screen->width, screen->height);
            img->data = NULL;
            XDestroyImage(img);
        }
    }

    // Free X11/gif shit
    if (style != STYLE_TILE)
        free(dataScaled);
    free(data);
    XFreeGC(display, gc);
    DGifCloseFile(gif);

    // Cycle through frames
    frame = 0;
    for (;;) {
        if (setWallpaper(frames[frame].pixmap))
            return 1;
        usleep(frames[frame].wait * 10000);
        if (++frame >= nFrames)
            frame = 0;
    }

    return 0;

usage:
    fprintf(stderr, "usage: %s <center|tile|stretch|fit|fill> <anti-aliasing off|on> <animated gif image>\n", argv[0]);
    return 1;
}
