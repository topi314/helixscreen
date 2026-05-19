// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_file_api.h"

#include "ui_error_reporting.h"

#include "moonraker_api_internal.h"
#include "moonraker_client.h"
#include "spdlog/spdlog.h"

#include <sstream>

using namespace moonraker_internal;

// ============================================================================
// MoonrakerFileAPI Implementation
// ============================================================================

MoonrakerFileAPI::MoonrakerFileAPI(helix::MoonrakerClient& client) : client_(client) {}

// ============================================================================
// File Management Operations
// ============================================================================

void MoonrakerFileAPI::list_files(const std::string& root, const std::string& path, bool recursive,
                                  FileListCallback on_success, ErrorCallback on_error) {
    // Validate root parameter
    if (reject_invalid_identifier(root, "list_files", on_error))
        return;

    // Validate path if provided
    if (!path.empty() && reject_invalid_path(path, "list_files", on_error))
        return;

    json params = {{"root", root}};

    if (!path.empty()) {
        params["path"] = path;
    }

    if (recursive) {
        params["extended"] = true;
    }

    spdlog::debug("[FileAPI] Listing files in {}/{}", root, path);

    client_.send_jsonrpc(
        "server.files.list", params,
        [this, on_success, on_error](json response) {
            try {
                std::vector<FileInfo> files = parse_file_list(response);
                spdlog::trace("[FileAPI] Found {} files", files.size());
                on_success(files);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse file list: {}", e.what());
                report_parse_error(on_error, "server.files.list", e.what());
            }
        },
        on_error);
}

void MoonrakerFileAPI::get_directory(const std::string& root, const std::string& path,
                                     FileListCallback on_success, ErrorCallback on_error) {
    // Validate root
    if (reject_invalid_identifier(root, "get_directory", on_error))
        return;

    // Validate path if provided
    if (!path.empty() && reject_invalid_path(path, "get_directory", on_error))
        return;

    // Build the full path for the request
    std::string full_path = root;
    if (!path.empty()) {
        full_path += "/" + path;
    }

    json params = {{"path", full_path}};

    spdlog::debug("[FileAPI] Sending server.files.get_directory for path='{}'", full_path);

    client_.send_jsonrpc(
        "server.files.get_directory", params,
        [this, full_path, on_success, on_error](json response) {
            try {
                std::vector<FileInfo> files = parse_file_list(response);
                spdlog::debug("[FileAPI] get_directory response for '{}': {} items", full_path,
                              files.size());
                on_success(files);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse directory '{}': {}", full_path, e.what());
                report_parse_error(on_error, "server.files.get_directory", e.what());
            }
        },
        [full_path, on_error](const MoonrakerError& error) {
            spdlog::error("[FileAPI] get_directory FAILED for '{}': {} ({})", full_path,
                          error.message, error.get_type_string());
            if (on_error) {
                on_error(error);
            }
        });
}

void MoonrakerFileAPI::get_file_metadata(const std::string& filename,
                                         FileMetadataCallback on_success, ErrorCallback on_error,
                                         bool silent) {
    // Validate filename path
    if (reject_invalid_path(filename, "get_file_metadata", on_error, silent))
        return;

    json params = {{"filename", filename}};

    spdlog::trace("[FileAPI] Getting metadata for file: {}", filename);

    client_.send_jsonrpc(
        "server.files.metadata", params,
        [this, on_success, on_error](json response) {
            try {
                FileMetadata metadata = parse_file_metadata(response);
                on_success(metadata);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse file metadata: {}", e.what());
                report_parse_error(on_error, "server.files.metadata", e.what());
            }
        },
        on_error,
        0,     // timeout_ms: use default
        silent // silent: suppress RPC_ERROR events
    );
}

void MoonrakerFileAPI::metascan_file(const std::string& filename, FileMetadataCallback on_success,
                                     ErrorCallback on_error, bool silent) {
    // Validate filename path
    if (reject_invalid_path(filename, "metascan_file", on_error, silent))
        return;

    json params = {{"filename", filename}};

    spdlog::debug("[FileAPI] Triggering metascan for file: {}", filename);

    client_.send_jsonrpc(
        "server.files.metascan", params,
        [this, on_success, on_error, filename](json response) {
            try {
                FileMetadata metadata = parse_file_metadata(response);
                spdlog::debug("[FileAPI] Metascan successful for: {}", filename);
                on_success(metadata);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse metascan response: {}", e.what());
                report_parse_error(on_error, "server.files.metascan", e.what());
            }
        },
        on_error,
        0,     // timeout_ms: use default
        silent // silent: suppress RPC_ERROR events (default true)
    );
}

void MoonrakerFileAPI::delete_file(const std::string& filename, SuccessCallback on_success,
                                   ErrorCallback on_error) {
    // Validate filename path
    if (reject_invalid_path(filename, "delete_file", on_error))
        return;

    json params = {{"path", filename}};

    spdlog::info("[FileAPI] Deleting file: {}", filename);

    client_.send_jsonrpc(
        "server.files.delete_file", params,
        [on_success](json) {
            spdlog::info("[FileAPI] File deleted successfully");
            on_success();
        },
        on_error);
}

void MoonrakerFileAPI::move_file(const std::string& source, const std::string& dest,
                                 SuccessCallback on_success, ErrorCallback on_error) {
    // Validate source path
    if (reject_invalid_path(source, "move_file", on_error))
        return;

    // Validate destination path
    if (reject_invalid_path(dest, "move_file", on_error))
        return;

    spdlog::info("[FileAPI] Moving file from {} to {}", source, dest);

    json params = {{"source", source}, {"dest", dest}};

    client_.send_jsonrpc(
        "server.files.move", params,
        [on_success](json) {
            spdlog::info("[FileAPI] File moved successfully");
            on_success();
        },
        on_error);
}

void MoonrakerFileAPI::copy_file(const std::string& source, const std::string& dest,
                                 SuccessCallback on_success, ErrorCallback on_error) {
    // Validate source path
    if (reject_invalid_path(source, "copy_file", on_error))
        return;

    // Validate destination path
    if (reject_invalid_path(dest, "copy_file", on_error))
        return;

    spdlog::info("[FileAPI] Copying file from {} to {}", source, dest);

    json params = {{"source", source}, {"dest", dest}};

    client_.send_jsonrpc(
        "server.files.copy", params,
        [on_success](json) {
            spdlog::info("[FileAPI] File copied successfully");
            on_success();
        },
        on_error);
}

void MoonrakerFileAPI::create_directory(const std::string& path, SuccessCallback on_success,
                                        ErrorCallback on_error) {
    // Validate path
    if (reject_invalid_path(path, "create_directory", on_error))
        return;

    spdlog::info("[FileAPI] Creating directory: {}", path);

    json params = {{"path", path}};

    client_.send_jsonrpc(
        "server.files.post_directory", params,
        [on_success](json) {
            spdlog::info("[FileAPI] Directory created successfully");
            on_success();
        },
        on_error);
}

void MoonrakerFileAPI::delete_directory(const std::string& path, bool force,
                                        SuccessCallback on_success, ErrorCallback on_error) {
    // Validate path
    if (reject_invalid_path(path, "delete_directory", on_error))
        return;

    spdlog::info("[FileAPI] Deleting directory: {} (force: {})", path, force);

    json params = {{"path", path}, {"force", force}};

    client_.send_jsonrpc(
        "server.files.delete_directory", params,
        [on_success](json) {
            spdlog::info("[FileAPI] Directory deleted successfully");
            on_success();
        },
        on_error);
}

// ============================================================================
// File List/Metadata Parsing
// ============================================================================

std::vector<FileInfo> MoonrakerFileAPI::parse_file_list(const json& response) {
    std::vector<FileInfo> files;

    if (!response.contains("result")) {
        return files;
    }

    const json& result = response["result"];

    // Moonraker returns a flat array of file/directory objects in "result"
    // Each object has: path, modified, size, permissions
    // Directories are NOT returned by server.files.list - only by server.files.get_directory
    if (result.is_array()) {
        for (const auto& item : result) {
            FileInfo info;
            if (item.contains("path")) {
                info.path = item["path"].get<std::string>();
                // filename is the last component of the path
                size_t last_slash = info.path.rfind('/');
                info.filename = (last_slash != std::string::npos) ? info.path.substr(last_slash + 1)
                                                                  : info.path;
            } else if (item.contains("filename")) {
                info.filename = item["filename"].get<std::string>();
            }
            if (item.contains("size")) {
                info.size = item["size"].get<uint64_t>();
            }
            if (item.contains("modified")) {
                info.modified = item["modified"].get<double>();
            }
            if (item.contains("permissions")) {
                info.permissions = item["permissions"].get<std::string>();
            }
            info.is_dir = false; // server.files.list only returns files
            files.push_back(info);
        }
        return files;
    }

    // Legacy format: result is an object with "dirs" and "files" arrays
    // (may be used by server.files.get_directory or older Moonraker versions)
    if (result.contains("dirs")) {
        for (const auto& dir : result["dirs"]) {
            FileInfo info;
            if (dir.contains("dirname")) {
                info.filename = dir["dirname"].get<std::string>();
                info.is_dir = true;
            }
            if (dir.contains("modified")) {
                info.modified = dir["modified"].get<double>();
            }
            if (dir.contains("permissions")) {
                info.permissions = dir["permissions"].get<std::string>();
            }
            files.push_back(info);
        }
    }

    if (result.contains("files")) {
        for (const auto& file : result["files"]) {
            FileInfo info;
            if (file.contains("filename")) {
                info.filename = file["filename"].get<std::string>();
            }
            if (file.contains("path")) {
                info.path = file["path"].get<std::string>();
            }
            if (file.contains("size")) {
                info.size = file["size"].get<uint64_t>();
            }
            if (file.contains("modified")) {
                info.modified = file["modified"].get<double>();
            }
            if (file.contains("permissions")) {
                info.permissions = file["permissions"].get<std::string>();
            }
            info.is_dir = false;
            files.push_back(info);
        }
    }

    return files;
}

FileMetadata MoonrakerFileAPI::parse_file_metadata(const json& response) {
    FileMetadata metadata;

    if (!response.contains("result")) {
        return metadata;
    }

    const json& result = response["result"];

    // Helper lambdas to safely extract values (Moonraker returns null for missing metadata)
    auto get_string = [&result](const char* key) -> std::string {
        if (result.contains(key) && result[key].is_string()) {
            return result[key].get<std::string>();
        }
        return {};
    };

    auto get_double = [&result](const char* key) -> double {
        if (result.contains(key) && result[key].is_number()) {
            return result[key].get<double>();
        }
        return 0.0;
    };

    auto get_uint64 = [&result](const char* key) -> uint64_t {
        if (result.contains(key) && result[key].is_number()) {
            return result[key].get<uint64_t>();
        }
        return 0;
    };

    auto get_uint32 = [&result](const char* key) -> uint32_t {
        if (result.contains(key) && result[key].is_number()) {
            return result[key].get<uint32_t>();
        }
        return 0;
    };

    // Basic file info
    metadata.filename = get_string("filename");
    metadata.size = get_uint64("size");
    metadata.modified = get_double("modified");

    // Slicer info
    metadata.slicer = get_string("slicer");
    metadata.slicer_version = get_string("slicer_version");

    // Print info
    metadata.print_start_time = get_double("print_start_time");
    metadata.job_id = get_string("job_id");
    metadata.layer_count = get_uint32("layer_count");
    metadata.object_height = get_double("object_height");
    metadata.estimated_time = get_double("estimated_time");

    // Filament info
    metadata.filament_total = get_double("filament_total");
    metadata.filament_weight_total = get_double("filament_weight_total");

    // Per-tool filament weights / usage. Multi-format parser handles slicer
    // variance. Empty result means "unknown" — caller must NOT treat as all-zero.
    metadata.filament_weights = moonraker_internal::parse_filament_weights(result);
    if (!metadata.filament_weights.empty()) {
        spdlog::trace("[FileAPI] Parsed {} per-tool filament weights",
                      metadata.filament_weights.size());
    }

    // Normalize filament_type: may be semicolon string, JSON array, or stringified array
    metadata.filament_type = moonraker_internal::json_string_list_or(result, "filament_type");
    // Full filament name — use first entry only for display
    std::string all_names = moonraker_internal::json_string_list_or(result, "filament_name");
    if (!all_names.empty()) {
        size_t semicolon = all_names.find(';');
        metadata.filament_name =
            (semicolon != std::string::npos) ? all_names.substr(0, semicolon) : all_names;
    }
    // Layer height info
    metadata.layer_height = get_double("layer_height");
    metadata.first_layer_height = get_double("first_layer_height");

    // Filament colors (array of hex strings from slicer metadata)
    // Newer Moonraker versions return "filament_colors" as a JSON array.
    if (result.contains("filament_colors") && result["filament_colors"].is_array()) {
        for (const auto& color : result["filament_colors"]) {
            if (color.is_string()) {
                metadata.filament_colors.push_back(color.get<std::string>());
            }
        }
        if (!metadata.filament_colors.empty()) {
            spdlog::trace("[FileAPI] Found {} filament colors (array)",
                          metadata.filament_colors.size());
        }
    }

    // Fallback: "filament_colour" as semicolon-separated string (e.g., "#FF0000;#00FF00")
    // Standard Moonraker returns this field name from slicer metadata comments.
    if (metadata.filament_colors.empty()) {
        std::string colour_str = get_string("filament_colour");
        if (!colour_str.empty()) {
            std::istringstream ss(colour_str);
            std::string token;
            while (std::getline(ss, token, ';')) {
                // Trim leading/trailing whitespace
                size_t start = token.find_first_not_of(" \t");
                size_t end = token.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    metadata.filament_colors.push_back(token.substr(start, end - start + 1));
                }
            }
            if (!metadata.filament_colors.empty()) {
                spdlog::trace("[FileAPI] Found {} filament colors (filament_colour string)",
                              metadata.filament_colors.size());
            }
        }
    }

    // Temperature info
    metadata.first_layer_bed_temp = get_double("first_layer_bed_temp");
    metadata.first_layer_extr_temp = get_double("first_layer_extr_temp");

    // G-code info
    metadata.gcode_start_byte = get_uint64("gcode_start_byte");
    metadata.gcode_end_byte = get_uint64("gcode_end_byte");

    // UUID for history matching (slicer-generated unique identifier)
    metadata.uuid = get_string("uuid");

    // Thumbnails - parse with dimensions for selecting largest
    if (result.contains("thumbnails") && result["thumbnails"].is_array()) {
        for (const auto& thumb : result["thumbnails"]) {
            if (thumb.contains("relative_path") && thumb["relative_path"].is_string()) {
                ThumbnailInfo info;
                info.relative_path = thumb["relative_path"].get<std::string>();
                if (thumb.contains("width") && thumb["width"].is_number()) {
                    info.width = thumb["width"].get<int>();
                }
                if (thumb.contains("height") && thumb["height"].is_number()) {
                    info.height = thumb["height"].get<int>();
                }
                metadata.thumbnails.push_back(info);
                spdlog::trace("[FileAPI] Found thumbnail {}x{}: {}", info.width, info.height,
                              info.relative_path);
            }
        }
    }

    return metadata;
}
