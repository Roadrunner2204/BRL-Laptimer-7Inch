/**
 * LineChart — minimal SVG multi-trace line chart.
 *
 * Props:
 *   width / height    — pixel size
 *   xDomain           — [xMin, xMax] shared X (distance or time)
 *   yDomain           — [yMin, yMax] shared Y (or auto from traces)
 *   traces            — [{ x:number[], y:number[], color, width? }]
 *   gridY             — draw horizontal grid lines with labels at these Y values
 *   label             — chart title / channel name (top-left)
 *   unit              — displayed next to current-value readout
 *   cursorX           — draw vertical cursor at this X (e.g. scrubber position)
 *   onScrub           — called with X value while user drags horizontally
 */

import React, { useMemo } from 'react';
import { View, Text, StyleSheet, GestureResponderEvent } from 'react-native';
import Svg, { Path, Line, Text as SvgText, G, Rect } from 'react-native-svg';
import { C } from '../theme';

export interface Trace {
  x: number[];
  y: number[];
  color: string;
  width?: number;
  label?: string;
}

export interface LineChartProps {
  width:  number;
  height: number;
  xDomain: [number, number];
  yDomain?: [number, number];
  traces: Trace[];
  gridY?: number[];
  label?:  string;
  unit?:   string;
  cursorX?: number;
  onScrub?: (x: number) => void;
  /** Format function for the Y value readout at cursor, default: 1 decimal */
  fmtY?: (y: number) => string;
  /** Baseline Y (e.g. 0 for deltas) — drawn as bold horizontal line */
  baselineY?: number;
  /** Height reserved for the top-label row (default 20) */
  labelBarHeight?: number;
}

const PAD_L = 44;
const PAD_R = 8;
const PAD_T = 4;
const PAD_B = 18;

export default function LineChart(props: LineChartProps) {
  const {
    width, height, xDomain, traces,
    gridY, label, unit, cursorX, onScrub, fmtY, baselineY,
    labelBarHeight = 20,
  } = props;

  const chartW = width  - PAD_L - PAD_R;
  const chartH = height - PAD_T - PAD_B - labelBarHeight;
  const topY   = PAD_T + labelBarHeight;

  // Auto Y domain if not provided
  const yDomain: [number, number] = useMemo(() => {
    if (props.yDomain) return props.yDomain;
    let lo = Infinity, hi = -Infinity;
    for (const t of traces) {
      for (const v of t.y) {
        if (!isFinite(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
    if (!isFinite(lo) || !isFinite(hi)) return [0, 1];
    if (lo === hi) { lo -= 0.5; hi += 0.5; }
    const pad = (hi - lo) * 0.05;
    return [lo - pad, hi + pad];
  }, [traces, props.yDomain]);

  const [xMinRaw, xMaxRaw] = xDomain;
  const [yMinRaw, yMaxRaw] = yDomain;
  // Sanitize — any NaN/±Inf turns the SVG into broken glass.
  const xMin = isFinite(xMinRaw) ? xMinRaw : 0;
  const xMax = isFinite(xMaxRaw) && xMaxRaw > xMin ? xMaxRaw : xMin + 1;
  const yMin = isFinite(yMinRaw) ? yMinRaw : 0;
  const yMax = isFinite(yMaxRaw) && yMaxRaw > yMin ? yMaxRaw : yMin + 1;
  const xToPx = (x: number) => PAD_L + ((x - xMin) / (xMax - xMin || 1)) * chartW;
  const yToPx = (y: number) => topY + (1 - (y - yMin) / (yMax - yMin || 1)) * chartH;

  // Build SVG path strings — skip NaN / Infinity / missing points so a bad
  // data hole doesn't blow up the entire chart with an invalid SVG "d" attr.
  const paths = useMemo(() => traces.map(t => {
    const n = Math.min(t.x.length, t.y.length);
    if (n === 0) return '';
    let d = '';
    let started = false;
    for (let i = 0; i < n; i++) {
      const x = t.x[i], y = t.y[i];
      if (!isFinite(x) || !isFinite(y)) { started = false; continue; }
      const px = xToPx(x), py = yToPx(y);
      if (!isFinite(px) || !isFinite(py)) { started = false; continue; }
      d += started ? ` L${px} ${py}` : `M${px} ${py}`;
      started = true;
    }
    return d;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [traces, xMin, xMax, yMin, yMax, chartW, chartH]);

  // Value at cursor for each trace (linear interp)
  const valuesAtCursor = useMemo(() => {
    if (cursorX == null) return null;
    return traces.map(t => {
      if (t.x.length < 2) return null;
      let j = 0;
      while (j < t.x.length - 1 && t.x[j + 1] < cursorX) j++;
      const x0 = t.x[j], x1 = t.x[Math.min(j + 1, t.x.length - 1)];
      const y0 = t.y[j], y1 = t.y[Math.min(j + 1, t.y.length - 1)];
      if (x1 <= x0) return y1;
      const f = Math.max(0, Math.min(1, (cursorX - x0) / (x1 - x0)));
      return y0 + f * (y1 - y0);
    });
  }, [traces, cursorX]);

  function handleTouch(e: GestureResponderEvent) {
    if (!onScrub) return;
    const x = e.nativeEvent.locationX;
    const xVal = xMin + ((x - PAD_L) / chartW) * (xMax - xMin);
    onScrub(Math.max(xMin, Math.min(xMax, xVal)));
  }

  const fmt = fmtY ?? ((v: number) => v.toFixed(1));

  return (
    <View
      style={[styles.root, { width, height }]}
      onStartShouldSetResponder={() => true}
      onMoveShouldSetResponder={() => true}
      onResponderGrant={handleTouch}
      onResponderMove={handleTouch}
    >
      <Svg width={width} height={height}>
        {/* Background */}
        <Rect x={0} y={0} width={width} height={height} fill={C.surface} rx={8} />

        {/* Y grid + labels */}
        {gridY?.map((gy, i) => (
          <G key={`gy-${i}`}>
            <Line
              x1={PAD_L} y1={yToPx(gy)} x2={PAD_L + chartW} y2={yToPx(gy)}
              stroke={C.border} strokeWidth={0.5} strokeDasharray="3 3"
            />
            <SvgText
              x={PAD_L - 4} y={yToPx(gy) + 3}
              fill={C.dim} fontSize={9} textAnchor="end"
            >
              {fmt(gy)}
            </SvgText>
          </G>
        ))}

        {/* Baseline (bold, e.g. delta=0) */}
        {baselineY != null && (
          <Line
            x1={PAD_L} y1={yToPx(baselineY)} x2={PAD_L + chartW} y2={yToPx(baselineY)}
            stroke={C.textDark} strokeWidth={1}
          />
        )}

        {/* Traces */}
        {paths.map((d, i) => (
          <Path key={i} d={d} stroke={traces[i].color} strokeWidth={traces[i].width ?? 1.5} fill="none" />
        ))}

        {/* Cursor */}
        {cursorX != null && (
          <Line
            x1={xToPx(cursorX)} y1={topY} x2={xToPx(cursorX)} y2={topY + chartH}
            stroke={C.accent} strokeWidth={1} strokeDasharray="2 2"
          />
        )}

        {/* Label bar: channel name + current value */}
        <SvgText x={PAD_L} y={14} fill={C.text} fontSize={11} fontWeight="700">
          {label ?? ''}
        </SvgText>
        {valuesAtCursor && traces.map((t, i) => {
          const v = valuesAtCursor[i];
          if (v == null) return null;
          const txt = `${fmt(v)}${unit ? ' ' + unit : ''}`;
          return (
            <SvgText
              key={`v-${i}`}
              x={width - PAD_R - (traces.length - 1 - i) * 60}
              y={14}
              fill={t.color}
              fontSize={11}
              fontWeight="700"
              textAnchor="end"
            >
              {txt}
            </SvgText>
          );
        })}
      </Svg>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { marginBottom: 6, borderRadius: 8, overflow: 'hidden' },
});
