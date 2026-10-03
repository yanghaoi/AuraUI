#pragma once

#include "config/config.h"

#include <d2d1.h>

namespace auraui {

// Colour + metric palette for the HUD, derived from the user configuration.
struct Theme {
    D2D1_COLOR_F bg;         // panel background (alpha from Config::opacity)
    D2D1_COLOR_F border;
    D2D1_COLOR_F separator;
    D2D1_COLOR_F title;
    D2D1_COLOR_F label;
    D2D1_COLOR_F value;
    D2D1_COLOR_F muted;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F track;
    D2D1_COLOR_F warn;
    D2D1_COLOR_F crit;

    static Theme FromConfig(const Config& cfg);
};

// 0xRRGGBB (+ alpha) -> D2D1_COLOR_F
D2D1_COLOR_F Rgb(unsigned rgb, float alpha = 1.0f);

// Pick the bar colour for a 0..1 fraction.
D2D1_COLOR_F BarColorFor(const Theme& t, float fraction);

}  // namespace auraui
