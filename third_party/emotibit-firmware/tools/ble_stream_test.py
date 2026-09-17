"""BLE reception test for the bioacq EmotiBit BLE firmware (test tool, not part of the app).

Scans for "EmotiBit: <id>", subscribes to the Nordic-UART-style TX characteristic,
parses EmotiBit CSV packets and reports throughput, per-type sample rates, malformed
lines and gaps in the packet counter.
"""
import asyncio
import collections
import sys
import time

from bleak import BleakClient, BleakScanner

TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
EXPECTED = {"PI": 25, "PR": 25, "PG": 25, "AX": 25, "AY": 25, "AZ": 25, "GX": 25, "GY": 25, "GZ": 25,
            "MX": 25, "MY": 25, "MZ": 25, "EA": 15, "T1": 7.5, "TH": 7.5}


async def main():
    print("scanning for an EmotiBit (15 s)...", flush=True)
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith("EmotiBit"), timeout=15.0)
    if dev is None:
        print("no EmotiBit advertising over BLE")
        return 1
    print(f"found {dev.name} ({dev.address})", flush=True)

    buf = bytearray()
    stats = {"bytes": 0, "notifs": 0, "lines": 0, "malformed": 0}
    samples = collections.Counter()
    packet_numbers = []
    first_rx = [None]

    def on_notify(_, data: bytearray):
        now = time.monotonic()
        if first_rx[0] is None:
            first_rx[0] = now
        stats["bytes"] += len(data)
        stats["notifs"] += 1
        buf.extend(data)
        while True:
            i = buf.find(b"\n")
            if i < 0:
                break
            line = bytes(buf[:i]).decode("ascii", "replace").strip("\r\x00 ")
            del buf[: i + 1]
            if not line:
                continue
            stats["lines"] += 1
            f = line.split(",")
            try:
                n_data = int(f[2])
                tag = f[3]
                pnum = int(f[1])
            except (IndexError, ValueError):
                stats["malformed"] += 1
                continue
            if len(f) - 6 != n_data:
                stats["malformed"] += 1
                continue
            packet_numbers.append(pnum)
            samples[tag] += n_data

    async with BleakClient(dev) as client:
        print(f"connected, ATT MTU {client.mtu_size}", flush=True)
        await client.start_notify(TX_UUID, on_notify)
        t0 = time.monotonic()
        while time.monotonic() - t0 < DURATION:
            await asyncio.sleep(1.0)
        await client.stop_notify(TX_UUID)

    if first_rx[0] is None:
        print("connected but no notifications arrived")
        return 1
    el = time.monotonic() - first_rx[0]
    print(f"\n{el:.1f} s of data: {stats['bytes'] / el / 1000:.2f} kB/s, {stats['notifs'] / el:.1f} notifications/s, "
          f"{stats['bytes'] / max(1, stats['notifs']):.0f} bytes each; {stats['lines']} packets, "
          f"{stats['malformed']} malformed")
    uniq = sorted(set(packet_numbers))
    if uniq:
        # the counter wraps at 16 bits in some firmware versions; count only forward gaps under 1000
        gaps = sum(b - a - 1 for a, b in zip(uniq, uniq[1:]) if 0 < b - a - 1 < 1000)
        print(f"packet counter {uniq[0]}..{uniq[-1]}: {gaps} missing packets "
              f"({100.0 * gaps / max(1, len(uniq) + gaps):.2f} %)")
    print("\ntype  samples/s  expected")
    for tag in sorted(set(samples) | set(EXPECTED)):
        rate = samples[tag] / el
        exp = EXPECTED.get(tag)
        flag = "" if exp is None else ("  OK" if abs(rate - exp) <= 0.1 * exp else "  <-- off")
        print(f"{tag:>4}  {rate:9.2f}  {'' if exp is None else exp}{flag}")
    return 0


sys.exit(asyncio.run(main()))
