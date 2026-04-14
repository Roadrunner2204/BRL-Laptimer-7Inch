// Bavarian RaceLabs — matches the ESP32 display palette (main/ui/theme.h).
// Primary is Bavarian Electric Blue, not green.
export const C = {
  bg:        '#000000',  // pure black (same as display)
  surface:   '#0D1117',  // dark blue-tinted surface
  surface2:  '#161B22',  // elevated surface
  border:    '#1C3A5C',  // blue-tinted border
  text:      '#FFFFFF',
  dim:       '#7A8FA6',  // blue-grey dim text
  textDark:  '#3A4A5C',
  accent:    '#0096FF',  // BRL blue — primary / active / GPS ok
  accentDim: '#0060C0',  // pressed states
  faster:    '#00C8FF',  // cyan-blue — faster than best
  warn:      '#FF9500',  // orange — slower
  danger:    '#FF3B30',  // red — much slower / error
  highlight: '#0A1A2E',  // dark-blue row background (e.g. best lap)
};
