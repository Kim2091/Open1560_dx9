/*
    Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
    Copyright (C) 2020 Brick

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

define_dummy_symbol(mmcamcs_camshake);

#include "camshake.h"

#include "mmcar/car.h"
#include "mmsettings/settings.h"
#include "vector7/matrix34.h"

static mem::cmd_param PARAM_shake {"orbitshake", "Camera engine shake strength; 0 is off"};
static mem::cmd_param PARAM_shakeamp {"orbitshakeamp", "Overall camera shake displacement multiplier"};
static mem::cmd_param PARAM_shakerough {"orbitshakerough", "Camera road roughness shake strength"};
static mem::cmd_param PARAM_shakeimpact {"orbitshakeimpact", "Camera collision impact shake strength"};
static mem::cmd_param PARAM_shakeaccel {"orbitshakeaccel", "Camera acceleration shake strength"};
static mem::cmd_param PARAM_shakerpm {"orbitshakerpm", "Fraction of the redline the engine shake starts building from"};
static mem::cmd_param PARAM_shakegears {"orbitshakegears", "How many of the top gears the engine shake is allowed in"};
static mem::cmd_param PARAM_shakeshift {
    "orbitshakeshift", "How far the engine shake dips while a gear change is in progress"};
static mem::cmd_param PARAM_shakemingear {
    "orbitshakemingear", "Lowest forward gear the engine shake is allowed in at all; 1 is first"};
static mem::cmd_param PARAM_shakerevtop {
    "orbitshakerevtop", "How much of the engine shake's top-end rev ramp is taken back off; 0 is the straight ramp"};
static mem::cmd_param PARAM_shaketurn {
    "orbitshaketurn", "How much of the shake a turn removes; 1 silences it while turning, 0 is off"};

// Fundamental frequency of the shake, in cycles per second, rising with trauma. Since trauma is
// mostly engine revs, the vibration tightens as the needle climbs and slackens on every upshift,
// which is what an engine actually does. The octave multipliers below sit on top of this.
static constexpr f32 ShakeBaseHz = 13.0f;
static constexpr f32 ShakeHzRange = 11.0f;

// Rate the amplitude itself wanders at, relative to the shake frequency. Real vibration does not
// hold a constant intensity - it gusts - and a fixed amplitude is most of what makes shake read as
// artificial. This modulates between GustFloor and full strength.
static constexpr f32 GustRatio = 0.25f;
static constexpr f32 GustFloor = 0.4f;

// Independent hash salts, so the channels never correlate into a single diagonal wobble.
static constexpr i32 SaltPitch = 0;
static constexpr i32 SaltYaw = 1;
static constexpr i32 SaltRoll = 2;
static constexpr i32 SaltRight = 3;
static constexpr i32 SaltUp = 4;
static constexpr i32 SaltGust = 5;
static constexpr i32 SaltForward = 6;

// Envelope rates. Attack is quicker than release everywhere: effects should arrive promptly and
// leave gently, or the camera feels twitchy.
//
// The engine envelope is much quicker than the rest: revs move fast, and the whole point of driving
// the shake from them is that it tracks them closely enough to feel the gear change.
static constexpr f32 EngineAttack = 9.0f;
static constexpr f32 EngineRelease = 6.0f;
static constexpr f32 RoughAttack = 14.0f;
static constexpr f32 RoughRelease = 5.0f;
static constexpr f32 AccelAttack = 10.0f;
static constexpr f32 AccelRelease = 3.5f;

// Impacts have no attack at all - they land on the frame they happen and then decay.
static constexpr f32 ImpactDecay = 3.2f;

// Acceleration magnitude, in m/s^2, beyond which a frame counts as a collision rather than driving.
// Hard braking peaks around 10; anything near 30 is a wall.
static constexpr f32 ImpactThreshold = 30.0f;
static constexpr f32 ImpactRange = 55.0f;

// Longitudinal acceleration, in m/s^2, that produces full acceleration shake.
static constexpr f32 AccelReference = 11.0f;

// Mean suspension travel rate, in m/s, that produces full roughness shake.
static constexpr f32 RoughReference = 0.85f;

// Turn rate, in radians per second, that produces full shake suppression. About a second and a half
// for a half turn - brisk cornering rather than a lane change, so ordinary straight-line steering
// corrections leave the shake alone.
static constexpr f32 TurnRateFull = 1.1f;

// Envelope for the turn gate. Attack is near-immediate so the shake is already gone by the time the
// car is visibly turning; release is slow enough that it does not return between the two halves of
// an S-bend, where the yaw rate passes through zero but the camera is still working.
static constexpr f32 TurnAttack = 16.0f;
static constexpr f32 TurnRelease = 2.5f;

// Length of the noise lattice. The hash index wraps within it, which makes every channel exactly
// periodic over this many cells; that in turn lets the phase accumulator be wrapped without a
// discontinuity, and keeps the values it feeds small enough for f32 to resolve them cleanly. At the
// frequencies used here one period is several minutes, so the repeat is not perceptible.
static constexpr f32 NoisePeriod = 4096.0f;
static constexpr i32 NoiseMask = 4095;

// Reduces an angle to [-PI, PI] so differences take the short way round.
static f32 WrapAngle(f32 angle)
{
    return std::remainder(angle, 2.0f * ARTS_PI);
}

// Frame-rate independent exponential approach: the same fraction of the remaining distance is
// covered per second regardless of how the frames fall.
static f32 Blend(f32 rate, f32 delta)
{
    return 1.0f - std::exp(-rate * delta);
}

// Asymmetric envelope follower - rises at one rate, falls at another.
static f32 Envelope(f32 current, f32 target, f32 delta, f32 attack, f32 release)
{
    return current + ((target - current) * Blend((target > current) ? attack : release, delta));
}

// Integer hash to [-1, 1]. Salt selects an independent sequence, so two channels sampled at the same
// instant are uncorrelated.
static f32 Hash(i32 point, i32 salt)
{
    u32 hash = static_cast<u32>(point & NoiseMask) + (static_cast<u32>(salt) * 0x9E3779B9u);

    hash ^= hash >> 16;
    hash *= 0x7FEB352Du;
    hash ^= hash >> 15;
    hash *= 0x846CA68Bu;
    hash ^= hash >> 16;

    return (static_cast<f32>(hash >> 8) * (2.0f / 16777215.0f)) - 1.0f;
}

// Value noise: random values on the integer lattice, smoothstepped between. Unlike a sum of sines
// this has no period at all, which is what stops the shake being anticipatable, and unlike per-frame
// randomness it is a continuous function of time.
static f32 ValueNoise(f32 time, i32 salt)
{
    f32 base = std::floor(time);
    f32 frac = time - base;
    i32 point = static_cast<i32>(base);

    f32 weight = frac * frac * (3.0f - (2.0f * frac));
    f32 low = Hash(point, salt);

    return low + ((Hash(point + 1, salt) - low) * weight);
}

// Three octaves of value noise. The high octaves are what give the motion its grain; without them it
// reads as a slow sway rather than vibration. Weights sum to one, bounding the result to [-1, 1].
// The octave multipliers are whole numbers so that every octave completes a whole number of periods
// together, which is what keeps the phase wrap seamless; the octaves are decorrelated by salt
// instead, so nothing lines up into a beat.
static f32 ShakeNoise(f32 time, i32 salt)
{
    return (ValueNoise(time, salt) * 0.55f) + (ValueNoise(time * 2.0f, salt + 64) * 0.33f) +
        (ValueNoise(time * 4.0f, salt + 128) * 0.12f);
}

void mmCamShake::Init()
{
    generation_ = mmSettingsGeneration();

    Amplitude = mmSettingFloat("orbitshakeamp", 1.0f);
    EngineScale = mmSettingFloat("orbitshake", 0.8f);
    RoughScale = mmSettingFloat("orbitshakerough", 0.55f);
    ImpactScale = mmSettingFloat("orbitshakeimpact", 1.0f);
    AccelScale = mmSettingFloat("orbitshakeaccel", 0.35f);

    RpmStart = mmSettingFloat("orbitshakerpm", 0.55f);

    // Narrow, and floored at third on top of that. The engine shake is at its most convincing as
    // something that only shows up once the car is genuinely working - in the gears it spends real
    // time in at speed - and it reads as a rattle rather than a top end when it can reach full
    // strength in a gear the car merely passes through.
    GearSpan = mmSettingFloat("orbitshakegears", 1.5f);
    MinGear = mmSettingFloat("orbitshakemingear", 3.0f);
    ShiftDip = mmSettingFloat("orbitshakeshift", 0.35f);
    RevRolloff = mmSettingFloat("orbitshakerevtop", 0.35f);

    TurnScale = mmSettingFloat("orbitshaketurn", 1.0f);
}

void mmCamShake::Reset()
{
    EngineTrauma = 0.0f;
    RoughTrauma = 0.0f;
    AccelTrauma = 0.0f;
    AccelSigned = 0.0f;
    ImpactTrauma = 0.0f;
    TurnFactor = 0.0f;

    time_ = 0.0f;
    gust_time_ = 0.0f;
    strength_ = 0.0f;

    prev_velocity_ = {0.0f, 0.0f, 0.0f};
    prev_heading_ = 0.0f;
    has_history_ = false;

    for (i32 i = 0; i < 4; ++i)
        prev_suspension_[i] = 0.0f;
}

f32 mmCamShake::Combine() const
{
    f32 trauma = std::clamp((EngineTrauma * EngineScale) + (RoughTrauma * RoughScale) + (AccelTrauma * AccelScale) +
            (ImpactTrauma * ImpactScale),
        0.0f, 1.0f);

    // Applied to the combined trauma rather than to any one source, and before the squaring below
    // rather than after, so that easing off in a corner is decisive rather than a slight thinning.
    trauma *= 1.0f - (std::clamp(TurnScale, 0.0f, 1.0f) * TurnFactor);

    // Squaring keeps low trauma imperceptible rather than a permanent low buzz, and makes the ramp
    // towards top speed feel like it builds instead of arriving linearly.
    return trauma * trauma;
}

void mmCamShake::Update(const mmCar* car, const Matrix34* car_matrix, f32 delta, f32 mouse_turn_rate)
{
    if (delta <= 0.0f)
        return;

    // Re-resolve when the menu has changed something. One integer compare in the common case, which
    // is what buys the tunables being ordinary fields rather than a name lookup each time they are
    // read - and there are a dozen of them read several times a frame.
    if (generation_ != mmSettingsGeneration())
        Init();

    if (car)
    {
        const mmCarSim& sim = car->Sim;

        // The shake comes off the engine rather than road speed: what shakes a car is the motor, so
        // it climbs towards the redline and falls away the moment a shift drops the needle - no gear
        // change has to be scripted, the revs already describe it.
        const mmEngine& engine = sim.Engine;
        const mmTransmission& trans = sim.Trans;

        f32 engine_target = 0.0f;

        if (trans.IsForward() && (engine.MaxRPM > 0.0f))
        {
            f32 rpm_floor = engine.MaxRPM * RpmStart;
            f32 rpm_span = engine.MaxRPM - rpm_floor;
            f32 revs = (rpm_span > 0.0f) ? std::clamp((engine.RPM - rpm_floor) / rpm_span, 0.0f, 1.0f) : 0.0f;

            // Ease the top of the rev band off. A straight ramp puts full shake on the limiter and
            // holds it there, which is where the car spends the whole of a long straight - so the
            // effect meant to mark the top end becomes the normal state of driving fast. Still
            // monotonic in the revs, just flatter where it was hardest.
            revs *= 1.0f - (std::clamp(RevRolloff, 0.0f, 0.9f) * revs);

            // Top gears only, fading in over the last few. The lower gears are short and mostly
            // spent mid-shift, and shaking through them turns every pull-away into a rattle.
            //
            // CurrentGear counts reverse as 0 and neutral as 1 (mmTransmission::IsForward), so the
            // first forward gear is 2 and MinGear, which is expressed as an ordinary gear number, is
            // compared against CurrentGear - 1.
            i32 gear_count = trans.IsAutomatic ? trans.NumGears : trans.ManualNumGears;
            f32 span = std::max(GearSpan, 1.0f);
            f32 top = static_cast<f32>(gear_count - 1);
            f32 gear = std::clamp((static_cast<f32>(trans.CurrentGear) - (top - span)) / span, 0.0f, 1.0f);

            // The fade above is relative to the top of the box, so a short gearbox reaches full
            // shake in what is really second. This is the absolute floor underneath it.
            if (static_cast<f32>(trans.CurrentGear - 1) < MinGear)
                gear = 0.0f;

            engine_target = revs * gear;

            // The revs falling already eases the shake off on their own; this deepens the gap so the
            // shift lands as a beat rather than a shrug.
            if (engine.ChangingGear)
                engine_target *= ShiftDip;
        }

        EngineTrauma = Envelope(EngineTrauma, engine_target, delta, EngineAttack, EngineRelease);

        // Road roughness, read as how fast the suspension is moving. Only wheels actually on the
        // ground contribute - a wheel dangling in mid-air swings freely and would otherwise register
        // as the roughest road in the city.
        //
        // Measured per AXLE, from the part of the motion both its wheels share. Body roll is the
        // largest thing the suspension does in a corner and it is entirely differential - the
        // outside wheel compresses by very nearly what the inside one extends - so summing the two
        // wheels independently reads a hard turn-in as the roughest road surface in the game. A bump
        // moves both wheels the same way and survives the average; roll cancels out of it exactly,
        // which is what it should do, because rolling the body is not the road being rough.
        //
        // An axle with only one wheel down has nothing to average against and contributes that
        // wheel's own motion, which is the right answer there: with one wheel in the air the car is
        // over something, and the roll and the road are the same event.
        const mmWheel* wheels[4] {&sim.FrontLeft, &sim.FrontRight, &sim.BackLeft, &sim.BackRight};

        f32 rates[4] {};
        bool grounded[4] {};

        for (i32 i = 0; i < 4; ++i)
        {
            rates[i] = (wheels[i]->Suspension - prev_suspension_[i]) / delta;
            grounded[i] = (wheels[i]->OnGround != 0);

            prev_suspension_[i] = wheels[i]->Suspension;
        }

        f32 travel = 0.0f;
        i32 axles = 0;

        for (i32 axle = 0; axle < 2; ++axle)
        {
            const i32 left = axle * 2;
            const i32 right = left + 1;

            if (grounded[left] && grounded[right])
                travel += std::fabs((rates[left] + rates[right]) * 0.5f);
            else if (grounded[left])
                travel += std::fabs(rates[left]);
            else if (grounded[right])
                travel += std::fabs(rates[right]);
            else
                continue;

            ++axles;
        }

        f32 rough_target = 0.0f;

        if (has_history_ && (axles > 0))
            rough_target = std::clamp((travel / static_cast<f32>(axles)) / RoughReference, 0.0f, 1.0f);

        RoughTrauma = Envelope(RoughTrauma, rough_target, delta, RoughAttack, RoughRelease);

        // One velocity derivative feeds both remaining sources: its component along the car's
        // forward axis is acceleration and braking, while its total magnitude is what a collision
        // looks like.
        Vector3 velocity = sim.ICS.LinearVelocity;
        f32 accel_target = 0.0f;
        f32 signed_target = 0.0f;

        if (has_history_)
        {
            Vector3 change = velocity - prev_velocity_;
            f32 magnitude = change.Mag() / delta;

            if (car_matrix)
            {
                signed_target = std::clamp((change ^ car_matrix->m2) / delta / AccelReference, -1.0f, 1.0f);
                accel_target = std::fabs(signed_target);
            }

            if (magnitude > ImpactThreshold)
                ImpactTrauma =
                    std::max(ImpactTrauma, std::clamp((magnitude - ImpactThreshold) / ImpactRange, 0.0f, 1.0f));
        }

        prev_velocity_ = velocity;

        AccelTrauma = Envelope(AccelTrauma, accel_target, delta, AccelAttack, AccelRelease);

        // Chased at the attack rate in both directions: the lean should track the throttle and the
        // brake equally closely, and an asymmetric follower would make it drift back to centre at a
        // different speed from the one it left at.
        AccelSigned += (signed_target - AccelSigned) * Blend(AccelAttack, delta);
    }
    else
    {
        // No car - decay everything rather than holding the last frame's values.
        EngineTrauma = Envelope(EngineTrauma, 0.0f, delta, EngineRelease, EngineRelease);
        RoughTrauma = Envelope(RoughTrauma, 0.0f, delta, RoughRelease, RoughRelease);
        AccelTrauma = Envelope(AccelTrauma, 0.0f, delta, AccelRelease, AccelRelease);
        AccelSigned -= AccelSigned * Blend(AccelRelease, delta);
    }

    ImpactTrauma *= std::exp(-ImpactDecay * delta);

    // The turn gate. Roughness no longer mistakes body roll for a rough road, which removes the
    // largest single cause of the camera shaking through corners, but it is not the only one: the
    // car's own yaw is already swinging the view, and any vibration on top of a view that is moving
    // reads as instability rather than as speed. So the shake steps aside while the view is being
    // turned, whichever of the two is turning it.
    //
    // Taken from the car's heading rather than its yaw rate about its own axis, because it is the
    // camera's motion that matters and the camera follows the heading. A spin registers as a turn
    // too, which is correct - that is the last moment to be adding camera shake to.
    f32 turn_rate = mouse_turn_rate;

    if (car_matrix)
    {
        f32 heading = std::atan2(car_matrix->m2.x, car_matrix->m2.z);

        if (has_history_)
            turn_rate = std::max(turn_rate, std::fabs(WrapAngle(heading - prev_heading_)) / delta);

        prev_heading_ = heading;
    }

    TurnFactor = Envelope(TurnFactor, std::clamp(turn_rate / TurnRateFull, 0.0f, 1.0f), delta, TurnAttack, TurnRelease);

    has_history_ = true;

    const f32 amount = Combine();

    // Phase is integrated rather than time being scaled at the sample point, so that changing the
    // frequency with trauma speeds the vibration up instead of jumping its phase.
    const f32 rate = ShakeBaseHz + (ShakeHzRange * amount);

    time_ = std::fmod(time_ + (delta * rate), NoisePeriod);
    gust_time_ = std::fmod(gust_time_ + (delta * rate * GustRatio), NoisePeriod);

    // Let the amplitude wander instead of holding steady. A constant-amplitude shake is the giveaway
    // that it is being generated rather than caused.
    const f32 gust = (ShakeNoise(gust_time_, SaltGust) * 0.5f) + 0.5f;

    strength_ = amount * Amplitude * (GustFloor + ((1.0f - GustFloor) * gust));
}

f32 mmCamShake::PitchNoise() const
{
    return ShakeNoise(time_, SaltPitch);
}

f32 mmCamShake::YawNoise() const
{
    return ShakeNoise(time_, SaltYaw);
}

f32 mmCamShake::RollNoise() const
{
    return ShakeNoise(time_, SaltRoll);
}

f32 mmCamShake::RightNoise() const
{
    return ShakeNoise(time_, SaltRight);
}

f32 mmCamShake::UpNoise() const
{
    return ShakeNoise(time_, SaltUp);
}

f32 mmCamShake::ForwardNoise() const
{
    return ShakeNoise(time_, SaltForward);
}
