/**
 * overlayConfig — persistable user preferences for VideoOverlay.
 *
 * Saved as JSON in AsyncStorage. VideoOverlay reads this to decide which
 * HUD elements to draw and where.
 */

import AsyncStorage from '@react-native-async-storage/async-storage';

export type Corner = 'tl' | 'tr' | 'bl' | 'br';

export interface OverlayConfig {
  showSpeed:   boolean;
  showLapTime: boolean;
  showDelta:   boolean;
  showGMeter:  boolean;
  showTrackName: boolean;

  positionSpeed:   Corner;   // big number placement
  positionLapTime: Corner;
  positionGMeter:  Corner;

  fontScale: number;         // 0.7 – 1.4, default 1.0

  /** Accent color used for overlay highlights (hex). */
  accentColor: string;

  /** 0..255 — alpha of the dark backdrop behind text. */
  backdropAlpha: number;
}

export const DEFAULT_OVERLAY_CONFIG: OverlayConfig = {
  showSpeed:     true,
  showLapTime:   true,
  showDelta:     true,
  showGMeter:    true,
  showTrackName: false,

  positionSpeed:   'tl',
  positionLapTime: 'tr',
  positionGMeter:  'bl',

  fontScale: 1.0,
  accentColor: '#0096FF',
  backdropAlpha: 115,   // ~0.45
};

const KEY = 'overlay_config_v1';

export async function loadOverlayConfig(): Promise<OverlayConfig> {
  try {
    const raw = await AsyncStorage.getItem(KEY);
    if (!raw) return DEFAULT_OVERLAY_CONFIG;
    const parsed = JSON.parse(raw);
    // Merge with defaults so new fields survive old stored configs
    return { ...DEFAULT_OVERLAY_CONFIG, ...parsed };
  } catch {
    return DEFAULT_OVERLAY_CONFIG;
  }
}

export async function saveOverlayConfig(cfg: OverlayConfig): Promise<void> {
  await AsyncStorage.setItem(KEY, JSON.stringify(cfg));
}
