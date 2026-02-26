#include "offsets.h"

#include <dolos/offset_cache.h>
#include <dolos/offset_registry.h>
#include <dolos/pattern_scanner.h>
#include <dolos/pe_builder.h>
#include <dolos/pipe_log.h>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <string>

namespace d2r {

using dolos::OffsetCache;
using dolos::OffsetCacheManager;
using dolos::PatternScanner;
using dolos::PEBuilder;
using dolos::SignatureDef;

namespace {

std::vector<SignatureDef> BuildSignatureList() {
  std::vector<SignatureDef> signatures;

#define ADD_SIGNATURE(...)                                                                                             \
  signatures.push_back({                                                                                               \
      D2R_GET_NAME(__VA_ARGS__),                                                                                       \
      D2R_GET_PATTERN(__VA_ARGS__),                                                                                    \
      D2R_GET_TYPE(__VA_ARGS__),                                                                                       \
      (void**)&D2R_GET_VAR(__VA_ARGS__),                                                                               \
      0,                                                                                                               \
      std::nullopt, /* parsed pattern - lazy init */                                                                   \
  });
  D2R_OFFSET_LIST(ADD_SIGNATURE)
#undef ADD_SIGNATURE

  return signatures;
}

void ApplyCachedOffsets(const OffsetCache& cache, std::vector<SignatureDef>& signatures) {
  HMODULE module = GetModuleHandle(NULL);
  uint64_t module_base = reinterpret_cast<uint64_t>(module);
  for (const auto& entry : cache.entries) {
    auto it = std::find_if(
        signatures.begin(), signatures.end(), [&entry](const SignatureDef& sig) { return entry.name == sig.name; });
    if (it != signatures.end()) {
      *it->target = reinterpret_cast<void*>(module_base + entry.offset);
    }
  }
}

OffsetCache BuildCache(std::uint64_t exe_hash, std::uint32_t sig_hash, const std::vector<SignatureDef>& signatures) {
  OffsetCache cache;
  cache.exe_hash = exe_hash;
  cache.signature_hash = sig_hash;

  for (const auto& sig : signatures) {
    cache.entries.push_back({
        sig.name,
        sig.offset,
    });
  }

  return cache;
}

void RegisterOffsetsWithDolos() {
#define REGISTER_OFFSET(...) dolos::RegisterOffset(D2R_GET_NAME(__VA_ARGS__), D2R_GET_VAR(__VA_ARGS__));
  D2R_OFFSET_LIST(REGISTER_OFFSET)
#undef REGISTER_OFFSET
}

void LogMissingOffsets(const std::vector<SignatureDef>& signatures) {
  std::size_t missing_count = 0;
  for (const auto& sig : signatures) {
    if (*sig.target == nullptr) {
      ++missing_count;
    }
  }

  if (missing_count == 0) {
    return;
  }

  PIPE_LOG_WARN("[Offsets] {} unresolved offsets:", missing_count);
  for (const auto& sig : signatures) {
    if (*sig.target == nullptr) {
      PIPE_LOG_WARN("[Offsets]   - {} | pattern: {}", sig.name, sig.pattern);
    }
  }
}

constexpr std::uint32_t kPlayerIdCacheMagic = 0x43444950;  // "PIDC"
constexpr std::uint32_t kPlayerIdCacheVersion = 2;

struct PlayerIdConstantsCacheFile {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint64_t exe_hash;
  std::uint32_t xor_const;
  std::uint32_t add_const;
};

std::string GetPlayerIdConstantsCachePath(OffsetCacheManager& cache_mgr, std::uint64_t exe_hash) {
  std::string path = cache_mgr.GetCachePath(exe_hash);
  if (path.size() > 4 && path.substr(path.size() - 4) == ".bin") {
    path = path.substr(0, path.size() - 4) + ".playerid.bin";
  } else {
    path += ".playerid.bin";
  }
  return path;
}

bool LoadPlayerIdConstantsFromCache(OffsetCacheManager& cache_mgr, std::uint64_t exe_hash) {
  const std::string cache_path = GetPlayerIdConstantsCachePath(cache_mgr, exe_hash);
  std::ifstream file(cache_path, std::ios::binary);
  if (!file) {
    return false;
  }

  PlayerIdConstantsCacheFile data{};
  file.read(reinterpret_cast<char*>(&data), sizeof(data));
  if (!file) {
    PIPE_LOG_WARN("[PlayerIdConstants] Failed reading cache file {}", cache_path);
    return false;
  }

  if (data.magic != kPlayerIdCacheMagic || data.version != kPlayerIdCacheVersion || data.exe_hash != exe_hash) {
    return false;
  }

  PlayerIdXorConst = data.xor_const;
  PlayerIdAddConst = data.add_const;
  PIPE_LOG_INFO("[PlayerIdConstants] Loaded cached XOR=0x{:08X} ADD=0x{:08X}", PlayerIdXorConst, PlayerIdAddConst);
  return true;
}

bool SavePlayerIdConstantsToCacheInternal(OffsetCacheManager& cache_mgr,
                                          std::uint64_t exe_hash,
                                          std::uint32_t xor_const,
                                          std::uint32_t add_const) {
  if (exe_hash == 0) {
    return false;
  }
  if (!cache_mgr.EnsureCacheDirectory()) {
    return false;
  }

  const std::string cache_path = GetPlayerIdConstantsCachePath(cache_mgr, exe_hash);
  std::ofstream file(cache_path, std::ios::binary | std::ios::trunc);
  if (!file) {
    PIPE_LOG_WARN("[PlayerIdConstants] Failed creating cache file {}", cache_path);
    return false;
  }

  PlayerIdConstantsCacheFile data{
      kPlayerIdCacheMagic,
      kPlayerIdCacheVersion,
      exe_hash,
      xor_const,
      add_const,
  };

  file.write(reinterpret_cast<const char*>(&data), sizeof(data));
  if (!file) {
    PIPE_LOG_WARN("[PlayerIdConstants] Failed writing cache file {}", cache_path);
    return false;
  }

  PIPE_LOG_DEBUG("[PlayerIdConstants] Cached XOR=0x{:08X} ADD=0x{:08X}", xor_const, add_const);
  return true;
}

}  // namespace

bool InitializeOffsets() {
  PIPE_LOG_INFO("[Offsets] Initializing...");

  auto signatures = BuildSignatureList();

  if (signatures.empty()) {
    PIPE_LOG_WARN("[Offsets] No offsets defined in D2R_OFFSET_LIST");
    return true;  // Not an error, just nothing to do
  }

  PIPE_LOG_DEBUG("[Offsets] {} offsets to resolve", signatures.size());

  OffsetCacheManager cache_mgr;
  std::uint64_t exe_hash = cache_mgr.ComputeExecutableHash();
  std::uint32_t sig_hash = cache_mgr.ComputeSignatureHash(signatures);

  if (exe_hash == 0) {
    PIPE_LOG_WARN("[Offsets] Failed to compute executable hash, caching disabled");
  }

  if (exe_hash != 0) {
    auto cached = cache_mgr.LoadCache(exe_hash, sig_hash);
    if (cached.has_value()) {
      PIPE_LOG_DEBUG("[Offsets] Applying cached offsets...");
      ApplyCachedOffsets(*cached, signatures);

      if (ValidateOffsets()) {
        PIPE_LOG_INFO("[Offsets] Loaded {} offsets from cache", signatures.size());
        RegisterOffsetsWithDolos();
        return true;
      }

      PIPE_LOG_DEBUG("[Offsets] Cache validation failed, rescanning...");
    }
  }

  PIPE_LOG_DEBUG("[Offsets] Performing full pattern scan...");

  PatternScanner scanner;
  if (!scanner.Initialize()) {
    PIPE_LOG_ERROR("[Offsets] Failed to initialize pattern scanner");
    return false;
  }

  if (!scanner.ScanAll(signatures)) {
    PIPE_LOG_WARN("[Offsets] Not all patterns were found");
  }

#ifdef NYX_D2R_PE_DUMP
  if (exe_hash != 0) {
    std::string dump_path = cache_mgr.GetCachePath(exe_hash);
    if (dump_path.size() > 4 && dump_path.substr(dump_path.size() - 4) == ".bin") {
      dump_path = dump_path.substr(0, dump_path.size() - 4) + ".exe";
    } else {
      dump_path += ".exe";
    }

    PEBuilder builder(scanner.module_base(), scanner.module_size());
    for (const auto& sec : scanner.sections()) {
      builder.AddSection(sec);
    }
    if (!builder.WriteExecutable(scanner.buffer(), dump_path)) {
      PIPE_LOG_WARN("[Offsets] Failed to write PE dump");
    }
  }
#endif  // NYX_D2R_PE_DUMP

  std::size_t found_count = 0;
  for (const auto& sig : signatures) {
    if (*sig.target != nullptr) {
      ++found_count;
    }
  }

  PIPE_LOG_INFO("[Offsets] Resolved {}/{} offsets", found_count, signatures.size());
  if (found_count != signatures.size()) {
    LogMissingOffsets(signatures);
  }

  if (exe_hash != 0 && found_count > 0) {
    auto cache = BuildCache(exe_hash, sig_hash, signatures);
    if (cache_mgr.SaveCache(cache)) {
      PIPE_LOG_DEBUG("[Offsets] Offsets cached for future use");
    }
  }

  RegisterOffsetsWithDolos();
  return found_count == signatures.size();
}

bool ValidateOffsets() {
#define VALIDATE_OFFSET(...)                                                                                           \
  if (D2R_GET_VAR(__VA_ARGS__) == nullptr) return false;
  D2R_OFFSET_LIST(VALIDATE_OFFSET)
#undef VALIDATE_OFFSET

  return true;
}

bool InitializePlayerIdConstants() {
  // Prefer validated cached constants by exe hash.
  //
  // We intentionally avoid trusting an initialization-time pattern scan result
  // here because false positives can select invalid immediates and destabilize
  // later calls. Runtime code validates candidates against real unit lookups
  // and persists only validated values.

  OffsetCacheManager cache_mgr;
  std::uint64_t exe_hash = cache_mgr.ComputeExecutableHash();
  if (exe_hash != 0 && LoadPlayerIdConstantsFromCache(cache_mgr, exe_hash)) {
    return true;
  }

  // Bootstrap constants: runtime validation/recovery may replace these and
  // cache validated values for future launches.
  PlayerIdXorConst = 0x8633C320;
  PlayerIdAddConst = 0x53D5CDD3;
  PIPE_LOG_WARN("[PlayerIdConstants] No validated cache found, using bootstrap constants");
  return true;
}

bool SavePlayerIdConstantsToCache(uint32_t xor_const, uint32_t add_const) {
  OffsetCacheManager cache_mgr;
  std::uint64_t exe_hash = cache_mgr.ComputeExecutableHash();
  return SavePlayerIdConstantsToCacheInternal(cache_mgr, exe_hash, xor_const, add_const);
}

void GetOffsetInfo(OffsetInfo* out, std::size_t count) {
  if (count == 0 || out == nullptr) {
    return;
  }

  std::size_t i = 0;

#define FILL_INFO(...)                                                                                                 \
  if (i < count) {                                                                                                     \
    out[i].name = D2R_GET_NAME(__VA_ARGS__);                                                                           \
    out[i].pattern = D2R_GET_PATTERN(__VA_ARGS__);                                                                     \
    out[i].type = D2R_GET_TYPE(__VA_ARGS__);                                                                           \
    out[i].value = D2R_GET_VAR(__VA_ARGS__);                                                                           \
    out[i].found = D2R_GET_VAR(__VA_ARGS__) != nullptr;                                                                \
    ++i;                                                                                                               \
  }
  D2R_OFFSET_LIST(FILL_INFO)
#undef FILL_INFO
}

}  // namespace d2r
