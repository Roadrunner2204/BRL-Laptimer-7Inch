import React from 'react';
import { NavigationContainer, DefaultTheme } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { StatusBar } from 'expo-status-bar';
import ConnectScreen   from './screens/ConnectScreen';
import SessionsScreen  from './screens/SessionsScreen';
import DetailScreen    from './screens/DetailScreen';
import MapScreen       from './screens/MapScreen';
import { C } from './theme';

export type RootStackParamList = {
  Connect:  undefined;
  Sessions: { mode: 'device' | 'local' };
  Detail:   { sessionId: string };
  Map:      { sessionId: string };
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
        screenOptions={{
          headerStyle: { backgroundColor: C.surface },
          headerTintColor: C.text,
          headerTitleStyle: { fontWeight: '700' },
          contentStyle: { backgroundColor: C.bg },
        }}
      >
        <Stack.Screen name="Connect"  component={ConnectScreen}  options={{ title: 'BRL Telemetry', headerShown: false }} />
        <Stack.Screen name="Sessions" component={SessionsScreen} options={{ title: 'Sessions' }} />
        <Stack.Screen name="Detail"   component={DetailScreen}   options={{ title: 'Session Detail' }} />
        <Stack.Screen name="Map"      component={MapScreen}      options={{ title: 'Karte', headerTransparent: true, headerTintColor: C.text }} />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
