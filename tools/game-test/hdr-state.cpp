#include <sdkddkver.h>
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Failure : std::runtime_error {
  DWORD code;
  Failure(const char* operation, DWORD error)
      : std::runtime_error(operation), code(error ? error : ERROR_GEN_FAILURE) {}
};

template <typename T>
void Header(T& packet, DISPLAYCONFIG_DEVICE_INFO_TYPE type, LUID adapter, UINT32 id) {
  packet.header.type = type;
  packet.header.size = sizeof(packet);
  packet.header.adapterId = adapter;
  packet.header.id = id;
}

std::string LuidString(LUID id) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(8)
      << static_cast<UINT32>(id.HighPart) << std::setw(8) << id.LowPart;
  return out.str();
}

struct Target {
  LUID adapter;
  UINT32 id;
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color{};
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacyColor{};
  DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
  DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology{};
};

bool Matches(const Target& target, LUID adapter, UINT32 id) {
  return target.adapter.HighPart == adapter.HighPart &&
         target.adapter.LowPart == adapter.LowPart && target.id == id;
}

std::vector<Target> Query() {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  bool complete = false;
  // A concurrent topology change can invalidate the buffer sizes.
  for (int attempt = 0; attempt < 8; ++attempt) {
    UINT32 pathCount = 0, modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS) throw Failure("GetDisplayConfigBufferSizes", result);
    paths.resize(pathCount);
    modes.resize(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                &modeCount, modes.data(), nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER) continue;
    if (result != ERROR_SUCCESS) throw Failure("QueryDisplayConfig", result);
    paths.resize(pathCount);
    complete = true;
    break;
  }
  if (!complete) throw Failure("display topology kept changing", ERROR_RETRY);

  std::vector<Target> targets;
  for (const auto& path : paths) {
    const auto& info = path.targetInfo;
    if (std::any_of(targets.begin(), targets.end(), [&](const Target& item) {
          return Matches(item, info.adapterId, info.id);
        })) continue;
    Target target{info.adapterId, info.id};
    Header(target.color, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2,
           target.adapter, target.id);
    const LONG result = DisplayConfigGetDeviceInfo(&target.color.header);
    if (result != ERROR_SUCCESS) throw Failure("DisplayConfigGetDeviceInfo(INFO_2)", result);
    Header(target.legacyColor, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
           target.adapter, target.id);
    const LONG legacyResult = DisplayConfigGetDeviceInfo(&target.legacyColor.header);
    if (legacyResult != ERROR_SUCCESS) throw Failure("DisplayConfigGetDeviceInfo(INFO)", legacyResult);
    Header(target.name, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, target.adapter, target.id);
    const LONG nameResult = DisplayConfigGetDeviceInfo(&target.name.header);
    if (nameResult != ERROR_SUCCESS) throw Failure("DisplayConfigGetDeviceInfo(TARGET_NAME)", nameResult);
    target.outputTechnology = info.outputTechnology;
    targets.push_back(target);
  }
  return targets;
}

const char* ModeName(DISPLAYCONFIG_ADVANCED_COLOR_MODE mode) {
  switch (mode) {
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR: return "SDR";
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG: return "WCG";
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR: return "HDR";
    default: return "unknown";
  }
}

template <size_t N>
std::string JsonString(const WCHAR (&text)[N]) {
  std::ostringstream out;
  out << '"' << std::hex << std::setfill('0');
  for (const WCHAR ch : text) {
    if (!ch) break;
    // Escape UTF-16 code units, including surrogate pairs, as JSON Unicode.
    if (ch < 0x20 || ch >= 0x7f || ch == '"' || ch == '\\')
      out << "\\u" << std::setw(4) << static_cast<unsigned int>(ch);
    else
      out << static_cast<char>(ch);
  }
  out << '"';
  return out.str();
}

void PrintTargets(const std::vector<Target>& targets) {
  std::cout << '[';
  bool first = true;
  for (const auto& target : targets) {
    if (!first) std::cout << ',';
    first = false;
    const auto& c = target.color;
    std::cout << "{\"adapterLUID\":\"" << LuidString(target.adapter)
              << "\",\"adapterLUIDHighPart\":" << target.adapter.HighPart
              << ",\"adapterLUIDLowPart\":" << target.adapter.LowPart
              << ",\"targetId\":" << target.id
              << ",\"active\":true,\"highDynamicRangeSupported\":" << bool(c.highDynamicRangeSupported)
              << ",\"highDynamicRangeUserEnabled\":" << bool(c.highDynamicRangeUserEnabled)
              << ",\"advancedColorLimitedByPolicy\":" << bool(c.advancedColorLimitedByPolicy)
              << ",\"advancedColorSupported\":" << bool(c.advancedColorSupported)
              << ",\"advancedColorActive\":" << bool(c.advancedColorActive)
              << ",\"wideColorSupported\":" << bool(c.wideColorSupported)
              << ",\"wideColorUserEnabled\":" << bool(c.wideColorUserEnabled)
              << ",\"activeColorMode\":" << static_cast<UINT32>(c.activeColorMode)
              << ",\"activeColorModeName\":\"" << ModeName(c.activeColorMode)
              << "\",\"colorEncoding\":" << static_cast<UINT32>(c.colorEncoding)
              << ",\"bitsPerColorChannel\":" << c.bitsPerColorChannel
              << ",\"rawValue\":" << c.value
              << ",\"legacyAdvancedColorInfo\":{\"type\":"
              << static_cast<UINT32>(target.legacyColor.header.type)
              << ",\"rawValue\":" << target.legacyColor.value
              << ",\"advancedColorSupported\":" << bool(target.legacyColor.advancedColorSupported)
              << ",\"advancedColorEnabled\":" << bool(target.legacyColor.advancedColorEnabled)
              << ",\"advancedColorForceDisabled\":" << bool(target.legacyColor.advancedColorForceDisabled)
              << ",\"wideColorEnforced\":" << bool(target.legacyColor.wideColorEnforced)
              << "},\"outputTechnology\":" << static_cast<UINT32>(target.outputTechnology)
              << ",\"targetNameOutputTechnology\":" << static_cast<UINT32>(target.name.outputTechnology)
              << ",\"monitorFriendlyDeviceName\":" << JsonString(target.name.monitorFriendlyDeviceName)
              << '}';
  }
  std::cout << ']';
}

UINT32 ParseNumber(const std::string& text, int base) {
  UINT32 value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw Failure("invalid numeric target identifier", ERROR_INVALID_PARAMETER);
  return value;
}
}  // namespace

int main(int argc, char** argv) {
  std::cout << std::boolalpha;
  try {
    if (argc == 2 && std::string(argv[1]) == "query") {
      const auto targets = Query();
      std::cout << "{\"ok\":true,\"command\":\"query\",\"targets\":";
      PrintTargets(targets);
      std::cout << "}\n";
      return 0;
    }
    if (argc != 8 || std::string(argv[1]) != "set" ||
        std::string(argv[2]) != "--adapter-luid" ||
        std::string(argv[4]) != "--target-id" ||
        std::string(argv[6]) != "--enabled")
      throw Failure("usage: query | set --adapter-luid <16 hex digits> --target-id <uint32> --enabled <true|false>", ERROR_INVALID_PARAMETER);
    const std::string luid = argv[3], enabled = argv[7];
    if (luid.size() != 16 || (enabled != "true" && enabled != "false"))
      throw Failure("LUID must contain 16 hex digits; enabled must be true or false", ERROR_INVALID_PARAMETER);
    LUID adapter{};
    const UINT32 high = ParseNumber(luid.substr(0, 8), 16);
    // Preserve all 64 identifier bits, including a signed HighPart.
    static_assert(sizeof(high) == sizeof(adapter.HighPart));
    CopyMemory(&adapter.HighPart, &high, sizeof(high));
    adapter.LowPart = ParseNumber(luid.substr(8), 16);
    const UINT32 id = ParseNumber(argv[5], 10);
    const bool requested = enabled == "true";
    auto before = Query();
    const auto target = std::find_if(before.begin(), before.end(), [&](const Target& item) {
      return Matches(item, adapter, id);
    });
    if (target == before.end()) throw Failure("target is not in current active query", ERROR_NOT_FOUND);
    if (!target->color.highDynamicRangeSupported || target->color.advancedColorLimitedByPolicy)
      throw Failure("target does not support HDR or advanced color is limited by policy", ERROR_ACCESS_DENIED);
    DISPLAYCONFIG_SET_HDR_STATE state{};
    Header(state, DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE, adapter, id);
    state.enableHdr = requested;
    const LONG result = DisplayConfigSetDeviceInfo(&state.header);
    if (result != ERROR_SUCCESS) throw Failure("DisplayConfigSetDeviceInfo(SET_HDR_STATE)", result);

    // User preference and active composition mode are independent; verify both.
    std::vector<Target> after;
    bool verified = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
      after = Query();
      const auto current = std::find_if(after.begin(), after.end(), [&](const Target& item) {
        return Matches(item, adapter, id);
      });
      if (current == after.end()) throw Failure("target disappeared after set", ERROR_NOT_FOUND);
      const bool hdrActive = current->color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
      if (bool(current->color.highDynamicRangeUserEnabled) == requested && hdrActive == requested) {
        verified = true;
        break;
      }
      if (attempt < 19) Sleep(250);
    }
    std::cout << "{\"ok\":" << verified << ",\"command\":\"set\",\"setApplied\":true,\"verified\":"
              << verified << ",\"adapterLUID\":\"" << LuidString(adapter)
              << "\",\"targetId\":" << id << ",\"requestedEnabled\":" << requested
              << ",\"before\":";
    PrintTargets(before);
    std::cout << ",\"after\":";
    PrintTargets(after);
    std::cout << "}\n";
    return verified ? 0 : ERROR_INVALID_STATE;
  } catch (const Failure& error) {
    // Messages are fixed ASCII literals, never unescaped user input.
    std::cout << "{\"ok\":false,\"error\":\"" << error.what()
              << "\",\"win32Error\":" << error.code << "}\n";
    return static_cast<int>(error.code);
  } catch (const std::exception&) {
    std::cout << "{\"ok\":false,\"error\":\"unexpected C++ failure\"}\n";
    return ERROR_GEN_FAILURE;
  }
}
