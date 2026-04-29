"""
libVLC-backed video player widget.

Same backend as the Android app's VideoScreen (react-native-vlc-media-player)
so we share the codec story: AVI containers with MJPEG video + 16-bit PCM
audio. Qt's built-in QtMultimedia uses ExoPlayer-like decoders that
historically can't render our MJPEG-in-AVI; libVLC just works.

The player draws into a native window handle (HWND on Windows). The HUD
overlay is a *sibling* widget raised above this one and uses
`WA_TransparentForMouseEvents` so clicks pass through to the player.

API:
    play_url(url)         start streaming
    play_path(path)       start playing a local file
    stop()                stop and release media
    set_position_ms(ms)   seek
    position_ms()         current playback ms (-1 if unknown)
    is_playing()          bool

Signal:
    position_changed(ms)  emitted ~10 Hz while playing — the HUD listens
    duration_changed(ms)
    state_changed(playing: bool)
"""

from __future__ import annotations

import sys
from pathlib import Path

from PyQt6.QtCore import QTimer, pyqtSignal
from PyQt6.QtWidgets import QFrame, QStackedLayout, QWidget

try:
    import vlc  # type: ignore[import-untyped]
    _VLC_OK = True
    _VLC_ERR = ""
except Exception as _e:  # noqa: BLE001
    vlc = None
    _VLC_OK = False
    _VLC_ERR = str(_e)


class VideoPlayer(QFrame):
    position_changed = pyqtSignal(int)     # ms
    duration_changed = pyqtSignal(int)     # ms
    state_changed = pyqtSignal(bool)       # is_playing

    def __init__(self) -> None:
        super().__init__()
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("background-color: black;")
        self.setMinimumSize(320, 180)

        self._instance = None
        self._player = None
        self._media = None
        self._last_state = False

        if _VLC_OK:
            # `--no-xlib` on Linux avoids xcb conflicts; harmless on Win/macOS.
            self._instance = vlc.Instance(["--no-xlib"])
            self._player = self._instance.media_player_new()

        # Poll player state. VLC's event manager works but the callbacks
        # arrive on a non-Qt thread; a 100 ms timer in the GUI thread keeps
        # the wiring trivial.
        self._poll = QTimer(self)
        self._poll.setInterval(100)
        self._poll.timeout.connect(self._on_poll)

    # ── Lifecycle ──────────────────────────────────────────────────────

    def showEvent(self, evt) -> None:  # noqa: ANN001
        super().showEvent(evt)
        # Bind only after the native handle exists (winId is 0 before show).
        if self._player is not None and self.winId():
            handle = int(self.winId())
            if sys.platform.startswith("win"):
                self._player.set_hwnd(handle)
            elif sys.platform == "darwin":
                self._player.set_nsobject(handle)
            else:
                self._player.set_xwindow(handle)

    def is_available(self) -> bool:
        return _VLC_OK

    def availability_error(self) -> str:
        return _VLC_ERR

    # ── Playback control ───────────────────────────────────────────────

    def play_url(self, url: str) -> None:
        if not _VLC_OK or self._player is None:
            return
        self._media = self._instance.media_new(url)
        self._player.set_media(self._media)
        self._player.play()
        self._poll.start()

    def play_path(self, path: str | Path) -> None:
        if not _VLC_OK or self._player is None:
            return
        p = Path(path)
        self._media = self._instance.media_new_path(str(p))
        self._player.set_media(self._media)
        self._player.play()
        self._poll.start()

    def stop(self) -> None:
        if self._player is None:
            return
        self._player.stop()
        self._poll.stop()
        if self._last_state:
            self._last_state = False
            self.state_changed.emit(False)

    def pause(self) -> None:
        if self._player is None:
            return
        self._player.pause()

    def set_paused(self, paused: bool) -> None:
        if self._player is None:
            return
        self._player.set_pause(1 if paused else 0)

    def set_position_ms(self, ms: int) -> None:
        if self._player is None:
            return
        self._player.set_time(int(ms))

    def position_ms(self) -> int:
        if self._player is None:
            return -1
        t = self._player.get_time()
        return int(t) if t is not None and t >= 0 else -1

    def duration_ms(self) -> int:
        if self._player is None or self._media is None:
            return 0
        d = self._media.get_duration()
        return int(d) if d is not None and d > 0 else 0

    def is_playing(self) -> bool:
        return bool(self._player and self._player.is_playing())

    # ── Polling loop ───────────────────────────────────────────────────

    def _on_poll(self) -> None:
        if self._player is None:
            return
        playing = self.is_playing()
        if playing != self._last_state:
            self._last_state = playing
            self.state_changed.emit(playing)
        pos = self.position_ms()
        if pos >= 0:
            self.position_changed.emit(pos)
        dur = self.duration_ms()
        if dur > 0:
            self.duration_changed.emit(dur)


class VideoStage(QWidget):
    """Container that overlays a HUD widget on top of a VideoPlayer.

    On Windows VLC paints into the player's HWND; the HUD lives as a
    sibling widget with `WA_TransparentForMouseEvents` and is `raise_()`d
    on every resize so it composites above the native frame paint.
    Same trick the original VideoView used; pulled out so AnalyseView
    can reuse it side-by-side with charts.
    """

    def __init__(self, player: VideoPlayer, hud) -> None:  # noqa: ANN001
        super().__init__()
        self.player = player
        self.hud = hud
        layout = QStackedLayout(self)
        layout.setStackingMode(QStackedLayout.StackingMode.StackAll)
        layout.addWidget(player)
        layout.addWidget(hud)
        hud.raise_()

    def resizeEvent(self, evt) -> None:  # noqa: ANN001
        super().resizeEvent(evt)
        self.hud.raise_()
