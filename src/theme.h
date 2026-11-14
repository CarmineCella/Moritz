// theme.h — colours and sizes shared by the canvas and the interface objects
#pragma once

#include <FL/Enumerations.H>

namespace mlsp {

constexpr int kPortW = 8, kPortH = 3, kBoxH = 22, kFontSize = 12, kRadius = 6;
constexpr int kPortInset = 4;            // keeps ports clear of the rounded corners
constexpr int kSuggRowH = 20, kSuggMax = 8;
constexpr Fl_Font kFont = FL_HELVETICA;

// dark theme
const Fl_Color kBgLocked  = fl_rgb_color(34, 35, 39);
const Fl_Color kBgEdit    = fl_rgb_color(28, 29, 33);
const Fl_Color kGrid      = fl_rgb_color(50, 52, 58);
const Fl_Color kBoxFill   = fl_rgb_color(54, 57, 64);
const Fl_Color kNumFill   = fl_rgb_color(44, 47, 54);
const Fl_Color kMsgFill   = fl_rgb_color(76, 80, 90);
const Fl_Color kFlash     = fl_rgb_color(70, 130, 220);
const Fl_Color kBroken    = fl_rgb_color(78, 40, 44);
const Fl_Color kBorder    = fl_rgb_color(92, 96, 108);
const Fl_Color kText      = fl_rgb_color(226, 228, 234);
const Fl_Color kDim       = fl_rgb_color(140, 144, 156);
const Fl_Color kAccent    = fl_rgb_color(110, 170, 255);
const Fl_Color kSelect    = fl_rgb_color(90, 160, 255);
const Fl_Color kSignal    = fl_rgb_color(236, 196, 70);
const Fl_Color kSignalDim = fl_rgb_color(120, 96, 28);
const Fl_Color kCtlPort   = fl_rgb_color(196, 200, 210);
const Fl_Color kCable     = fl_rgb_color(160, 164, 176);
const Fl_Color kPopup     = fl_rgb_color(44, 46, 53);
const Fl_Color kStatusBg  = fl_rgb_color(22, 23, 26);
const Fl_Color kEditBg    = fl_rgb_color(78, 84, 98);   // box being typed into
const Fl_Color kOn        = fl_rgb_color(52, 150, 90);    // toolbar: audio running

}  // namespace mlsp
