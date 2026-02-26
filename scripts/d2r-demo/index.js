'use strict';

import { ObjectManager, UnitTypes, DebugPanel, revealLevel } from 'nyx:d2r';
import { RuntimeModes, setRuntimeMode, getRuntimeMode, isActiveMutationEnabled } from 'nyx:d2r';
import { withGameLock } from 'nyx:memory';
import { Markers } from './markers.js';
import { ExitMarkers } from './exit-markers.js';

const fsBinding = internalBinding('fs');
const processBinding = internalBinding('process');

// d2r-demo expects reveal behavior; keep mutation enabled by default here.
const ENABLE_ACTIVE_MUTATION = true;
const FORCE_DEBUG_LOG = false;

function pathJoin(base, leaf) {
  if (!base || base.length === 0) return leaf;
  const last = base[base.length - 1];
  if (last === '\\' || last === '/') return `${base}${leaf}`;
  return `${base}\\${leaf}`;
}

function detectDebugLogFlag() {
  const roots = [];
  try {
    const scriptsRoot = processBinding.scriptsRoot();
    if (scriptsRoot) roots.push(scriptsRoot);
  } catch {}
  try {
    const cwd = processBinding.cwd();
    if (cwd) roots.push(cwd);
  } catch {}

  const candidates = [];
  for (const root of roots) {
    candidates.push(pathJoin(root, 'd2r-demo.debug-log.flag'));
    candidates.push(pathJoin(root, 'd2r-demo\\debug-log.flag'));
  }

  for (const candidate of candidates) {
    try {
      if (fsBinding.existsSync(candidate)) {
        return true;
      }
    } catch {}
  }
  return false;
}

const DEBUG_LOG = FORCE_DEBUG_LOG || detectDebugLogFlag();

function debugLog(...args) {
  if (DEBUG_LOG) {
    console.log(...args);
  }
}

try {
  const objMgr = new ObjectManager();
  const debugPanel = new DebugPanel(objMgr);
  const markers  = new Markers(objMgr);
  const exitMarkers = new ExitMarkers(objMgr);
  const desiredRuntimeMode = ENABLE_ACTIVE_MUTATION ? RuntimeModes.ActiveMutation : RuntimeModes.ReadOnlySafe;
  if (!setRuntimeMode(desiredRuntimeMode)) {
    console.warn(`[RuntimeMode] Failed to set mode: ${desiredRuntimeMode}`);
  }
  if (DEBUG_LOG) {
    console.log('[DebugLog] enabled');
  }
  debugLog(`[RuntimeMode] ${getRuntimeMode()}`);
  if (!isActiveMutationEnabled()) {
    console.warn('Mutation features disabled in read_only_safe mode');
  }

  objMgr.tick();

  const players = objMgr.getUnits(UnitTypes.Player);
  const monsters = objMgr.getUnits(UnitTypes.Monster);
  const items = objMgr.getUnits(UnitTypes.Item);
  const objects = objMgr.getUnits(UnitTypes.Object);
  const missiles = objMgr.getUnits(UnitTypes.Missile);
  const tiles = objMgr.getUnits(UnitTypes.Tile);

  debugLog(`Objects`);
  debugLog(`  Players:  ${players.size}`);
  debugLog(`  Monsters: ${monsters.size}`);
  debugLog(`  Items:    ${items.size}`);
  debugLog(`  Objects:  ${objects.size}`);
  debugLog(`  Missiles: ${missiles.size}`);
  debugLog(`  Tiles:    ${tiles.size}`);

  if (DEBUG_LOG && objMgr.me) {
    debugLog(`\nLocal player: id=${objMgr.me.id} at (${objMgr.me.posX}, ${objMgr.me.posY})`);
    debugLog(JSON.stringify(objMgr.me, (_, v) => typeof v === 'bigint' ? v.toString(16) : v, 2));
  }

  // Show first few monsters
  let count = 0;
  for (const [id, monster] of monsters) {
    if (count >= 5) break;
    if (DEBUG_LOG) {
      debugLog(`  Monster id=${id} classId=${monster.classId} at (${monster.posX}, ${monster.posY}) alive=${monster.isAlive}`);
    }
    count++;
  }

  let revealed_levels = [];
  let revealDisabled = !isActiveMutationEnabled();
  let revealFailureStreak = 0;
  let wasInGame = !!objMgr.me;
  setInterval(() => {
    objMgr.tick();
    exitMarkers.tick();
    debugPanel.refresh();

    if (!revealDisabled && !isActiveMutationEnabled()) {
      revealDisabled = true;
      console.warn('Runtime mode changed to read_only_safe; disabling reveal');
    }

    if (!revealDisabled && typeof objMgr.isRiskCircuitTripped === 'function' && objMgr.isRiskCircuitTripped()) {
      revealDisabled = true;
      console.warn('Reveal circuit breaker enabled for this session; keeping read-only overlays active');
    }

    const me = objMgr.me;
    const inGame = !!me;
    if (inGame !== wasInGame) {
      if (inGame) {
        debugLog('[Transition] Entered game');
      } else {
        debugLog('[Transition] Left game');
      }
      wasInGame = inGame;
    }

    if (!me && revealed_levels.length > 0) {
      console.log("Resetting revealed levels");
      revealed_levels = [];
      revealFailureStreak = 0;
    }
    if (me && !revealDisabled) {
      const currentLevelId = me.path?.room?.drlgRoom?.level?.id;
      if (currentLevelId !== undefined && !revealed_levels.includes(currentLevelId)) {
        withGameLock(_ => {
          if (revealLevel(currentLevelId)) {
            console.log(`Revealed level ${currentLevelId}`);
            revealed_levels.push(currentLevelId);
            revealFailureStreak = 0;
          } else {
            revealFailureStreak++;
            if (revealFailureStreak >= 3) {
              revealDisabled = true;
              console.warn('Reveal disabled after repeated failures; continuing in read-only mode');
            }
          }
        });
      }
    }
  }, 20);
} catch (err) {
  console.error(err.message);
  console.error(err.stack);
}
