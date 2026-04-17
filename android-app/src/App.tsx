import React from 'react';
import { LogBox } from 'react-native';
import { NavigationContainer, DefaultTheme } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { StatusBar } from 'expo-status-bar';
import HomeScreen      from './screens/HomeScreen';
import ConnectScreen   from './screens/ConnectScreen';
import SessionsScreen  from './screens/SessionsScreen';
import DetailScreen    from './screens/DetailScreen';
import MapScreen       from './screens/MapScreen';
import ChartsScreen    from './screens/ChartsScreen';
import VideoScreen     from './screens/VideoScreen';
import VideosScreen    from './screens/VideosScreen';
import CompareScreen   from './screens/CompareScreen';
import OverlayConfigScreen from './screens/OverlayConfigScreen';
import TrackCreatorScreen from './screens/TrackCreatorScreen';
import TracksScreen from './screens/TracksScreen';
import { C } from './theme';

// Suppress RN's in-app yellow LogBox overlay. Logs still go to logcat for
// debugging via `adb logcat *:W`. Keeps the app UI clean for end-users.
LogBox.ignoreAllLogs(true);

export type RootStackParamList = {
  Home:     undefined;
  Connect:  undefined;
  Sessions: { mode: 'device' | 'local' };
  Detail:   { sessionId: string };
  Map:      { sessionId: string };
  Charts:   { sessionId: string };
  Video:    { videoId: string; sessionId?: string; mode: 'stream' | 'download' };
  Videos:   undefined;
  Compare:  { sessionId: string };
  OverlayConfig: undefined;
  TrackCreator: { initial?: import('./types').Track } | undefined;
  Tracks: undefined;
};

const Stack = createNativeStackNavigator<RootStackParamList>();

const DarkTheme = {
  ...DefaultTheme,
  colors: { ...DefaultTheme.colors, background: C.bg, card: C.surface, text: C.text, border: '#222', primary: C.accent },
};

export default function App() {
  return (
    <NavigationContainer theme={DarkTheme}>
      <StatusBar style="light" />
      <Stack.Navigator
        initialRouteName="Home"
        screenOptions={{
          headerStyle: { backgroundColor: C.surface },
          headerTintColor: C.text,
          headerTitleStyle: { fontWeight: '700' },
          contentStyle: { backgroundColor: C.bg },
        }}
      >
        <Stack.Screen name="Home"     component={HomeScreen}     options={{ headerShown: false }} />
        <Stack.Screen name="Connect"  component={ConnectScreen}  options={{ title: 'Verbinden' }} />
        <Stack.Screen name="Sessions" component={SessionsScreen} options={{ title: 'Sessions' }} />
        <Stack.Screen name="Detail"   component={DetailScreen}   options={{ title: 'Übersicht' }} />
        <Stack.Screen name="Map"      component={MapScreen}      options={{ title: 'Karte', headerTransparent: true, headerTintColor: C.text }} />
        <Stack.Screen name="Charts"   component={ChartsScreen}   options={{ title: 'Analyse' }} />
        <Stack.Screen name="Video"    component={VideoScreen}    options={{ title: 'Video' }} />
        <Stack.Screen name="Videos"   component={VideosScreen}   options={{ title: 'Videos' }} />
        <Stack.Screen name="Compare"  component={CompareScreen}  options={{ title: 'Runden vergleichen' }} />
        <Stack.Screen name="OverlayConfig" component={OverlayConfigScreen} options={{ title: 'Overlay anpassen' }} />
        <Stack.Screen name="TrackCreator"  component={TrackCreatorScreen}  options={{ title: 'Strecke erstellen' }} />
        <Stack.Screen name="Tracks"        component={TracksScreen}        options={{ title: 'Strecken' }} />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
