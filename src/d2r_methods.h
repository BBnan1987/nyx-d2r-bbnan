#pragma once

#include "d2r_structs.h"

namespace d2r {

enum class RuntimeMode : uint32_t {
  ReadOnlySafe = 0,
  ActiveMutation = 1,
};

D2UnitStrc* GetUnit(uint32_t id, uint32_t type);
uint32_t GetPlayerId(uint32_t index);
D2UnitStrc* GetPlayerUnit(uint32_t index);
bool AutomapReveal(D2ActiveRoomStrc* hRoom);
bool RevealLevelById(uint32_t id);
RuntimeMode GetRuntimeMode();
void SetRuntimeMode(RuntimeMode mode);
bool IsActiveMutationEnabled();
const char* GetRuntimeModeName(RuntimeMode mode);

}
