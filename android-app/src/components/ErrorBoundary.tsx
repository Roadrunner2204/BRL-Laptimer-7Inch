/**
 * Simple React error boundary. Wraps children and renders a friendly error
 * screen instead of crashing the whole app when a render throws.
 */
import React from 'react';
import { View, Text, StyleSheet, ScrollView } from 'react-native';
import { C } from '../theme';

interface State { err: Error | null; }

export default class ErrorBoundary extends React.Component<{ children: React.ReactNode }, State> {
  state: State = { err: null };

  static getDerivedStateFromError(err: Error) {
    return { err };
  }

  componentDidCatch(err: Error, info: any) {
    console.warn('[ErrorBoundary]', err?.message, info?.componentStack);
  }

  render() {
    if (this.state.err) {
      return (
        <ScrollView style={s.root} contentContainerStyle={s.content}>
          <Text style={s.title}>⚠️  Render-Fehler</Text>
          <Text style={s.msg}>{this.state.err.message}</Text>
          {this.state.err.stack && (
            <Text style={s.stack} selectable>{this.state.err.stack}</Text>
          )}
          <Text style={s.hint}>Zurück gehen und erneut versuchen.</Text>
        </ScrollView>
      );
    }
    return this.props.children;
  }
}

const s = StyleSheet.create({
  root:    { flex: 1, backgroundColor: C.bg },
  content: { padding: 20 },
  title:   { color: C.danger, fontSize: 18, fontWeight: '700', marginBottom: 12 },
  msg:     { color: C.text, fontSize: 14, marginBottom: 12 },
  stack:   { color: C.dim, fontSize: 10, fontFamily: 'monospace' },
  hint:    { color: C.dim, fontSize: 12, marginTop: 20, fontStyle: 'italic' },
});
