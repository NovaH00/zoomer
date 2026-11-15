#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <vector>

struct MonitorRect {
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
    int primaryIndex = 0;
};

MonitorRect getPrimaryMonitorBounds() {
    MonitorRect rect;
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return rect;

    Window root = DefaultRootWindow(dpy);
    int count = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(dpy, root, True, &count);
    if (monitors && count > 0) {
        for (int i = 0; i < count; ++i) {
            if (monitors[i].primary) {
                rect.x = monitors[i].x;
                rect.y = monitors[i].y;
                rect.width = monitors[i].width;
                rect.height = monitors[i].height;
                rect.primaryIndex = i;
                break;
            }
        }
        XRRFreeMonitors(monitors);
    }
    XCloseDisplay(dpy);
    return rect;
}

int main(int argc, char* argv[]) {
    MonitorRect primary = getPrimaryMonitorBounds();
    const char* tmpPath = "/dev/shm/gnome_shot.png";

    // 1. Capture full desktop directly to RAM (/dev/shm) using trusted gnome-screenshot
    std::string cmd = std::string("gnome-screenshot -f ") + tmpPath;
    if (std::system(cmd.c_str()) != 0) {
        std::println(stderr, "gnome-screenshot failed");
        return 1;
    }

    // 2. Decode PNG directly from RAM
    int fullW, fullH, channels;
    unsigned char* pixels = stbi_load(tmpPath, &fullW, &fullH, &channels, 4);
    std::remove(tmpPath); // Clean up RAM file immediately
    if (!pixels) {
        std::println(stderr, "Failed to load screenshot: {}", stbi_failure_reason());
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::println(stderr, "SDL_Init error: {}", SDL_GetError());
        stbi_image_free(pixels);
        return 1;
    }

    // 3. Open fullscreen window on target monitor (defaults to primary)
    int displayIndex = (argc >= 2) ? std::stoi(argv[1]) : primary.primaryIndex;
    SDL_Window* window = SDL_CreateWindow(
        "Viewer",
        SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex),
        SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex),
        0, 0,
        SDL_WINDOW_FULLSCREEN_DESKTOP
    );

    // Disable VSYNC buffering to eliminate swapchain lag between hardware cursor and rendered frames
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_GL_SetSwapInterval(0);
    SDL_RenderSetLogicalSize(renderer, primary.width, primary.height);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, fullW, fullH
    );
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    SDL_UpdateTexture(texture, nullptr, pixels, fullW * 4);
    stbi_image_free(pixels);

    // 4. Create dimming mask texture: immediate sharp cutoff between clear circle and dimmed region
    const int SPOT_SIZE = 1024;
    const float center = (SPOT_SIZE - 1) * 0.5f;
    const float R_cutoff = center - 2.0f;

    SDL_Texture* dimMaskTexture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, SPOT_SIZE, SPOT_SIZE
    );
    SDL_SetTextureBlendMode(dimMaskTexture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(dimMaskTexture, SDL_ScaleModeLinear);

    std::vector<Uint8> maskPixels(SPOT_SIZE * SPOT_SIZE * 4);

    for (int y = 0; y < SPOT_SIZE; ++y) {
        for (int x = 0; x < SPOT_SIZE; ++x) {
            float dx = static_cast<float>(x) - center;
            float dy = static_cast<float>(y) - center;
            float dist = std::sqrt(dx * dx + dy * dy);
            int idx = (y * SPOT_SIZE + x) * 4;

            // Immediate sharp cutoff: inside is 100% clear (0), outside is 100% dimmed (255)
            float alpha = 0.0f;
            if (dist < R_cutoff - 0.5f) {
                alpha = 0.0f;
            } else if (dist <= R_cutoff + 0.5f) {
                alpha = (dist - (R_cutoff - 0.5f)) * 255.0f;
            } else {
                alpha = 255.0f;
            }

            maskPixels[idx + 0] = 0;
            maskPixels[idx + 1] = 0;
            maskPixels[idx + 2] = 0;
            maskPixels[idx + 3] = static_cast<Uint8>(std::clamp(alpha, 0.0f, 255.0f));
        }
    }
    SDL_UpdateTexture(dimMaskTexture, nullptr, maskPixels.data(), SPOT_SIZE * 4);

    // 5. GPU-side crop: render only the primary monitor's rectangle
    SDL_Rect srcRect = { primary.x, primary.y, primary.width, primary.height };

    // Zoom and pan state
    float currentZoom = 1.0f;
    float targetZoom = 1.0f;
    float currentOffX = 0.0f;
    float currentOffY = 0.0f;
    float targetOffX = 0.0f;
    float targetOffY = 0.0f;

    // Lightbulb spotlight state
    const float MAX_DIM_ALPHA = 0.72f;
    bool spotlightOn = false;
    float spotlightRadius = static_cast<float>(primary.height) * 0.22f;

    // Direct X11 pointer querying for zero-latency cursor tracking
    Display* xDisplay = XOpenDisplay(nullptr);
    Window xRoot = xDisplay ? DefaultRootWindow(xDisplay) : 0;

    float cursorLogicalX = static_cast<float>(primary.width) * 0.5f;
    float cursorLogicalY = static_cast<float>(primary.height) * 0.5f;

    auto updateLiveCursor = [&]() {
        if (xDisplay) {
            Window r_ret, c_ret;
            int rx = 0, ry = 0, wx = 0, wy = 0;
            unsigned int mask = 0;
            if (XQueryPointer(xDisplay, xRoot, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
                cursorLogicalX = static_cast<float>(rx - primary.x);
                cursorLogicalY = static_cast<float>(ry - primary.y);
                cursorLogicalX = std::clamp(cursorLogicalX, 0.0f, static_cast<float>(primary.width));
                cursorLogicalY = std::clamp(cursorLogicalY, 0.0f, static_cast<float>(primary.height));
                return;
            }
        }
        int winX = 0, winY = 0;
        SDL_GetMouseState(&winX, &winY);
        float mx = static_cast<float>(winX);
        float my = static_cast<float>(winY);
        SDL_RenderWindowToLogical(renderer, winX, winY, &mx, &my);
        cursorLogicalX = std::clamp(mx, 0.0f, static_cast<float>(primary.width));
        cursorLogicalY = std::clamp(my, 0.0f, static_cast<float>(primary.height));
    };

    updateLiveCursor();

    bool isDragging = false;

    Uint64 lastTime = SDL_GetPerformanceCounter();
    const Uint64 perfFreq = SDL_GetPerformanceFrequency();

    bool running = true;
    SDL_Event e;

    auto processEvent = [&](const SDL_Event& ev) {
        if (ev.type == SDL_QUIT) {
            running = false;
        } else if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) {
                running = false;
            } else if (ev.key.keysym.sym == SDLK_r || ev.key.keysym.sym == SDLK_0 ||
                       ev.key.keysym.sym == SDLK_BACKSPACE) {
                targetZoom = 1.0f;
                targetOffX = 0.0f;
                targetOffY = 0.0f;
                spotlightOn = false;
            } else if (ev.key.keysym.sym == SDLK_l) {
                spotlightOn = !spotlightOn;
            } else if (ev.key.keysym.sym == SDLK_LEFTBRACKET) {
                spotlightRadius = std::max(60.0f, spotlightRadius - 20.0f);
            } else if (ev.key.keysym.sym == SDLK_RIGHTBRACKET) {
                spotlightRadius = std::min(static_cast<float>(primary.height) * 0.7f, spotlightRadius + 20.0f);
            }
        } else if (ev.type == SDL_MOUSEWHEEL) {
            float wheelDelta = (ev.wheel.preciseY != 0.0f) ? ev.wheel.preciseY
                                                           : static_cast<float>(ev.wheel.y);
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                wheelDelta = -wheelDelta;
            }

            if (wheelDelta != 0.0f) {
                updateLiveCursor();

                float u = (cursorLogicalX - currentOffX) / currentZoom;
                float v = (cursorLogicalY - currentOffY) / currentZoom;

                float zoomFactor = std::pow(1.20f, wheelDelta);
                float newTargetZoom = std::clamp(targetZoom * zoomFactor, 1.0f, 32.0f);

                if (newTargetZoom <= 1.0001f) {
                    targetZoom = 1.0f;
                    targetOffX = 0.0f;
                    targetOffY = 0.0f;
                    spotlightOn = false;
                } else {
                    targetZoom = newTargetZoom;
                    targetOffX = cursorLogicalX - targetZoom * u;
                    targetOffY = cursorLogicalY - targetZoom * v;

                    float minOffX = (1.0f - targetZoom) * primary.width;
                    float minOffY = (1.0f - targetZoom) * primary.height;
                    targetOffX = std::clamp(targetOffX, minOffX, 0.0f);
                    targetOffY = std::clamp(targetOffY, minOffY, 0.0f);

                    // Zooming in automatically turns on the lightbulb effect
                    spotlightOn = true;
                }
            }
        } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            if (ev.button.button == SDL_BUTTON_MIDDLE) {
                // Scroll wheel button click toggles the lightbulb effect
                spotlightOn = !spotlightOn;
            } else if (ev.button.button == SDL_BUTTON_LEFT) {
                // Left mouse button drags around the zoomed screen
                if (targetZoom > 1.0f || currentZoom > 1.0f) {
                    isDragging = true;
                }
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                targetZoom = 1.0f;
                targetOffX = 0.0f;
                targetOffY = 0.0f;
                spotlightOn = false;
            }
        } else if (ev.type == SDL_MOUSEBUTTONUP) {
            if (ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_MIDDLE) {
                isDragging = false;
            }
        } else if (ev.type == SDL_MOUSEMOTION) {
            updateLiveCursor();
            if (isDragging) {
                targetOffX += static_cast<float>(ev.motion.xrel);
                targetOffY += static_cast<float>(ev.motion.yrel);

                float minOffX = (1.0f - targetZoom) * primary.width;
                float minOffY = (1.0f - targetZoom) * primary.height;
                targetOffX = std::clamp(targetOffX, minOffX, 0.0f);
                targetOffY = std::clamp(targetOffY, minOffY, 0.0f);

                currentOffX = targetOffX;
                currentOffY = targetOffY;
            }
        }
    };

    while (running) {
        while (SDL_PollEvent(&e)) {
            processEvent(e);
        }

        // Rigid tracking: fetch instantaneous hardware pointer coordinates
        updateLiveCursor();

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(now - lastTime) / static_cast<float>(perfFreq);
        lastTime = now;
        if (dt > 0.05f) dt = 0.05f;

        bool isZoomAnimating = (std::abs(currentZoom - targetZoom) > 0.0001f ||
                                std::abs(currentOffX - targetOffX) > 0.01f ||
                                std::abs(currentOffY - targetOffY) > 0.01f ||
                                isDragging);

        if (isZoomAnimating) {
            float factor = 1.0f - std::exp(-18.0f * dt);
            currentZoom += (targetZoom - currentZoom) * factor;
            currentOffX += (targetOffX - currentOffX) * factor;
            currentOffY += (targetOffY - currentOffY) * factor;

            if (currentZoom <= 1.0001f) {
                currentZoom = 1.0f;
                currentOffX = 0.0f;
                currentOffY = 0.0f;
            } else {
                float minCurOffX = (1.0f - currentZoom) * primary.width;
                float minCurOffY = (1.0f - currentZoom) * primary.height;
                currentOffX = std::clamp(currentOffX, minCurOffX, 0.0f);
                currentOffY = std::clamp(currentOffY, minCurOffY, 0.0f);
            }

            if (std::abs(currentZoom - targetZoom) < 0.0005f &&
                std::abs(currentOffX - targetOffX) < 0.05f &&
                std::abs(currentOffY - targetOffY) < 0.05f) {
                currentZoom = targetZoom;
                currentOffX = targetOffX;
                currentOffY = targetOffY;
            }
        }

        const bool spotlightActive = spotlightOn;

        SDL_RenderClear(renderer);

        // 1. Render zoomed screenshot
        SDL_FRect dstRect = {
            currentOffX,
            currentOffY,
            static_cast<float>(primary.width) * currentZoom,
            static_cast<float>(primary.height) * currentZoom
        };
        SDL_RenderCopyF(renderer, texture, &srcRect, &dstRect);

        // 2. Render lightbulb spotlight with immediate cutoff and zero lag
        if (spotlightActive) {
            // Re-query pointer right before drawing spotlight to guarantee zero latency
            updateLiveCursor();

            const float R = spotlightRadius;
            const float x0 = cursorLogicalX - R;
            const float y0 = cursorLogicalY - R;
            const float x1 = cursorLogicalX + R;
            const float y1 = cursorLogicalY + R;
            const float W = static_cast<float>(primary.width);
            const float H = static_cast<float>(primary.height);

            const Uint8 alphaByte = static_cast<Uint8>(MAX_DIM_ALPHA * 255.0f);

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, alphaByte);

            // Top rect
            if (y0 > 0.0f) {
                SDL_FRect topRect = { 0.0f, 0.0f, W, y0 };
                SDL_RenderFillRectF(renderer, &topRect);
            }
            // Bottom rect
            if (y1 < H) {
                SDL_FRect bottomRect = { 0.0f, y1, W, H - y1 };
                SDL_RenderFillRectF(renderer, &bottomRect);
            }

            // Vertical band around the spotlight
            float spanTop = std::max(0.0f, y0);
            float spanBottom = std::min(H, y1);
            float spanHeight = spanBottom - spanTop;

            if (spanHeight > 0.0f) {
                if (x0 > 0.0f) {
                    SDL_FRect leftRect = { 0.0f, spanTop, x0, spanHeight };
                    SDL_RenderFillRectF(renderer, &leftRect);
                }
                if (x1 < W) {
                    SDL_FRect rightRect = { x1, spanTop, W - x1, spanHeight };
                    SDL_RenderFillRectF(renderer, &rightRect);
                }
            }

            // Dim mask: clear light circle inside, sharp cutoff to dimmed outside
            SDL_SetTextureAlphaMod(dimMaskTexture, alphaByte);
            SDL_FRect spotRect = { x0, y0, 2.0f * R, 2.0f * R };
            SDL_RenderCopyF(renderer, dimMaskTexture, nullptr, &spotRect);
        }

        SDL_RenderPresent(renderer);

        // Frame pacing: sleep when idle to save CPU, run fast when active/animating
        if (!spotlightActive && !isZoomAnimating) {
            SDL_Delay(10);
        } else {
            SDL_Delay(1);
        }
    }

    if (xDisplay) {
        XCloseDisplay(xDisplay);
    }
    SDL_DestroyTexture(dimMaskTexture);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}