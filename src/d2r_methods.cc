#include "d2r_methods.h"

#include <dolos/pipe_log.h>
#include "d2r_structs.h"
#include "offsets.h"

#include <Windows.h>

#include <bit>
#include <map>
#include <unordered_set>

// Dear Blizzard,
//
// Adding ret checks in every automap function wont stop us. Try harder.
//
// Sincerely, everyone.

namespace d2r {

D2UnitStrc* GetUnit(uint32_t id, uint32_t type);

namespace {
constexpr uint32_t kLegacyPlayerIdXorConst = 0x8633C320u;
constexpr uint32_t kLegacyPlayerIdAddConst = 0x53D5CDD3u;
constexpr std::size_t kMaxUnitChainTraversal = 8192;
constexpr ULONGLONG kPlayerIdCacheHitWindowMs = 3000;
constexpr uint32_t kPlayerIdCacheCommitHits = 3;
constexpr ULONGLONG kDirectLocalPlayerScanIntervalMs = 250;
constexpr ULONGLONG kRevealCircuitWindowMs = 10000;
constexpr uint32_t kRevealCircuitMaxStrikes = 6;

struct CircuitBreakerState {
  const char* name;
  bool tripped = false;
  ULONGLONG window_start_ms = 0;
  uint32_t strikes = 0;
};

struct PlayerIdCandidateState {
  uint32_t xor_const = 0;
  uint32_t add_const = 0;
  uint32_t hits = 0;
  ULONGLONG last_hit_ms = 0;
  bool committed = false;
};

struct LocalPlayerIdentityState {
  uint32_t cached_id = 0;
  ULONGLONG last_scan_ms = 0;
  bool logged_direct_path = false;
};

CircuitBreakerState s_reveal_circuit{"RevealFeature"};
PlayerIdCandidateState s_player_id_candidate{};
LocalPlayerIdentityState s_local_player_identity{};
RuntimeMode s_runtime_mode = RuntimeMode::ReadOnlySafe;

D2UnitStrc* TryGetUnitNoThrow(uint32_t id, uint32_t type);

bool ShouldLogNow(ULONGLONG* last_ms, ULONGLONG interval_ms) {
  if (last_ms == nullptr) {
    return true;
  }
  ULONGLONG now = GetTickCount64();
  if (now - *last_ms >= interval_ms) {
    *last_ms = now;
    return true;
  }
  return false;
}

void RecordCircuitStrike(CircuitBreakerState* state, const char* reason) {
  if (state == nullptr || state->tripped) {
    return;
  }
  ULONGLONG now = GetTickCount64();
  if (state->window_start_ms == 0 || now - state->window_start_ms > kRevealCircuitWindowMs) {
    state->window_start_ms = now;
    state->strikes = 0;
  }
  ++state->strikes;
  if (state->strikes >= kRevealCircuitMaxStrikes) {
    state->tripped = true;
    PIPE_LOG_ERROR("[{}] Circuit breaker tripped (reason: {})", state->name, reason ? reason : "unknown");
  } else {
    static ULONGLONG s_last_circuit_log_ms = 0;
    if (ShouldLogNow(&s_last_circuit_log_ms, 3000)) {
      PIPE_LOG_WARN("[{}] Circuit strike {}/{} ({})",
                    state->name,
                    state->strikes,
                    kRevealCircuitMaxStrikes,
                    reason ? reason : "unknown");
    }
  }
}

bool IsCircuitTripped(const CircuitBreakerState* state) {
  if (state == nullptr) {
    return false;
  }
  if (state->tripped) {
    static ULONGLONG s_last_log_ms = 0;
    if (ShouldLogNow(&s_last_log_ms, 5000)) {
      PIPE_LOG_WARN("[{}] Circuit breaker active, skipping call", state->name);
    }
    return true;
  }
  return false;
}

const char* RuntimeModeToString(RuntimeMode mode) {
  switch (mode) {
    case RuntimeMode::ReadOnlySafe:
      return "read_only_safe";
    case RuntimeMode::ActiveMutation:
      return "active_mutation";
    default:
      return "unknown";
  }
}

bool IsMutationBlockedByMode(const char* caller) {
  if (s_runtime_mode == RuntimeMode::ActiveMutation) {
    return false;
  }
  static ULONGLONG s_last_log_ms = 0;
  if (ShouldLogNow(&s_last_log_ms, 5000)) {
    PIPE_LOG_WARN("[{}] Blocked by runtime mode: {}", caller ? caller : "Mutation", RuntimeModeToString(s_runtime_mode));
  }
  return true;
}

void RememberLocalPlayerId(uint32_t id) {
  if (id == 0) {
    return;
  }
  s_local_player_identity.cached_id = id;
}

bool TryResolveSinglePlayerId(uint32_t* out_id) {
  if (out_id == nullptr || sgptClientSideUnitHashTable == nullptr) {
    return false;
  }
#if defined(_MSC_VER)
  __try {
#endif
    EntityHashTable* client_units = sgptClientSideUnitHashTable;
    uint32_t single_id = 0;
    bool found_single = false;

    for (size_t i = 0; i < kUnitHashTableCount; ++i) {
      std::size_t traversed = 0;
      D2UnitStrc* last_node = nullptr;
      for (D2UnitStrc* current = client_units[0][i]; current; current = current->pUnitNext) {
        if (++traversed > kMaxUnitChainTraversal) {
          break;
        }
        if (current == last_node) {
          break;
        }
        last_node = current;
        uint32_t id = current->dwId;
        if (id == 0) {
          continue;
        }
        if (!found_single) {
          single_id = id;
          found_single = true;
          continue;
        }
        if (id != single_id) {
          return false;
        }
      }
    }

    if (!found_single) {
      return false;
    }
    *out_id = single_id;
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

bool TryGetDirectLocalPlayerId(uint32_t* out_id) {
  if (out_id == nullptr) {
    return false;
  }

  if (s_local_player_identity.cached_id != 0) {
    if (TryGetUnitNoThrow(s_local_player_identity.cached_id, 0) != nullptr) {
      *out_id = s_local_player_identity.cached_id;
      return true;
    }
    s_local_player_identity.cached_id = 0;
  }

  ULONGLONG now = GetTickCount64();
  if (now - s_local_player_identity.last_scan_ms < kDirectLocalPlayerScanIntervalMs) {
    return false;
  }
  s_local_player_identity.last_scan_ms = now;

  uint32_t direct_id = 0;
  if (!TryResolveSinglePlayerId(&direct_id)) {
    return false;
  }
  if (TryGetUnitNoThrow(direct_id, 0) == nullptr) {
    return false;
  }

  s_local_player_identity.cached_id = direct_id;
  *out_id = direct_id;
  if (!s_local_player_identity.logged_direct_path) {
    s_local_player_identity.logged_direct_path = true;
    PIPE_LOG_INFO("[LocalPlayerIdentity] Using direct local-player unit identity path");
  }
  return true;
}

void ObservePlayerIdCandidateForCache(uint32_t xor_val, uint32_t add_val) {
  ULONGLONG now = GetTickCount64();
  bool same_candidate = (s_player_id_candidate.xor_const == xor_val && s_player_id_candidate.add_const == add_val &&
                         now - s_player_id_candidate.last_hit_ms <= kPlayerIdCacheHitWindowMs);
  if (same_candidate) {
    ++s_player_id_candidate.hits;
  } else {
    s_player_id_candidate.xor_const = xor_val;
    s_player_id_candidate.add_const = add_val;
    s_player_id_candidate.hits = 1;
    s_player_id_candidate.committed = false;
  }
  s_player_id_candidate.last_hit_ms = now;

  if (!s_player_id_candidate.committed && s_player_id_candidate.hits >= kPlayerIdCacheCommitHits) {
    s_player_id_candidate.committed = true;
    if (SavePlayerIdConstantsToCache(xor_val, add_val)) {
      PIPE_LOG_INFO("[PlayerIdConstants] Cached validated runtime constants after {} confirmations", kPlayerIdCacheCommitHits);
    }
  }
}

bool TryDecodePlayerIdWithConstants(uint32_t index, uint32_t xor_const, uint32_t add_const, uint32_t* out_id) {
  if (out_id == nullptr) {
    return false;
  }

#if defined(_MSC_VER)
  __try {
#endif
    if (EncEncryptionKeys == nullptr || PlayerIndexToIDEncryptedTable == nullptr || EncTransformValue == nullptr) {
      return false;
    }

    uintptr_t keys_base = *EncEncryptionKeys;
    if (keys_base == 0) {
      return false;
    }

    uint32_t key = *(uint32_t*)(keys_base + 0x146);
    uint32_t encrypted = PlayerIndexToIDEncryptedTable[index];
    uint32_t temp = (encrypted ^ key ^ xor_const) + add_const;
    uint32_t v = std::rotl(std::rotl(temp, 9), 7);
    // transform doesn't seem to do anything, keep for now but can probably be removed.
    uint32_t id = EncTransformValue(&v);
    if (id == 0xFFFFFFFFu) {
      id = 0;
    }
    *out_id = id;
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

inline uint32_t DecodePlayerIdWithConstants(uint32_t index, uint32_t xor_const, uint32_t add_const) {
  uint32_t id = 0;
  if (!TryDecodePlayerIdWithConstants(index, xor_const, add_const, &id)) {
    return 0;
  }
  return id;
}

D2UnitStrc* TryGetUnitNoThrow(uint32_t id, uint32_t type) {
#if defined(_MSC_VER)
  __try {
    return GetUnit(id, type);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
#else
  return GetUnit(id, type);
#endif
}

bool HasAnyPlayerUnits() {
  if (sgptClientSideUnitHashTable == nullptr) {
    return false;
  }
#if defined(_MSC_VER)
  __try {
#endif
    EntityHashTable* client_units = sgptClientSideUnitHashTable;
    for (size_t i = 0; i < kUnitHashTableCount; ++i) {
      if (client_units[0][i] != nullptr) {
        return true;
      }
    }
    return false;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

bool IsUnsafeStateForInvasiveCall(const char* caller) {
  bool unsafe = false;
  if (sgptClientSideUnitHashTable == nullptr) {
    unsafe = true;
  } else if (s_PlayerUnitIndex == nullptr || *s_PlayerUnitIndex >= 8) {
    unsafe = true;
  } else if (!HasAnyPlayerUnits()) {
    unsafe = true;
  }

  if (unsafe) {
    static ULONGLONG s_last_log_ms = 0;
    if (ShouldLogNow(&s_last_log_ms, 5000)) {
      PIPE_LOG_WARN("[{}] Skipping invasive call in unsafe runtime state", caller ? caller : "InvasiveCall");
    }
  }
  return unsafe;
}

bool TryRecoverPlayerIdConstantsFromRuntime(uint32_t index, uint32_t* recovered_id) {
  HMODULE module = GetModuleHandle(NULL);
  if (!module) {
    return false;
  }

  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
  const uint8_t* base = reinterpret_cast<const uint8_t*>(module);

  std::unordered_set<uint64_t> seen_candidates;
  auto try_candidate = [&](uint32_t xor_val, uint32_t add_val, const char* source) -> bool {
    uint64_t key = (static_cast<uint64_t>(xor_val) << 32) | add_val;
    if (!seen_candidates.insert(key).second) {
      return false;
    }

    uint32_t candidate_id = 0;
    if (!TryDecodePlayerIdWithConstants(index, xor_val, add_val, &candidate_id)) {
      return false;
    }
    if (candidate_id == 0) {
      return false;
    }
    if (TryGetUnitNoThrow(candidate_id, 0) == nullptr) {
      return false;
    }

    PlayerIdXorConst = xor_val;
    PlayerIdAddConst = add_val;
    ObservePlayerIdCandidateForCache(xor_val, add_val);
    if (recovered_id != nullptr) {
      *recovered_id = candidate_id;
    }
    PIPE_LOG_INFO("[PlayerIdConstants] Recovered runtime constants from {} candidate (xor=0x{:08X} add=0x{:08X})",
                  source,
                  xor_val,
                  add_val);
    return true;
  };

  const auto* section = IMAGE_FIRST_SECTION(nt);
  // Pass 1: strict candidates.
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    const uint8_t* start = base + section->VirtualAddress;
    const uint8_t* end = start + section->Misc.VirtualSize;
    for (const uint8_t* p = start; p + 16 <= end; ++p) {
      if (p[0] != 0x35 || p[5] != 0x05 || p[10] != 0xC1 || p[11] != 0xC0 || p[12] != 0x09 || p[13] != 0xC1 ||
          p[14] != 0xC0 || p[15] != 0x07) {
        continue;
      }
      if (try_candidate(*reinterpret_cast<const uint32_t*>(p + 1), *reinterpret_cast<const uint32_t*>(p + 6), "strict")) {
        return true;
      }
    }
  }

  // Pass 2: relaxed candidates.
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    const uint8_t* start = base + section->VirtualAddress;
    const uint8_t* end = start + section->Misc.VirtualSize;
    for (const uint8_t* p = start; p + 12 <= end; ++p) {
      if (p[0] != 0x35 || p[5] != 0x05 || p[10] != 0xC1 || p[11] != 0xC0) {
        continue;
      }
      if (try_candidate(*reinterpret_cast<const uint32_t*>(p + 1), *reinterpret_cast<const uint32_t*>(p + 6), "relaxed")) {
        return true;
      }
    }
  }

  return false;
}
}  // namespace

D2UnitStrc* GetUnit(uint32_t id, uint32_t type) {
  if (sgptClientSideUnitHashTable == nullptr || type >= kUnitHashTableCount) {
    return nullptr;
  }

#if defined(_MSC_VER)
  __try {
#endif
  EntityHashTable* client_units = sgptClientSideUnitHashTable;
  for (size_t i = id & 0x7F; i < kUnitHashTableCount; ++i) {
    std::size_t traversed = 0;
    D2UnitStrc* current = client_units[type][i];
    for (; current; current = current->pUnitNext) {
      if (++traversed > kMaxUnitChainTraversal) {
        static ULONGLONG s_last_log_ms = 0;
        if (ShouldLogNow(&s_last_log_ms, 5000)) {
          PIPE_LOG_WARN("[GetUnit] Chain traversal limit hit (type={}, bucket={}, id={})", type, i, id);
        }
        break;
      }
      if (current->dwId == id) {
        return current;
      }
    }
  }
  return nullptr;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
#endif
}

uint32_t GetPlayerId(uint32_t index) {
  if (index < 0 || index >= 8) {
    return 0;
  };

  const bool is_local_slot = (s_PlayerUnitIndex != nullptr && index == *s_PlayerUnitIndex);
  if (!is_local_slot || sgptClientSideUnitHashTable == nullptr) {
    uint32_t id = DecodePlayerIdWithConstants(index, PlayerIdXorConst, PlayerIdAddConst);
    return id;
  }

  static bool s_local_player_observed = false;
  uint32_t direct_id = 0;
  if (TryGetDirectLocalPlayerId(&direct_id)) {
    s_local_player_observed = true;
    return direct_id;
  }

  uint32_t id = DecodePlayerIdWithConstants(index, PlayerIdXorConst, PlayerIdAddConst);

  if (id != 0 && TryGetUnitNoThrow(id, 0) != nullptr) {
    s_local_player_observed = true;
    RememberLocalPlayerId(id);
    return id;
  }

  uint32_t legacy_id = DecodePlayerIdWithConstants(index, kLegacyPlayerIdXorConst, kLegacyPlayerIdAddConst);
  if (legacy_id != 0 && TryGetUnitNoThrow(legacy_id, 0) != nullptr) {
    s_local_player_observed = true;
    RememberLocalPlayerId(legacy_id);
    if (PlayerIdXorConst != kLegacyPlayerIdXorConst || PlayerIdAddConst != kLegacyPlayerIdAddConst) {
      PIPE_LOG_WARN(
          "[PlayerIdConstants] Runtime validation failed for current constants "
          "(xor=0x{:08X} add=0x{:08X}), reverting to bootstrap constants",
          PlayerIdXorConst,
          PlayerIdAddConst);
      PlayerIdXorConst = kLegacyPlayerIdXorConst;
      PlayerIdAddConst = kLegacyPlayerIdAddConst;
    }
    return legacy_id;
  }

  // During teardown/loading, player units can be transiently absent.
  // Avoid expensive recovery scans in these states and after we have already
  // observed a valid local player once for this session.
  const bool has_any_player_units = HasAnyPlayerUnits();
  if (s_local_player_observed || !has_any_player_units) {
    if (!has_any_player_units) {
      s_local_player_identity.cached_id = 0;
    }
    return 0;
  }

  // For local slot, try runtime recovery whenever current constants fail to
  // produce a resolvable player unit. This also handles id==0 cases.
  static ULONGLONG s_last_recovery_attempt_ms = 0;
  ULONGLONG now = GetTickCount64();
  if (now - s_last_recovery_attempt_ms >= 1000) {
    s_last_recovery_attempt_ms = now;

    uint32_t recovered_id = 0;
    if (TryRecoverPlayerIdConstantsFromRuntime(index, &recovered_id)) {
      RememberLocalPlayerId(recovered_id);
      return recovered_id;
    }
  }

  return 0;
}

D2UnitStrc* GetPlayerUnit(uint32_t index) {
  uint32_t id = GetPlayerId(index);
  if (id == 0) {
    return nullptr;
  }
  return TryGetUnitNoThrow(id, 0);
}

static void* D2Alloc(size_t size, size_t align = 0x10) {
  auto allocator = *reinterpret_cast<void**>(D2Allocator);
  if (allocator == nullptr) {
    PIPE_LOG("allocator is null");
    // d2 creates the allocator here, because that should never happen for us we just skip it and fail
    return nullptr;
  }
  auto alloc_fn = reinterpret_cast<void* (*)(void*, size_t, size_t)>((*reinterpret_cast<void***>(allocator))[1]);
  return alloc_fn(allocator, sizeof(size), 0x10);
}

static D2AutomapLayerStrc* InitAutomapLayer(int32_t layer_id) {
  D2AutomapLayerStrc* link = *s_automapLayerLink;
  D2AutomapLayerStrc* current = *s_currentAutomapLayer;
  if (link != nullptr) {
    while (link->dwLayerID != layer_id) {
      link = link->prev;
      if (!link) {
        break;
      }
    }
  }
  // allocating a new link bugs out, cba to figure out why, fix me
  if (link == nullptr) {
    return nullptr;
  }
  if (link == nullptr) {
    link = static_cast<D2AutomapLayerStrc*>(D2Alloc(sizeof(D2AutomapLayerStrc)));
    if (link != nullptr) {
      link->dwLayerID = 0;
      link->unk = 0;
      link->visible_floors.head = 0;
      link->visible_floors.sentinel = &link->visible_floors;
      link->visible_floors.tail = &link->visible_floors;
      link->visible_floors.unk = 0;
      link->visible_floors.count = 0;
      link->visible_walls.head = 0;
      link->visible_walls.sentinel = &link->visible_walls;
      link->visible_walls.tail = &link->visible_walls;
      link->visible_walls.unk = 0;
      link->visible_walls.count = 0;
      link->visible_objects.head = 0;
      link->visible_objects.sentinel = &link->visible_objects;
      link->visible_objects.tail = &link->visible_objects;
      link->visible_objects.unk = 0;
      link->visible_objects.count = 0;
      link->visible_extras.head = 0;
      link->visible_extras.sentinel = &link->visible_extras;
      link->visible_extras.tail = &link->visible_extras;
      link->visible_extras.unk = 0;
      link->visible_extras.count = 0;
    }
    // this will crash if link was not allocated, but this is how Blizztard does it...
    // I could do better but... why?
    link->prev = *s_automapLayerLink;
    link->dwLayerID = layer_id;
    *s_automapLayerLink = link;
  }
  if (link != current) {
    return nullptr;  // bugs out, fix me
    PIPE_LOG("Replace automap layer with {:p} old {:p}", static_cast<void*>(link), static_cast<void*>(current));
    if (current) {
      ClearLinkedList(&current->visible_floors);
      ClearLinkedList(&current->visible_walls);
      ClearLinkedList(&current->visible_objects);
      ClearLinkedList(&current->visible_extras);
    }
    *s_currentAutomapLayer = link;
  }
  return link;
}

static void RevealAutomapCells(uint8_t datatbls_index,
                               D2DrlgTileDataStrc* tile_data,
                               D2DrlgRoomStrc* drlg_room,
                               D2LinkedList<D2AutomapCellStrc>* cells) {
  D2LevelDefBin* level_def;

  if ((tile_data->dwFlags & 0x40000) != 0) {
    return;  // already revealed
  }
  tile_data->dwFlags |= 0x40000;  // set revealed flag
  level_def = GetLevelDef(datatbls_index, drlg_room->ptLevel->eLevelId);
  uint32_t cell_id = DATATBLS_GetAutomapCellId(
      level_def->dwLevelType, tile_data->ptTile->nType, tile_data->ptTile->nStyle, tile_data->ptTile->nSequence);

  if (cell_id == -1u) {
    return;  // cell not found
  }

  int32_t x = tile_data->nPosX + drlg_room->tRoomCoords.nBackCornerTileX;
  int32_t y = tile_data->nPosY + drlg_room->tRoomCoords.nBackCornerTileY;
  int32_t absx = 80 * (x - y);
  int32_t absy = (80 * (y + x)) >> 1;
  if (tile_data->nTileCount >= 16) {
    absx += 24;
    absy += 24;
  }

  auto pack_coords = [](int32_t low, int32_t high) -> uint64_t {
    return (static_cast<uint64_t>(high / 10) << 32) | static_cast<uint32_t>(low / 10);
  };
  auto get_low_value = [](uint64_t value) -> int32_t { return (value << 32) >> 32; };
  auto get_high_value = [](uint64_t value) -> int32_t { return value >> 32; };

  int64_t packed = pack_coords(absx, absy);
  if (get_low_value(packed) + 0x8000 > 0xFFFF) {
    PIPE_LOG("low value out of bounds");
    return;
  }
  if (get_high_value(packed) + 0x8000 > 0xFFFF) {
    PIPE_LOG("high value out of bounds");
    return;
  }
  if (cell_id + 0x8000 > 0xFFFF) {
    PIPE_LOG("cell_id out of bounds");
    return;
  }

#pragma pack(push, 1)
  struct D2AutomapInitData {
    uint16_t fSaved;
    uint16_t nCellNo;
    uint64_t nPacked;
  } init_data;
#pragma pack(pop)
  init_data.fSaved = 0;
  init_data.nCellNo = static_cast<uint16_t>(cell_id);
  init_data.nPacked = packed;

  struct Link {
    D2AutomapCellStrc* tail;
    D2AutomapCellStrc** head;
  };
  Link link;
  // PIPE_LOG("new automap cell");
  Link* ret = static_cast<Link*>(AUTOMAP_NewAutomapCell(cells, &link, &init_data));
  if (ret == nullptr) {
    PIPE_LOG("Failed to allocate automap cell");
    return;
  }

  auto prev_next_ptr = ret->head;
  if (ret->head == nullptr) {
    return;
  }

  auto allocator = reinterpret_cast<void* (*)()>(BcAllocator)();
  auto alloc_fn = reinterpret_cast<void* (*)(void*, size_t, size_t)>((*reinterpret_cast<void***>(allocator))[1]);
  D2AutomapCellStrc* new_cell = static_cast<D2AutomapCellStrc*>(alloc_fn(allocator, sizeof(D2AutomapCellStrc), 0x10));

  // PIPE_LOG("increase count");
  cells->count++;

  auto prev_cell = link.tail;
  new_cell->pTail = link.tail;
  new_cell->pHead = 0;
  new_cell->N00000B37 = 0;
  *(uint64_t*)new_cell->pad_0018 = 0;
  new_cell->fSaved = init_data.fSaved;
  new_cell->nCellNo = init_data.nCellNo;
  new_cell->xPixel = get_low_value(packed);
  new_cell->yPixel = get_high_value(packed);

  if (prev_cell == (D2AutomapCellStrc*)cells) {
    cells->head = new_cell;
    cells->sentinel = (D2LinkedList<D2AutomapCellStrc>*)new_cell;
  } else {
    *prev_next_ptr = new_cell;
    if (prev_cell == (D2AutomapCellStrc*)cells->sentinel && prev_next_ptr == &prev_cell->pHead) {
      cells->sentinel = (D2LinkedList<D2AutomapCellStrc>*)new_cell;
    }
    if (prev_cell != (D2AutomapCellStrc*)cells->tail || prev_next_ptr != &prev_cell->N00000B37) {
      AUTOMAP_AddAutomapCell(cells, new_cell);
      return;
    }
  }
  cells->tail = (D2LinkedList<D2AutomapCellStrc>*)new_cell;
  AUTOMAP_AddAutomapCell(cells, new_cell);
}

static void RevealRoom(uint8_t datatbls_index,
                       D2ActiveRoomStrc* hRoom,
                       int32_t reveal_entire_room,
                       D2AutomapLayerStrc* layer) {
  D2DrlgRoomTilesStrc* tiles = hRoom->ptRoomTiles;
  D2DrlgRoomStrc* drlg_room = hRoom->ptDrlgRoom;
  if (tiles && tiles->nFloors > 0) {
    for (uint32_t n = 0; n < tiles->nFloors; ++n) {
      D2DrlgTileDataStrc* tile_data = &tiles->ptFloorTiles[n];
      if ((tile_data->dwFlags & 8) == 0 && (tile_data->dwFlags & 0x20000) != 0 || reveal_entire_room) {
        RevealAutomapCells(datatbls_index, tile_data, drlg_room, &layer->visible_floors);
      }
    }
  }
  if (tiles && tiles->nWalls > 0) {
    for (uint32_t n = 0; n < tiles->nWalls; ++n) {
      D2DrlgTileDataStrc* tile_data = &tiles->ptWallTiles[n];
      if ((tile_data->dwFlags & 8) == 0 && (tile_data->dwFlags & 0x20000) != 0 || reveal_entire_room) {
        RevealAutomapCells(datatbls_index, tile_data, drlg_room, &layer->visible_walls);
      }
    }
  }
  // TODO: RevealAutomapObjects
}

bool AutomapReveal(D2ActiveRoomStrc* hRoom) {
  if (IsMutationBlockedByMode("AutomapReveal")) {
    return false;
  }
  if (IsCircuitTripped(&s_reveal_circuit)) {
    return false;
  }
  if (IsUnsafeStateForInvasiveCall("AutomapReveal")) {
    RecordCircuitStrike(&s_reveal_circuit, "unsafe state");
    return false;
  }
  if (hRoom == nullptr || hRoom->ptDrlgRoom == nullptr || hRoom->ptDrlgRoom->ptLevel == nullptr) {
    return false;
  }

  D2UnitStrc* player = GetPlayerUnit(*s_PlayerUnitIndex);
  if (player == nullptr || player->pDrlgAct == nullptr || player->pDrlgAct->ptDrlg == nullptr) {
    return false;
  }

  uint8_t datatbls_index = 0;
  uint32_t current_layer_id = -1;
  uint32_t level_id = 0;
  D2LevelDefBin* level_def = nullptr;
  D2AutomapLayerStrc* inited = nullptr;
  D2AutomapLayerStrc* current = *s_currentAutomapLayer;

  if (player) {
    datatbls_index = player->nDataTblsIndex;
  } else {
    // datatbls_index = *0x1D44ACF
  }
  if (current) {
    current_layer_id = current->dwLayerID;
  }
  if (hRoom) {
    level_id = hRoom->ptDrlgRoom->ptLevel->eLevelId;
  }

  level_def = GetLevelDef(datatbls_index, level_id);
  inited = InitAutomapLayer(level_def->dwLayer);
  if (inited == nullptr) {
    return false;
  }
  // PIPE_LOG("inited = {:p}", static_cast<void*>(inited));
  RevealRoom(datatbls_index, hRoom, 1, inited);
  if (current_layer_id != -1) {
    // PIPE_LOG("init back previous layer");
    InitAutomapLayer(current_layer_id);
  }
  return true;
}

bool RevealLevelById(uint32_t id) {
  if (id <= 0 || id >= 137) {
    return false;
  }
  if (IsMutationBlockedByMode("RevealLevelById")) {
    return false;
  }
  if (IsCircuitTripped(&s_reveal_circuit)) {
    return false;
  }
  if (IsUnsafeStateForInvasiveCall("RevealLevelById")) {
    RecordCircuitStrike(&s_reveal_circuit, "unsafe state");
    return false;
  }

  D2UnitStrc* player = GetPlayerUnit(*s_PlayerUnitIndex);
  if (player == nullptr) {
    PIPE_LOG("No player");
    return false;
  }

  D2DrlgActStrc* drlg_act = player->pDrlgAct;
  if (drlg_act == nullptr) {
    PIPE_LOG("No DRLG act");
    return false;
  }

  D2DrlgStrc* drlg = drlg_act->ptDrlg;
  if (drlg == nullptr) {
    PIPE_LOG("No DRLG");
    return false;
  }

  D2DrlgLevelStrc* level;
  for (level = drlg->ptLevel; level; level = level->ptNextLevel) {
    if (level->eLevelId == id && level->tCoords.nBackCornerTileX > 0) {
      break;
    }
  }
  if (level == nullptr) {
    // alloc level
    level = DRLG_AllocLevel(player->nDataTblsIndex, drlg, id);
    if (level == nullptr) {
      PIPE_LOG("Failed to allocate level");
      return false;  // failed to alloc
    }
  }
  if (level->ptRoomFirst == nullptr) {
    // temp fix cba to find load act
    std::map<uint32_t, uint32_t> town_ids = {
        {0, 1},
        {1, 40},
        {2, 75},
        {3, 103},
        {4, 109},
        {5, 137},
    };
    if (id < town_ids[drlg_act->dwActId] || id >= town_ids[drlg_act->dwActId + 1]) {
      PIPE_LOG("Unsupported revealing level in another act ({})", id);
      return false;
    }
    reinterpret_cast<void (*)(uint8_t, D2DrlgLevelStrc*)>(DRLG_InitLevel)(player->nDataTblsIndex, level);
    if (level->ptRoomFirst == nullptr) {
      PIPE_LOG("Failed to init level");
      return false;  // failed to init level
    }
  }
  RetcheckFunction pfnAutomap(reinterpret_cast<void (*)(D2ActiveRoomStrc*)>(drlg->pfnAutomap));
  for (D2DrlgRoomStrc* drlg_room = level->ptRoomFirst; drlg_room; drlg_room = drlg_room->ptDrlgRoomNext) {
    if (drlg_room->hRoom == nullptr) {
      ROOMS_AddRoomData(player->nDataTblsIndex,
                        drlg_room->ptLevel->ptDrlg->ptAct,
                        drlg_room->ptLevel->eLevelId,
                        drlg_room->tRoomCoords.nBackCornerTileX,
                        drlg_room->tRoomCoords.nBackCornerTileY,
                        drlg_room->hRoom);
    }
    if (drlg_room->hRoom == nullptr) {
      PIPE_LOG("Failed to add room data");
      return false;
    }
    pfnAutomap(drlg_room->hRoom);
  }
  return true;
}

RuntimeMode GetRuntimeMode() {
  return s_runtime_mode;
}

void SetRuntimeMode(RuntimeMode mode) {
  if (mode == s_runtime_mode) {
    return;
  }
  s_runtime_mode = mode;
  PIPE_LOG_INFO("[RuntimeMode] Switched to {}", RuntimeModeToString(s_runtime_mode));
}

bool IsActiveMutationEnabled() {
  return s_runtime_mode == RuntimeMode::ActiveMutation;
}

const char* GetRuntimeModeName(RuntimeMode mode) {
  return RuntimeModeToString(mode);
}

}  // namespace d2r
