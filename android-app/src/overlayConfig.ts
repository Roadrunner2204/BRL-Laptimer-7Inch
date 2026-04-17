/**
 * overlayConfig — persistable user preferences for VideoOverlay.
 *
 * v2 schema (2026-04-17): widgets are freely positionable. Each widget
 * has an anchor point (x, y) as fractions of video width/height and an
 * anchor-alignment (start/center/end). Old v1 configs (corner-based) are
 * migrated on load. Legacy fields are retained as optional on the type
 * so code referencing them doesn't break during the transition, but the
 * renderer only reads `widgets`.
 *
 * Saved as JSON in AsyncStorage.
 */

import AsyncStorage from '@react-native-async-storage/async-storage';

// ── Legacy v1 types (kept so older code paths compile) ────────────────
export type Corner = 'tl' | 'tr' | 'bl' | 'br';

// ── v2: widget-based layout ───────────────────────────────────────────
export type WidgetType   = 'speed' | 'lapTime' | 'delta' | 'gMeter' | 'trackName';
export type WidgetAnchor = 'start' | 'center' | 'end';

export interface Widget {
  id: string;
  type: WidgetType;
  /** Anchor point X, fraction of video width (0..1). */
  x: number;
  /** Anchor point Y (top of the widget box), fraction of video height. */
  y: number;
  /** Horizontal alignment of the widget around (x,y). */
  anchor: WidgetAnchor;
  visible: boolean;
}

export interface OverlayConfig {
  widgets: Widget[];
  fontScale: number;
  accentColor: string;
  backdropAlpha: number;

  // ── Legacy (v1) fields — optional, not read by the renderer ────────
  // Kept in the type so any lingering code / saved JSON round-trips
  // cleanly. New writes omit them; migration on load converts them
  // into `widgets` the first time.
  showSpeed?:     boolean;
  showLapTime?:   boolean;
  showDelta?:     boolean;
  showGMeter?:    boolean;
  showTrackName?: boolean;
  positionSpeed?:   Corner;
  positionLapTime?: Corner;
  positionGMeter?:  Corner;
}

// Sensible corner-based default placement. x/y are anchor points; for
// 'end' anchors x is the right edge, for 'center' it's the centerline.
export const DEFAULT_WIDGETS: Widget[] = [
  { id: 'speed',     type: 'speed',     x: 0.04, y: 0.05, anchor: 'start',  visible: true  },
  { id: 'lapTime',   type: 'lapTime',   x: 0.96, y: 0.05, anchor: 'end',    visible: true  },
  { id: 'delta',     type: 'delta',     x: 0.96, y: 0.14, anchor: 'end',    visible: true  },
  { id: 'gMeter',    type: 'gMeter',    x: 0.04, y: 0.55, anchor: 'start',  visible: true  },
  { id: 'trackName', type: 'trackName', x: 0.50, y: 0.04, anchor: 'center', visible: false },
];

export const DEFAULT_OVERLAY_CONFIG: OverlayConfig = {
  widgets: DEFAULT_WIDGETS,
  fontScale: 1.0,
  accentColor: '#0096FF',
  backdropAlpha: 115,   // ~0.45 * 255
};

// Map legacy Corner → (x, y, anchor) for migration.
function cornerToPos(c: Corner | undefined): { x: number; y: number; anchor: WidgetAnchor } {
  switch (c) {
    case 'tr': return { x: 0.96, y: 0.05, anchor: 'end'   };
    case 'bl': return { x: 0.04, y: 0.80, anchor: 'start' };
    case 'br': return { x: 0.96, y: 0.80, anchor: 'end'   };
    case 'tl':
    default:   return { x: 0.04, y: 0.05, anchor: 'start' };
  }
}

// Build a v2 widget list from v1 (corner + booleans) fields.
function migrateLegacy(raw: any): Widget[] {
  const s = cornerToPos(raw?.positionSpeed);
  const l = cornerToPos(raw?.positionLapTime);
  const g = cornerToPos(raw?.positionGMeter);
  // Delta sits just below lap time using the same anchor edge.
  const dy = Math.min(0.95, l.y + (l.y < 0.5 ? 0.08 : -0.08));
  return [
    { id: 'speed',    type: 'speed',    x: s.x, y: s.y, anchor: s.anchor,
                      visible: raw?.showSpeed    !== false },
    { id: 'lapTime',  type: 'lapTime',  x: l.x, y: l.y, anchor: l.anchor,
                      visible: raw?.showLapTime  !== false },
    { id: 'delta',    type: 'delta',    x: l.x, y: dy,  anchor: l.anchor,
                      visible: raw?.showDelta    !== false },
    { id: 'gMeter',   type: 'gMeter',   x: g.x, y: g.y, anchor: g.anchor,
                      visible: raw?.showGMeter   !== false },
    { id: 'trackName',type: 'trackName',x: 0.5, y: 0.04, anchor: 'center',
                      visible: !!raw?.showTrackName },
  ];
}

const KEY = 'overlay_config_v1';   // key kept stable; content format evolves

export async function loadOverlayConfig(): Promise<OverlayConfig> {
  try {
    const raw = await AsyncStorage.getItem(KEY);
    if (!raw) return DEFAULT_OVERLAY_CONFIG;
    const parsed = JSON.parse(raw);
    const hasWidgets = Array.isArray(parsed.widgets) && parsed.widgets.length > 0;
    const widgets: Widget[] = hasWidgets ? parsed.widgets : migrateLegacy(parsed);
    return {
      ...DEFAULT_OVERLAY_CONFIG,
      ...parsed,
      widgets,
    };
  } catch {
    return DEFAULT_OVERLAY_CONFIG;
  }
}

export async function saveOverlayConfig(cfg: OverlayConfig): Promise<void> {
  // Strip legacy fields on write — they're redundant with widgets.
  const {
    showSpeed, showLapTime, showDelta, showGMeter, showTrackName,
    positionSpeed, positionLapTime, positionGMeter,
    ...clean
  } = cfg;
  await AsyncStorage.setItem(KEY, JSON.stringify(clean));
}
