#pragma once

namespace Frost {

    // World space is measured in centimeters: one world unit is one centimeter (as in Unreal
    // Engine). Positions, sizes and distances (collider sizes, light ranges, clip planes) are in
    // centimeters, and speeds in centimeters per second; mass is in kilograms and time in seconds.
    //
    // Write lengths with these constants (1.8f * kMeter, 35.0f * kCentimeter) so the unit is visible
    // where the number is.
    inline constexpr float kCentimeter = 1.0f;
    inline constexpr float kMeter = 100.0f * kCentimeter;
    inline constexpr float kKilometer = 1000.0f * kMeter;

    // Standard gravity, 9.81 m/s^2, in world units per second squared.
    inline constexpr float kStandardGravity = 9.81f * kMeter;

}  // namespace Frost
