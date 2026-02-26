declare module 'd2r' {
  // Re-export types
  export * from 'd2r/types';

  // Re-export models
  export * from 'd2r/models';

  // Re-export classes
  export { Seed } from 'd2r/seed';
  export { DrlgAct } from 'd2r/drlg-act';
  export { Unit } from 'd2r/unit';
  export { Player, LocalPlayer } from 'd2r/player';
  export { Monster } from 'd2r/monster';
  export { Item } from 'd2r/item';
  export { GameObject } from 'd2r/game-object';
  export { Missile } from 'd2r/missile';
  export { RoomTile } from 'd2r/room-tile';
  export { ObjectManager } from 'd2r/object-manager';
  export { DebugPanel } from 'd2r/debug-panel';

  export const RuntimeModes: {
    readonly ReadOnlySafe: 'read_only_safe';
    readonly ActiveMutation: 'active_mutation';
  };

  // Binding function
  export function revealLevel(levelId: number): boolean;
  export function getRuntimeMode(): 'read_only_safe' | 'active_mutation';
  export function setRuntimeMode(mode: 'read_only_safe' | 'active_mutation' | 'safe' | 'active' | 0 | 1): boolean;
  export function isActiveMutationEnabled(): boolean;
}

// Support nyx: prefix
declare module 'nyx:d2r' {
  export * from 'd2r';
}
