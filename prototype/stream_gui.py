#!/usr/bin/env python3
"""
Real-time streaming GUI for OpenBCI Cyton + EmotiBit (Feather), via BrainFlow.

Streams and plots:
  - OpenBCI Cyton Channel 1 (EXG)  -> connections SRB / AGND / N1P
  - EmotiBit Temperature
  - EmotiBit IMU (accelerometer, gyroscope, magnetometer)
  - EmotiBit PPG Green

Both devices run through BrainFlow so they share one API:
  - Cyton   -> BoardIds.CYTON_BOARD    (serial dongle)
  - EmotiBit-> BoardIds.EMOTIBIT_BOARD (WiFi, auto-discovered or by IP)

Each device connects independently, so one being offline never blocks the other.
"""

import os
import sys
import time
from collections import deque

import numpy as np
import pyqtgraph as pg
from PySide6 import QtCore, QtWidgets

from brainflow.board_shim import (
    BoardShim,
    BoardIds,
    BrainFlowInputParams,
    BrainFlowPresets,
)
from brainflow.exit_codes import BrainFlowError

# ----------------------------------------------------------------------------
# Configuration derived from BrainFlow board descriptions (not hard-coded).
# ----------------------------------------------------------------------------
CYTON_ID = int(BoardIds.CYTON_BOARD)
EMOTIBIT_ID = int(BoardIds.EMOTIBIT_BOARD)

DEFAULT_PRESET = BrainFlowPresets.DEFAULT_PRESET      # IMU (accel/gyro/mag)
AUX_PRESET = BrainFlowPresets.AUXILIARY_PRESET        # PPG (IR/Red/Green)
ANC_PRESET = BrainFlowPresets.ANCILLARY_PRESET        # temperature/EDA

# Default serial port for the Cyton USB dongle on this machine.
DEFAULT_CYTON_PORT = "/dev/cu.usbserial-DP04W4GA"

# Display window length (seconds) per device.
CYTON_WINDOW_S = 5.0
EMOTIBIT_WINDOW_S = 10.0

UPDATE_MS = 40  # GUI refresh period (~25 fps)

# Directory for optional CSV recordings.
RECORD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")


# ----------------------------------------------------------------------------
# Device wrappers
# ----------------------------------------------------------------------------
class CytonSource:
    """OpenBCI Cyton over the USB dongle. Streams EXG channel 1 for display."""

    def __init__(self):
        self.board = None
        self.connected = False
        self.rate = BoardShim.get_sampling_rate(CYTON_ID)
        self.ts_ch = BoardShim.get_timestamp_channel(CYTON_ID)
        self.ch1_row = BoardShim.get_exg_channels(CYTON_ID)[0]  # Channel 1

    def connect(self, serial_port, record=False):
        params = BrainFlowInputParams()
        params.serial_port = serial_port
        board = BoardShim(CYTON_ID, params)
        board.prepare_session()
        if record:
            os.makedirs(RECORD_DIR, exist_ok=True)
            path = os.path.join(RECORD_DIR, f"cyton_{_stamp()}.csv")
            board.add_streamer(f"file://{path}:w")
        board.start_stream()
        self.board = board
        self.connected = True

    def read_window(self, seconds):
        n = int(seconds * self.rate)
        data = self.board.get_current_board_data(n)
        if data.shape[1] == 0:
            return None, None
        y = data[self.ch1_row]
        t = data[self.ts_ch]
        t = t - t[-1]  # newest sample at t=0, history negative
        return t, y

    def disconnect(self):
        self.connected = False
        if self.board is not None:
            try:
                self.board.stop_stream()
            except BrainFlowError:
                pass
            try:
                self.board.release_session()
            except BrainFlowError:
                pass
            self.board = None


class EmotiBitSource:
    """EmotiBit (Feather) over WiFi. Streams IMU, PPG green, and temperature."""

    def __init__(self):
        self.board = None
        self.connected = False
        # DEFAULT preset -> IMU
        self.imu_rate = BoardShim.get_sampling_rate(EMOTIBIT_ID, DEFAULT_PRESET)
        self.accel = BoardShim.get_accel_channels(EMOTIBIT_ID, DEFAULT_PRESET)
        self.gyro = BoardShim.get_gyro_channels(EMOTIBIT_ID, DEFAULT_PRESET)
        self.mag = BoardShim.get_magnetometer_channels(EMOTIBIT_ID, DEFAULT_PRESET)
        self.imu_ts = BoardShim.get_timestamp_channel(EMOTIBIT_ID, DEFAULT_PRESET)
        # AUXILIARY preset -> PPG (index 2 is green)
        self.ppg_rate = BoardShim.get_sampling_rate(EMOTIBIT_ID, AUX_PRESET)
        self.ppg_green = BoardShim.get_ppg_channels(EMOTIBIT_ID, AUX_PRESET)[2]
        self.ppg_ts = BoardShim.get_timestamp_channel(EMOTIBIT_ID, AUX_PRESET)
        # ANCILLARY preset -> temperature
        self.temp_rate = BoardShim.get_sampling_rate(EMOTIBIT_ID, ANC_PRESET)
        self.temp = BoardShim.get_temperature_channels(EMOTIBIT_ID, ANC_PRESET)[0]
        self.temp_ts = BoardShim.get_timestamp_channel(EMOTIBIT_ID, ANC_PRESET)

    def connect(self, ip_address="", record=False):
        params = BrainFlowInputParams()
        if ip_address.strip():
            params.ip_address = ip_address.strip()
        board = BoardShim(EMOTIBIT_ID, params)
        board.prepare_session()
        if record:
            os.makedirs(RECORD_DIR, exist_ok=True)
            stamp = _stamp()
            board.add_streamer(f"file://{os.path.join(RECORD_DIR, f'emotibit_imu_{stamp}.csv')}:w", DEFAULT_PRESET)
            board.add_streamer(f"file://{os.path.join(RECORD_DIR, f'emotibit_ppg_{stamp}.csv')}:w", AUX_PRESET)
            board.add_streamer(f"file://{os.path.join(RECORD_DIR, f'emotibit_anc_{stamp}.csv')}:w", ANC_PRESET)
        board.start_stream()
        self.board = board
        self.connected = True

    def _read(self, preset, rate, ts_ch, rows, seconds):
        n = int(seconds * rate)
        data = self.board.get_current_board_data(n, preset)
        if data.shape[1] == 0:
            return None, None
        t = data[ts_ch]
        t = t - t[-1]
        return t, {r: data[r] for r in rows}

    def read_imu(self, seconds):
        return self._read(DEFAULT_PRESET, self.imu_rate, self.imu_ts,
                           self.accel + self.gyro + self.mag, seconds)

    def read_ppg(self, seconds):
        t, d = self._read(AUX_PRESET, self.ppg_rate, self.ppg_ts, [self.ppg_green], seconds)
        if t is None:
            return None, None
        return t, d[self.ppg_green]

    def read_temp(self, seconds):
        t, d = self._read(ANC_PRESET, self.temp_rate, self.temp_ts, [self.temp], seconds)
        if t is None:
            return None, None
        return t, d[self.temp]

    def disconnect(self):
        self.connected = False
        if self.board is not None:
            try:
                self.board.stop_stream()
            except BrainFlowError:
                pass
            try:
                self.board.release_session()
            except BrainFlowError:
                pass
            self.board = None


def _stamp():
    return time.strftime("%Y%m%d_%H%M%S")


# ----------------------------------------------------------------------------
# GUI
# ----------------------------------------------------------------------------
class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("OpenBCI + EmotiBit — Live Stream")
        self.resize(1200, 900)

        self.cyton = CytonSource()
        self.emotibit = EmotiBitSource()

        pg.setConfigOptions(antialias=True)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)

        root.addLayout(self._build_controls())
        root.addWidget(self._build_plots(), stretch=1)

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(UPDATE_MS)
        self.timer.timeout.connect(self._update)
        self.timer.start()

    # ---- controls ---------------------------------------------------------
    def _build_controls(self):
        bar = QtWidgets.QHBoxLayout()

        # Cyton group
        cyton_box = QtWidgets.QGroupBox("OpenBCI Cyton (dongle)")
        cl = QtWidgets.QHBoxLayout(cyton_box)
        cl.addWidget(QtWidgets.QLabel("Port:"))
        self.port_edit = QtWidgets.QLineEdit(DEFAULT_CYTON_PORT)
        self.port_edit.setMinimumWidth(220)
        cl.addWidget(self.port_edit)
        self.cyton_btn = QtWidgets.QPushButton("Connect")
        self.cyton_btn.clicked.connect(self._toggle_cyton)
        cl.addWidget(self.cyton_btn)
        self.cyton_status = QtWidgets.QLabel("● offline")
        self.cyton_status.setStyleSheet("color: gray;")
        cl.addWidget(self.cyton_status)
        bar.addWidget(cyton_box)

        # EmotiBit group
        emo_box = QtWidgets.QGroupBox("EmotiBit (WiFi)")
        el = QtWidgets.QHBoxLayout(emo_box)
        el.addWidget(QtWidgets.QLabel("IP (blank=auto):"))
        self.ip_edit = QtWidgets.QLineEdit("")
        self.ip_edit.setPlaceholderText("192.168.x.x")
        self.ip_edit.setMinimumWidth(140)
        el.addWidget(self.ip_edit)
        self.emo_btn = QtWidgets.QPushButton("Connect")
        self.emo_btn.clicked.connect(self._toggle_emotibit)
        el.addWidget(self.emo_btn)
        self.emo_status = QtWidgets.QLabel("● offline")
        self.emo_status.setStyleSheet("color: gray;")
        el.addWidget(self.emo_status)
        bar.addWidget(emo_box)

        # Options
        opt_box = QtWidgets.QGroupBox("Options")
        ol = QtWidgets.QHBoxLayout(opt_box)
        self.record_chk = QtWidgets.QCheckBox("Record CSV")
        ol.addWidget(self.record_chk)
        self.detrend_chk = QtWidgets.QCheckBox("Detrend Ch1")
        self.detrend_chk.setChecked(True)
        ol.addWidget(self.detrend_chk)
        bar.addWidget(opt_box)

        bar.addStretch(1)
        return bar

    # ---- plots ------------------------------------------------------------
    def _build_plots(self):
        layout = pg.GraphicsLayoutWidget()

        # Cyton Ch1
        self.p_ch1 = layout.addPlot(row=0, col=0, colspan=2, title="OpenBCI Cyton — Channel 1 (µV)")
        self.p_ch1.showGrid(x=True, y=True, alpha=0.3)
        self.p_ch1.setLabel("bottom", "time", units="s")
        self.c_ch1 = self.p_ch1.plot(pen=pg.mkPen("#00d0ff", width=1))

        # Temperature
        self.p_temp = layout.addPlot(row=1, col=0, title="EmotiBit — Temperature (°C)")
        self.p_temp.showGrid(x=True, y=True, alpha=0.3)
        self.p_temp.setLabel("bottom", "time", units="s")
        self.c_temp = self.p_temp.plot(pen=pg.mkPen("#ff8c00", width=2))

        # PPG green
        self.p_ppg = layout.addPlot(row=1, col=1, title="EmotiBit — PPG Green")
        self.p_ppg.showGrid(x=True, y=True, alpha=0.3)
        self.p_ppg.setLabel("bottom", "time", units="s")
        self.c_ppg = self.p_ppg.plot(pen=pg.mkPen("#00e676", width=2))

        # Accel
        self.p_acc = layout.addPlot(row=2, col=0, title="EmotiBit — Accelerometer (g)")
        self.p_acc.showGrid(x=True, y=True, alpha=0.3)
        self.p_acc.addLegend(offset=(10, 5))
        self.p_acc.setLabel("bottom", "time", units="s")
        self.c_acc = [
            self.p_acc.plot(pen=pg.mkPen("#ff5252", width=1), name="X"),
            self.p_acc.plot(pen=pg.mkPen("#69f0ae", width=1), name="Y"),
            self.p_acc.plot(pen=pg.mkPen("#448aff", width=1), name="Z"),
        ]

        # Gyro
        self.p_gyr = layout.addPlot(row=2, col=1, title="EmotiBit — Gyroscope (°/s)")
        self.p_gyr.showGrid(x=True, y=True, alpha=0.3)
        self.p_gyr.addLegend(offset=(10, 5))
        self.p_gyr.setLabel("bottom", "time", units="s")
        self.c_gyr = [
            self.p_gyr.plot(pen=pg.mkPen("#ff5252", width=1), name="X"),
            self.p_gyr.plot(pen=pg.mkPen("#69f0ae", width=1), name="Y"),
            self.p_gyr.plot(pen=pg.mkPen("#448aff", width=1), name="Z"),
        ]

        # Mag
        self.p_mag = layout.addPlot(row=3, col=0, colspan=2, title="EmotiBit — Magnetometer (µT)")
        self.p_mag.showGrid(x=True, y=True, alpha=0.3)
        self.p_mag.addLegend(offset=(10, 5))
        self.p_mag.setLabel("bottom", "time", units="s")
        self.c_mag = [
            self.p_mag.plot(pen=pg.mkPen("#ff5252", width=1), name="X"),
            self.p_mag.plot(pen=pg.mkPen("#69f0ae", width=1), name="Y"),
            self.p_mag.plot(pen=pg.mkPen("#448aff", width=1), name="Z"),
        ]

        return layout

    # ---- connect / disconnect --------------------------------------------
    def _toggle_cyton(self):
        if self.cyton.connected:
            self.cyton.disconnect()
            self._clear([self.c_ch1])
            self._set_status(self.cyton_status, self.cyton_btn, False)
            return
        try:
            self.cyton.connect(self.port_edit.text().strip(), self.record_chk.isChecked())
            self._set_status(self.cyton_status, self.cyton_btn, True)
        except BrainFlowError as e:
            self._error("Cyton connect failed", str(e))

    def _toggle_emotibit(self):
        if self.emotibit.connected:
            self.emotibit.disconnect()
            self._clear([self.c_temp, self.c_ppg] + self.c_acc + self.c_gyr + self.c_mag)
            self._set_status(self.emo_status, self.emo_btn, False)
            return
        try:
            self.emotibit.connect(self.ip_edit.text().strip(), self.record_chk.isChecked())
            self._set_status(self.emo_status, self.emo_btn, True)
        except BrainFlowError as e:
            self._error("EmotiBit connect failed", str(e))

    def _set_status(self, label, button, on):
        if on:
            label.setText("● streaming")
            label.setStyleSheet("color: #00c853; font-weight: bold;")
            button.setText("Disconnect")
        else:
            label.setText("● offline")
            label.setStyleSheet("color: gray;")
            button.setText("Connect")

    def _error(self, title, msg):
        QtWidgets.QMessageBox.critical(self, title, msg)

    def _clear(self, curves):
        for c in curves:
            c.clear()

    # ---- update loop ------------------------------------------------------
    def _update(self):
        if self.cyton.connected:
            try:
                t, y = self.cyton.read_window(CYTON_WINDOW_S)
                if t is not None:
                    if self.detrend_chk.isChecked():
                        y = y - np.mean(y)
                    self.c_ch1.setData(t, y)
            except BrainFlowError:
                pass

        if self.emotibit.connected:
            try:
                t, y = self.emotibit.read_temp(EMOTIBIT_WINDOW_S)
                if t is not None:
                    self.c_temp.setData(t, y)

                t, y = self.emotibit.read_ppg(EMOTIBIT_WINDOW_S)
                if t is not None:
                    self.c_ppg.setData(t, y)

                t, d = self.emotibit.read_imu(EMOTIBIT_WINDOW_S)
                if t is not None:
                    for curve, row in zip(self.c_acc, self.emotibit.accel):
                        curve.setData(t, d[row])
                    for curve, row in zip(self.c_gyr, self.emotibit.gyro):
                        curve.setData(t, d[row])
                    for curve, row in zip(self.c_mag, self.emotibit.mag):
                        curve.setData(t, d[row])
            except BrainFlowError:
                pass

    def closeEvent(self, event):
        self.timer.stop()
        self.cyton.disconnect()
        self.emotibit.disconnect()
        super().closeEvent(event)


def main():
    BoardShim.disable_board_logger()
    app = QtWidgets.QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
