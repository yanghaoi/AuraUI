#pragma once

namespace auraui::ids {

// Control identifiers for the settings window.
enum : int {
    // [General]
    ChkAutoStart = 1001,
    CmbRefresh,

    // [Display]
    CmbMonitor,
    EdtX,
    EdtY,
    EdtW,
    EdtH,
    SldOpacity,
    LblOpacityValue,
    CmbFont,
    CmbFontSize,

    // [Monitor]
    ChkCpu,
    ChkMemory,
    ChkGpu,
    ChkVram,
    ChkDisk,
    ChkNetwork,
    ChkLanIp,

    // [Appearance]
    ChkTitle,
    ChkBars,
    SldRadius,
    LblRadiusValue,
    BtnOpenConfig,

    // Buttons
    BtnApply,
    BtnOk,
    BtnCancel,
    BtnDefaults,
    BtnCenter,

    // Static text
    LblAbout = 1200,
    LblConfigPath,
    LblMonitorHint,
    LblVersion,   // "v0.2.0" corner badge (text composed at runtime)
};

}  // namespace auraui::ids
