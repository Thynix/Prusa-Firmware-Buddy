#include "homing_cart.hpp"

#include <PersistentStorage.h>
#include <config_store/store_instance.hpp>

#include "Marlin.h" // for suspend_auto_report
#include "../motion.h"
#include "../stepper.h"
#include "feature/prusa/crash_recovery.hpp"
#include "configuration.hpp"

inline constexpr float HOMING_BUMP_DIVISOR_STEP = 1.03f;
inline constexpr float homing_bump_divisor_dflt[] = HOMING_BUMP_DIVISOR;
inline constexpr float homing_bump_divisor_max[] = HOMING_BUMP_DIVISOR_MAX;
inline constexpr float homing_bump_divisor_min[] = HOMING_BUMP_DIVISOR_MIN;

/**
 * Turns automatic reports off until destructor is called.
 * Then it sets reports to previous value.
 */
class Temporary_Report_Off {
    bool suspend_reports = false;

public:
    Temporary_Report_Off() {
        suspend_reports = suspend_auto_report;
        suspend_auto_report = true;
    }
    ~Temporary_Report_Off() {
        suspend_auto_report = suspend_reports;
    }
};

/**
 *  Move back and forth to endstop
 * \returns MSCNT position after endstop has been hit
 */
static int32_t home_and_get_mscnt(AxisEnum axis, int axis_home_dir, feedRate_t fr_mm_s, float &probe_offset) {
    probe_offset = homeaxis_single_run(axis, axis_home_dir, fr_mm_s);
    const int32_t mscnt = (axis == X_AXIS) ? stepperX.MSCNT() : stepperY.MSCNT();
    return mscnt;
}

/**
 * \returns calibrated value from EEPROM in microsteps
 *          always 256 microsteps per step, range 0 to 1023
 */
static int get_calibrated_home(const AxisEnum axis, bool &calibrated) {
    uint16_t mscntRead[PersistentStorage::homeSamplesCount];
    calibrated = PersistentStorage::isCalibratedHome(mscntRead, axis);
    return home_modus(mscntRead, PersistentStorage::homeSamplesCount, 96);
}

/**
 * \param axis axis to be evaluated
 * \param mscnt measured motor position if known,
 *              otherwise, current motor position will be taken
 * \returns shortest distance of the motor to the calibrated position in microsteps,
 *          always 256 microsteps per step, range -512 to 512
 */
static int calibrated_offset_mscnt(const AxisEnum axis, const int mscnt, bool &calibrated) {
    const int cal = get_calibrated_home(axis, calibrated);
    return to_calibrated(cal, mscnt);
}

/**
 * \returns offset [mm] to be subtracted from the current axis position to have correct position
 */
float calibrated_home_offset(const AxisEnum axis) {
    bool calibrated;
    const int cal = get_calibrated_home(axis, calibrated);
    if (!calibrated)
        return 0;

    const constexpr float steps_per_unit[] = DEFAULT_AXIS_STEPS_PER_UNIT;
    switch (axis) {
    case X_AXIS: {
        return ((X_HOME_DIR < 0 ? X_HOME_GAP : -X_HOME_GAP)
            - ((((INVERT_X_DIR) ? -1.f : 1.f) * to_calibrated(cal, stepperX.MSCNT())) / (steps_per_unit[X_AXIS] * (256 / get_microsteps_x()))));
    }
    case Y_AXIS: {
        return ((Y_HOME_DIR < 0 ? Y_HOME_GAP : -Y_HOME_GAP)
            - ((((INVERT_Y_DIR) ? -1.f : 1.f) * to_calibrated(cal, stepperY.MSCNT())) / (steps_per_unit[Y_AXIS] * (256 / get_microsteps_y()))));
    }
    default:;
    }
    return 0;
}

/**
 * \brief Home and get offset from calibrated point
 *
 * \returns offset from calibrated point -512 .. +512
 */
static int32_t home_and_get_calibration_offset(AxisEnum axis, int axis_home_dir, float &probe_offset, bool store_samples, float fr_mm_s = 0) {
    int32_t calibration_offset = 0;
    bool calibrated = false;
    bool break_loop = false;
    fr_mm_s = fr_mm_s != 0.0f ? fr_mm_s : homing_feedrate_mm_s[axis];

    do {
        const int32_t mscnt = home_and_get_mscnt(axis, axis_home_dir, fr_mm_s / homing_bump_divisor[axis], probe_offset);

        if ((probe_offset >= axis_home_min_diff(axis))
            && (probe_offset <= axis_home_max_diff(axis))
            && store_samples) {
            PersistentStorage::pushHomeSample(mscnt, 255, axis); // todo board_temp
        } else {
            break_loop = true;
        }

        calibration_offset = calibrated_offset_mscnt(axis, mscnt, calibrated);
        if (!calibrated)
            calibration_offset = 0;

        SERIAL_ECHO_START();
        SERIAL_ECHOPAIR("    homing probe offset: ", probe_offset);
        SERIAL_ECHOPAIR(" divisor: ", homing_bump_divisor[axis]);
        SERIAL_ECHOPAIR(" mscnt: ", mscnt);
        if (calibrated) {
            SERIAL_ECHOLNPAIR(" calibration offset: ", calibration_offset);
        } else {
            ui.status_printf_P(0, "Calibrating %c axis", axis_codes[axis]);
            // I _could_ use SERIAL_ECHOLN(), but that somehow cuts off the end
            // of the printed string. Very nice.
            SERIAL_ECHOLNPAIR(" Not yet", " calibrated.");
        }

    } while (store_samples && !calibrated && !break_loop);

    return calibration_offset;
}

static void load_divisor_from_eeprom() {
    for (int axis = 0; axis < XY; axis++) {
        const float max = homing_bump_divisor_max[axis];
        const float min = homing_bump_divisor_min[axis];
        const float hbd = homing_bump_divisor[axis];
        if (hbd >= min && hbd <= max) {
            continue;
        }

        const float loaded = axis ? config_store().homing_bump_divisor_y.get() : config_store().homing_bump_divisor_x.get();
        if (loaded >= min && loaded <= max) {
            homing_bump_divisor[axis] = loaded;
        } else {
            homing_bump_divisor[axis] = homing_bump_divisor_dflt[axis];
        }
    }
}

static void homing_failed_update_divisor(AxisEnum axis) {
    homing_bump_divisor[axis] *= HOMING_BUMP_DIVISOR_STEP;
    const float max = homing_bump_divisor_max[axis];
    const float min = homing_bump_divisor_min[axis];
    const float hbd = homing_bump_divisor[axis];
    if (hbd > max || /* shouldnt happen, just to make sure */ hbd < min) {
        homing_bump_divisor[axis] = min;
    }
}

static void save_divisor_to_eeprom(int try_nr, AxisEnum axis) {
    if (try_nr > 0 && axis < XY) {
        if (axis) {
            config_store().homing_bump_divisor_y.set(homing_bump_divisor[axis]);
        } else {
            config_store().homing_bump_divisor_x.set(homing_bump_divisor[axis]);
        }
    }
}

#if PRINTER_IS_PRUSA_MK4
inline constexpr uint8_t HOMING_SENSITIVITY_CALIBRATION_TRIES = 4;

static void store_homing_sensitivity(AxisEnum axis, int16_t value) {
    switch (axis) {
    case X_AXIS:
        config_store().homing_sens_x.set(value);
        break;
    case Y_AXIS:
        config_store().homing_sens_y.set(value);
        break;
    default:
        assert(false && "invalid axis index");
    }
}

class SensitivityCalibration {
    struct AvgData {
        float probe_offset_avg { 0.0 };
        uint8_t n { 0 };
    };

    AxisEnum axis;
    std::array<AvgData, XY_STALL_SENSITIVITY_MAX - XY_STALL_SENSITIVITY_MIN + 1> avgs;
    int16_t current_sensitivity { XY_STALL_SENSITIVITY_MIN }; // start at the lowest sensitivity
    bool calibrated { false };

    size_t s2i(int16_t sensitivity) {
        return sensitivity - XY_STALL_SENSITIVITY_MIN;
    }

    int16_t i2s(size_t index) {
        return index + XY_STALL_SENSITIVITY_MIN;
    }

public:
    SensitivityCalibration(AxisEnum axis, bool force_recalibration = false)
        : axis(axis) {
        SERIAL_ECHO_START();
        SERIAL_ECHO("Homing sensitivity: ");
        if (crash_s.home_sensitivity[axis] != config_store_ns::stallguard_sensitivity_unset) {
            if (force_recalibration) {
                SERIAL_ECHOLNPAIR("forcing recalibration, sensitivity ", current_sensitivity);
                crash_s.home_sensitivity[axis] = current_sensitivity;
            } else {
                SERIAL_ECHOLNPAIR("already calibrated to ", crash_s.home_sensitivity[axis]);
                calibrated = true;
            }
        } else {
            SERIAL_ECHOLNPAIR("starting calibration at sensitivity ", current_sensitivity);
            crash_s.home_sensitivity[axis] = current_sensitivity;
        }
    }

    void update_probe_offset_avg(float probe_offset) {
        auto &data = avgs[s2i(current_sensitivity)];

        data.probe_offset_avg = data.probe_offset_avg + (abs(probe_offset) - data.probe_offset_avg) / ++data.n;

        // If the probe offset is off by more than 6mm after a couple of tries,
        // it's way too sensitive, move on to next sensitivity early
        if ((data.n >= HOMING_SENSITIVITY_CALIBRATION_TRIES / 2 && data.probe_offset_avg > 6)
            || data.n >= HOMING_SENSITIVITY_CALIBRATION_TRIES) {
            current_sensitivity++; // move on to the next sensitivity
            crash_s.home_sensitivity[axis] = current_sensitivity;

            if (current_sensitivity > XY_STALL_SENSITIVITY_MAX) {
                auto it = std::min_element(
                    avgs.begin(),
                    avgs.end(),
                    [](const AvgData &a, const AvgData &b) { return a.probe_offset_avg < b.probe_offset_avg; });

                current_sensitivity = i2s(it - avgs.begin());
                crash_s.home_sensitivity[axis] = current_sensitivity;

                SERIAL_ECHO_START();
                SERIAL_ECHOPAIR("Homing sensitivity: calibrated to ", current_sensitivity);
                SERIAL_ECHOLNPAIR(" of probe offset avg: ", it->probe_offset_avg);
                store_homing_sensitivity(axis, current_sensitivity);
                calibrated = true;
            } else {
                SERIAL_ECHO_START();
                SERIAL_ECHOLNPAIR("Homing sensitivity: calibrating at sensitivity ", current_sensitivity);
            }
        }
    }

    bool is_calibrated() { return calibrated; }

    int8_t get_current_sensitivity() { return current_sensitivity; }
};
#endif

/**
 * \brief Home and decide if position of both probes is close enough to calibrated home position
 *
 * Do homing probe and decide if can be accepted. If it is not
 * good enough do the probe again up to PRECISE_HOMING_TRIES times.
 *
 * \param axis axis to be homed (cartesian printers only)
 * \param axis_home_dir direction where the home of the axis is
 * \param can_calibrate Can be calibrated home position updated?
 * \return Distance between two probes in mm.
 */
float home_axis_precise(AxisEnum axis, int axis_home_dir, bool can_calibrate, float fr_mm_s) {
    const int tries = can_calibrate ? PRECISE_HOMING_TRIES : (3 * PRECISE_HOMING_TRIES);
    int accept_perfect_only_tries = can_calibrate ? 3 : 3 * 3;
    constexpr int perfect_offset = 96;
    constexpr int acceptable_offset = 288;
    float probe_offset;
    bool first_acceptable = false;
    [[maybe_unused]] bool done_sens_calib_reset = false;

    load_divisor_from_eeprom();

    for (int try_nr = 0; try_nr < tries; ++try_nr) {
        SERIAL_ECHO_START();
        SERIAL_ECHOPAIR("== Precise Homing axis ", axis_codes[axis]);
        SERIAL_ECHOPAIR(" try ", try_nr);
        SERIAL_ECHOLN(" ==");

#if PRINTER_IS_PRUSA_MK4
        // If homing is failing, try to recalibrate sensitivity. We do this
        // after we couldn't home perfectly, and increase the perfect only
        // tries so that we still try to home perfectly after recalibrating
        // the sensitivity.
        bool reset_sens_calibration = false;
        if (can_calibrate && try_nr >= accept_perfect_only_tries && !done_sens_calib_reset) {
            reset_sens_calibration = true;
            done_sens_calib_reset = true;
            accept_perfect_only_tries *= 2;
        }

        SensitivityCalibration sens_calibration { axis, reset_sens_calibration };

        while (can_calibrate && !sens_calibration.is_calibrated()) {
            home_and_get_calibration_offset(axis, axis_home_dir, probe_offset, false, fr_mm_s);
            sens_calibration.update_probe_offset_avg(probe_offset);
        }
#endif

        const int32_t calibration_offset = home_and_get_calibration_offset(axis, axis_home_dir, probe_offset, can_calibrate, fr_mm_s);
        if (planner.draining()) {
            // homing intentionally aborted, do not retry
            break;
        }

        SERIAL_ECHO_START();
        SERIAL_ECHO("Probe classified as ");

        if ((probe_offset < axis_home_min_diff(axis))
            || (probe_offset > axis_home_max_diff(axis))) {
            SERIAL_ECHOLN("failed.");
            ui.status_printf_P(0, "%c axis homing failed, retrying", axis_codes[axis]);
            homing_failed_update_divisor(axis);
        } else if (std::abs(calibration_offset) <= perfect_offset) {
            SERIAL_ECHOLN("perfect.");
            save_divisor_to_eeprom(try_nr, axis);
            return probe_offset;
        } else if ((std::abs(calibration_offset) <= acceptable_offset)) {
            SERIAL_ECHOLN("acceptable.");
            if (try_nr >= accept_perfect_only_tries) {
                save_divisor_to_eeprom(try_nr, axis);
                return probe_offset;
            }
            if (first_acceptable) {
                ui.status_printf_P(0, "Updating precise home point %c axis", axis_codes[axis]);
                homing_failed_update_divisor(axis);
            }
            first_acceptable = true;
        } else {
            SERIAL_ECHOLN("bad.");
            if (can_calibrate) {
                ui.status_printf_P(0, "Updating precise home point %c axis", axis_codes[axis]);
            } else {
                ui.status_printf_P(0, "%c axis homing failed,retrying", axis_codes[axis]);
            }
        }
    }

    SERIAL_ERROR_MSG("Precise homing runs out of tries to get acceptable probe.");
    return probe_offset;
}
