// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "hv/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class AmsBackendMedusaHc : public AmsSubscriptionBackend {
  public:
    AmsBackendMedusaHc(MoonrakerAPI* api, helix::MoonrakerClient* client);

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::MEDUSA_HC;
    }
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;
    [[nodiscard]] bool is_bypass_active() const override;
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info) const override;

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS MedusaHC]";
    }

  private:
    struct ToolState {
        bool docked = false;     ///< Tool is parked on its base (medusahc.tool{N}_docked)
        std::string color_hex;   ///< User-assigned slot color (HelixScreen-side, set_slot_info)
    };
    void apply_status_snapshot(const nlohmann::json& status);
    void apply_medusahc_object_locked(const nlohmann::json& obj, bool& changed);
    void ensure_tool_capacity_locked(int tool_count);
    void sync_system_info_locked();
    AmsError validate_slot_index_locked(int slot_index) const;
    static bool parse_boolish(const nlohmann::json& value);
    static std::optional<uint32_t> parse_color(const std::string& color);
    static std::optional<int> parse_tool_docked_index(const std::string& key);

    std::vector<ToolState> tools_;
    std::string medusahc_state_; ///< medusahc.state: changing/ready/error/uninitialized
    int current_tool_ = -1; ///< medusahc.current_tool (-1=none on head, -2=sensor error)
    int target_tool_ = -1;  ///< medusahc.target_tool (in-flight change target)
    bool error_state_ = false;
    bool feeder_open_ = false;
};
