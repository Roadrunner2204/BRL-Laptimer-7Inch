/**
 * overlayTemplates — named snapshots of an OverlayConfig.
 *
 * Built-in presets are hard-coded and can't be deleted or renamed from
 * the UI (they provide known-good starting points like "Minimal",
 * "Race", "Voll"). User-saved templates live in AsyncStorage under a
 * separate key from the active config.
 */

import AsyncStorage from '@react-native-async-storage/async-storage';
import { OverlayConfig, DEFAULT_OVERLAY_CONFIG, Widget } from './overlayConfig';

export interface Template {
  id: string;
  name: string;
  config: OverlayConfig;
  /** True for read-only built-ins; false/absent for user-saved. */
  builtin?: boolean;
}

// Helper to build a config with a given widget set but otherwise
// standard global settings.
function cfgWithWidgets(widgets: Widget[]): OverlayConfig {
  return {
    ...DEFAULT_OVERLAY_CONFIG,
    widgets,
  };
}

export const BUILTIN_TEMPLATES: Template[] = [
  {
    id: 'builtin-default',
    name: 'Standard (Race)',
    builtin: true,
    config: DEFAULT_OVERLAY_CONFIG,
  },
  {
    id: 'builtin-minimal',
    name: 'Minimal',
    builtin: true,
    config: cfgWithWidgets([
      { id: 'speed',   type: 'speed',   x: 0.04, y: 0.05, anchor: 'start', visible: true },
      { id: 'lapTime', type: 'lapTime', x: 0.96, y: 0.05, anchor: 'end',   visible: true },
    ]),
  },
  {
    id: 'builtin-full',
    name: 'Voll (AIM-Style)',
    builtin: true,
    config: cfgWithWidgets([
      { id: 'speed',     type: 'speed',     x: 0.04, y: 0.05, anchor: 'start',  visible: true },
      { id: 'speedBar',  type: 'speedBar',  x: 0.04, y: 0.22, anchor: 'start',  visible: true },
      { id: 'lapNumber', type: 'lapNumber', x: 0.50, y: 0.04, anchor: 'center', visible: true },
      { id: 'lapTime',   type: 'lapTime',   x: 0.50, y: 0.18, anchor: 'center', visible: true },
      { id: 'delta',     type: 'delta',     x: 0.50, y: 0.28, anchor: 'center', visible: true },
      { id: 'trackName', type: 'trackName', x: 0.96, y: 0.05, anchor: 'end',    visible: true },
      { id: 'gMeter',    type: 'gMeter',    x: 0.96, y: 0.55, anchor: 'end',    visible: true },
      { id: 'miniMap',   type: 'miniMap',   x: 0.04, y: 0.55, anchor: 'start',  visible: true },
      { id: 'gForce',    type: 'gForce',    x: 0.50, y: 0.70, anchor: 'center', visible: true },
    ]),
  },
  {
    id: 'builtin-rallye',
    name: 'Rallye / Stage',
    builtin: true,
    config: cfgWithWidgets([
      { id: 'speed',     type: 'speed',     x: 0.04, y: 0.05, anchor: 'start',  visible: true },
      { id: 'lapTime',   type: 'lapTime',   x: 0.50, y: 0.04, anchor: 'center', visible: true, scale: 1.3 },
      { id: 'trackName', type: 'trackName', x: 0.50, y: 0.90, anchor: 'center', visible: true },
      { id: 'gForce',    type: 'gForce',    x: 0.96, y: 0.05, anchor: 'end',    visible: true },
      { id: 'speedBar',  type: 'speedBar',  x: 0.50, y: 0.80, anchor: 'center', visible: true },
    ]),
  },
];

const KEY = 'overlay_templates_v1';

async function loadUserTemplates(): Promise<Template[]> {
  try {
    const raw = await AsyncStorage.getItem(KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    // Strip any accidental `builtin: true` from user-persisted data so a
    // hand-edited file can't shadow a built-in's id or be undeletable.
    return parsed
      .filter((t: any) => typeof t?.id === 'string' && typeof t?.name === 'string' && t?.config)
      .map((t: Template) => ({ ...t, builtin: false }));
  } catch {
    return [];
  }
}

/** All templates — built-ins first, then user-saved. */
export async function listTemplates(): Promise<Template[]> {
  const user = await loadUserTemplates();
  return [...BUILTIN_TEMPLATES, ...user];
}

/** Save the current config as a new named user template. */
export async function saveTemplate(name: string, config: OverlayConfig): Promise<Template> {
  const user = await loadUserTemplates();
  const trimmed = name.trim().slice(0, 40) || 'Unbenannt';
  const tpl: Template = {
    id: `user-${Date.now()}`,
    name: trimmed,
    // Deep-clone so later edits to the active config don't mutate the template.
    config: JSON.parse(JSON.stringify(config)) as OverlayConfig,
    builtin: false,
  };
  user.push(tpl);
  await AsyncStorage.setItem(KEY, JSON.stringify(user));
  return tpl;
}

/** Remove a user template. Built-ins can't be deleted. */
export async function deleteTemplate(id: string): Promise<void> {
  if (id.startsWith('builtin-')) return;
  const user = await loadUserTemplates();
  const next = user.filter(t => t.id !== id);
  await AsyncStorage.setItem(KEY, JSON.stringify(next));
}
