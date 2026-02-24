#pragma once

#include <algorithm>
#include <optional>
#include <regex>
#include <string>

#include "../codec/CodecConfig.h"

namespace strmctrl {

struct VideoConfigRequest {
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> fps;
};

inline std::string trimCopy(const std::string& text)
{
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

inline std::optional<int> findIntField(const std::string& text, const std::string& key)
{
    const std::regex re("\\\"?" + key + "\\\"?\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(text, match, re) && match.size() >= 2) {
        return std::stoi(match[1].str());
    }
    return std::nullopt;
}

inline std::optional<std::string> findStringField(const std::string& text, const std::string& key)
{
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (std::regex_search(text, match, re) && match.size() >= 2) {
        return match[1].str();
    }
    return std::nullopt;
}

inline VideoConfigRequest parseVideoConfigRequest(const std::string& payload)
{
    VideoConfigRequest req;
    req.width  = findIntField(payload, "width");
    req.height = findIntField(payload, "height");
    req.fps    = findIntField(payload, "fps");
    return req;
}

inline std::string serializeVideoConfigRequest(const VideoConfigRequest& req)
{
    std::string out = "{";
    bool first = true;
    if (req.width.has_value()) {
        out += "\"width\":" + std::to_string(*req.width);
        first = false;
    }
    if (req.height.has_value()) {
        if (!first) out += ",";
        out += "\"height\":" + std::to_string(*req.height);
        first = false;
    }
    if (req.fps.has_value()) {
        if (!first) out += ",";
        out += "\"fps\":" + std::to_string(*req.fps);
    }
    out += "}";
    return out;
}

inline CodecConfig applyVideoRequestWithCaps(const CodecConfig& base,
                                             const VideoConfigRequest& req)
{
    CodecConfig result = base;
    if (req.width.has_value()) {
        result.width = std::min(*req.width, 1920);
    }
    if (req.height.has_value()) {
        result.height = std::min(*req.height, 1920);
    }
    if (req.fps.has_value()) {
        result.fps = std::min(*req.fps, 60);
    }
    return result;
}

inline std::string serializeVideoConfig(const CodecConfig& cfg)
{
    return "{\"width\":" + std::to_string(cfg.width) +
           ",\"height\":" + std::to_string(cfg.height) +
           ",\"fps\":" + std::to_string(cfg.fps) +
           ",\"bitrate_kbps\":" + std::to_string(cfg.bitrate_kbps) +
           ",\"codec\":\"" + cfg.codec_name + "\"}";
}

inline std::optional<CodecConfig> parseVideoConfig(const std::string& payload)
{
    CodecConfig cfg;
    const auto width = findIntField(payload, "width");
    const auto height = findIntField(payload, "height");
    const auto fps = findIntField(payload, "fps");
    const auto bitrate = findIntField(payload, "bitrate_kbps");
    const auto codec = findStringField(payload, "codec");

    if (width.has_value()) cfg.width = *width;
    if (height.has_value()) cfg.height = *height;
    if (fps.has_value()) cfg.fps = *fps;
    if (bitrate.has_value()) cfg.bitrate_kbps = *bitrate;
    if (codec.has_value()) cfg.codec_name = *codec;

    return cfg;
}

} // namespace strmctrl
