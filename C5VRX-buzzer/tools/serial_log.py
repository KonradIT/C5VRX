"""Log serial lines with timestamps, reopen the port on errors, and send keys
found in a command file.

usage: serial_log.py PORT BAUD LOGFILE CMDFILE

Whenever CMDFILE exists its bytes are written to the port and the file is
deleted, so console keys ('+', '-', 'a', 'm', 'z', 'h') can be sent from
another process without touching the port:

    python -c "open('cmd.txt','wb').write(b'h')"

The XIAO ESP32-C5 re-enumerates on every reset and USB-Serial/JTAG gets
wedged by concurrent opens, so keep exactly one instance of this logger
running per port and never open the port from anything else while it runs
(stop it before flashing).
"""
import os
import sys
import time

import serial

port, baud, out, cmdfile = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]

with open(out, "a", encoding="utf-8", buffering=1) as fp:
    def note(msg):
        fp.write(time.strftime("%H:%M:%S ") + msg + "\n")

    while True:
        ser = None
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baud
            ser.timeout = 0.2
            ser.dtr = True
            ser.rts = True
            ser.open()
            note(f"--- attached to {port}")
            while True:
                if os.path.exists(cmdfile):
                    try:
                        with open(cmdfile, "rb") as cf:
                            data = cf.read()
                        os.remove(cmdfile)
                        ser.write(data)
                        note(f"--- sent {data!r}")
                    except OSError as exc:
                        note(f"--- command file error: {exc}")
                line = ser.readline()
                if line:
                    fp.write(time.strftime("%H:%M:%S ") + line.decode("utf-8", "replace").rstrip("\r\n") + "\n")
        except serial.SerialException as exc:
            note(f"--- port error: {exc}; reopening")
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
            time.sleep(0.5)
