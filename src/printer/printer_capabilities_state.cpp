// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_capabilities_state.cpp
 * @brief Printer capabilities state management extracted from PrinterState
 *
 * Manages capability subjects that control UI feature visibility based on
 * hardware detection and user overrides. Extracted from PrinterState as
 * part of god class decomposition.
 */

#include "printer_capabilities_state.h"

#include "ui_update_queue.h"

#include "sound_manager.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterCapabilitiesState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCapabilitiesState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCapabilitiesState] Initializing subjects (register_xml={})",
                  register_xml);

    // Printer capability subjects (all default to 0=not available)
    INIT_SUBJECT_INT(printer_has_qgl, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_z_tilt, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_bed_mesh, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_nozzle_clean, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_probe, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_heater_bed, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_led, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_accelerometer, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_spoolman, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_speaker, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_timelapse, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_shell_command, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_purge_line, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_firmware_retraction, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_bed_moves, 0, subjects_, register_xml); // 0=gantry moves, 1=bed moves
    INIT_SUBJECT_INT(printer_has_chamber_sensor, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber_heater, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_screws_tilt, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_webcam, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_extra_fans, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(power_device_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(sensor_count, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterCapabilitiesState] Subjects initialized successfully");
}

void PrinterCapabilitiesState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterCapabilitiesState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterCapabilitiesState::set_hardware(const PrinterDiscovery& hardware,
                                            const CapabilityOverrides& overrides) {
    // Update subjects using effective values (auto-detect + user overrides)
    // This allows users to force-enable features that weren't detected
    // (e.g., heat soak macro without chamber heater) or force-disable
    // features they don't want to see in the UI.
    lv_subject_set_int(&printer_has_qgl_, overrides.has_qgl() ? 1 : 0);
    lv_subject_set_int(&printer_has_z_tilt_, overrides.has_z_tilt() ? 1 : 0);
    lv_subject_set_int(&printer_has_bed_mesh_, overrides.has_bed_mesh() ? 1 : 0);
    lv_subject_set_int(&printer_has_nozzle_clean_, overrides.has_nozzle_clean() ? 1 : 0);

    // Hardware capabilities (no user override support yet - set directly from detection)
    spdlog::debug("[PrinterCapabilitiesState] has_probe={} has_led={} has_accel={}",
                  hardware.has_probe(), hardware.has_led(), hardware.has_accelerometer());
    lv_subject_set_int(&printer_has_probe_, hardware.has_probe() ? 1 : 0);
    lv_subject_set_int(&printer_has_heater_bed_, hardware.has_heater_bed() ? 1 : 0);
    lv_subject_set_int(&printer_has_led_, hardware.has_led() ? 1 : 0);
    lv_subject_set_int(&printer_has_accelerometer_, hardware.has_accelerometer() ? 1 : 0);

    // Install M300 (Klipper gcode beeper) backend now that we know whether
    // the printer's Klipper config actually has a beeper output_pin. This
    // MUST happen before flipping printer_has_speaker_ so any UI/handlers
    // observing the subject see a working backend. SoundManager no-ops if
    // a real audio backend (SDL/ALSA/PWM) is already installed.
    if (hardware.has_speaker()) {
        SoundManager::instance().try_install_m300_backend();
    }

    // Speaker capability — uses override system so presets can disable it
    // for printers without speakers (e.g., K1C has no beeper/buzzer).
    // AUTO mode: true if hardware beeper detected OR local sound backend exists.
    lv_subject_set_int(&printer_has_speaker_, overrides.has_speaker() ? 1 : 0);

    // Timelapse capability (Moonraker-Timelapse plugin)
    lv_subject_set_int(&printer_has_timelapse_, hardware.has_timelapse() ? 1 : 0);

    // Firmware retraction capability (for G10/G11 retraction settings)
    lv_subject_set_int(&printer_has_firmware_retraction_,
                       hardware.has_firmware_retraction() ? 1 : 0);

    // Chamber temperature sensor and heater capabilities
    lv_subject_set_int(&printer_has_chamber_sensor_, hardware.has_chamber_sensor() ? 1 : 0);
    lv_subject_set_int(&printer_has_chamber_heater_, hardware.has_chamber_heater() ? 1 : 0);
    lv_subject_set_int(&printer_has_chamber_,
                       (hardware.has_chamber_sensor() || hardware.has_chamber_heater()) ? 1 : 0);

    // Screws tilt adjust capability
    lv_subject_set_int(&printer_has_screws_tilt_, hardware.has_screws_tilt() ? 1 : 0);

    // Spoolman requires async check - default to 0, updated separately via set_spoolman_available()

    spdlog::debug("[PrinterCapabilitiesState] Hardware set: probe={}, heater_bed={}, LED={}, "
                  "accelerometer={}, speaker={}, timelapse={}, fw_retraction={}, chamber_sensor={}",
                  hardware.has_probe(), hardware.has_heater_bed(), hardware.has_led(),
                  hardware.has_accelerometer(), hardware.has_speaker(), hardware.has_timelapse(),
                  hardware.has_firmware_retraction(), hardware.has_chamber_sensor());
    spdlog::debug("[PrinterCapabilitiesState] Hardware set (with overrides): {}",
                  overrides.summary());
}

void PrinterCapabilitiesState::set_sound_backend_available(bool available) {
    if (available && lv_subject_get_int(&printer_has_speaker_) == 0) {
        lv_subject_set_int(&printer_has_speaker_, 1);
        spdlog::debug("[PrinterCapabilitiesState] Sound backend available, speaker enabled");
    }
}

void PrinterCapabilitiesState::set_spoolman_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    helix::ui::queue_update([this, available]() {
        lv_subject_set_int(&printer_has_spoolman_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Spoolman availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_webcam_available(bool available, const std::string& stream_url,
                                                    const std::string& snapshot_url, bool flip_h,
                                                    bool flip_v, int target_fps) {
    // Store URLs before queuing (captured by value for thread safety)
    helix::ui::queue_update([this, available, stream_url, snapshot_url, flip_h, flip_v,
                             target_fps]() {
        webcam_stream_url_ = available ? stream_url : "";
        webcam_snapshot_url_ = available ? snapshot_url : "";
        webcam_flip_h_ = flip_h;
        webcam_flip_v_ = flip_v;
        webcam_target_fps_ = target_fps > 0 ? target_fps : 15;
        lv_subject_set_int(&printer_has_webcam_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Webcam: available={}, stream_url={}, flip_h={}, "
                      "flip_v={}, target_fps={}",
                      available, stream_url, flip_h, flip_v, webcam_target_fps_);
    });
}

void PrinterCapabilitiesState::set_timelapse_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    helix::ui::queue_update([this, available]() {
        lv_subject_set_int(&printer_has_timelapse_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Timelapse availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_shell_command_available(bool available) {
    helix::ui::queue_update([this, available]() {
        lv_subject_set_int(&printer_has_shell_command_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] shell_command component available: {}",
                      available);
    });
}

void PrinterCapabilitiesState::set_purge_line(bool has_purge_line) {
    lv_subject_set_int(&printer_has_purge_line_, has_purge_line ? 1 : 0);
    spdlog::debug("[PrinterCapabilitiesState] Purge line capability set: {}", has_purge_line);
}

void PrinterCapabilitiesState::set_bed_moves(bool bed_moves) {
    int new_value = bed_moves ? 1 : 0;
    // Only log when value actually changes (this gets called frequently from status updates)
    if (lv_subject_get_int(&printer_bed_moves_) != new_value) {
        lv_subject_set_int(&printer_bed_moves_, new_value);
        spdlog::info("[PrinterCapabilitiesState] Bed moves on Z: {}", bed_moves);
    }
}

void PrinterCapabilitiesState::set_has_chamber_sensor(bool available) {
    lv_subject_set_int(&printer_has_chamber_sensor_, available ? 1 : 0);
    update_has_chamber();
}

void PrinterCapabilitiesState::set_has_chamber_heater(bool available) {
    lv_subject_set_int(&printer_has_chamber_heater_, available ? 1 : 0);
    update_has_chamber();
}

void PrinterCapabilitiesState::update_has_chamber() {
    bool has_any = lv_subject_get_int(&printer_has_chamber_sensor_) != 0 ||
                   lv_subject_get_int(&printer_has_chamber_heater_) != 0;
    lv_subject_set_int(&printer_has_chamber_, has_any ? 1 : 0);
}

void PrinterCapabilitiesState::set_power_device_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    helix::ui::queue_update([this, count]() {
        lv_subject_set_int(&power_device_count_, count);
        spdlog::debug("[PrinterCapabilitiesState] Power device count set: {}", count);
    });
}

void PrinterCapabilitiesState::set_sensor_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    helix::ui::queue_update([this, count]() {
        lv_subject_set_int(&sensor_count_, count);
        spdlog::debug("[PrinterCapabilitiesState] Sensor count set: {}", count);
    });
}

} // namespace helix
