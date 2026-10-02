from __future__ import annotations

import asyncio
import csv
import struct
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner


DEVICE_NAME = "CatMotion-01"

IMU_CHARACTERISTIC_UUID = (
    "5f2b0002-8a2f-4b7e-9d3c-112233445566"
)


def get_next_output_file() -> Path:
    index = 1
    while True:
        output_file = Path(f"cat_imu_ble{index}.csv")
        if not output_file.exists():
            return output_file
        index += 1


OUTPUT_FILE = get_next_output_file()
DURATION_SECONDS = 60
STALL_TIMEOUT_SECONDS = 8.0
RECONNECT_DELAY_SECONDS = 2.0

# <
#   小端格式
#
# I
#   uint32_t 时间戳
#
# hhhhhh
#   六个 int16_t
#
# 总长度：16字节
PACKET_STRUCT = struct.Struct("<Ihhhhhh")


async def main() -> None:
    print(f"正在扫描 {DEVICE_NAME} ...")

    device = await BleakScanner.find_device_by_name(
        DEVICE_NAME,
        timeout=15.0,
    )

    if device is None:
        raise RuntimeError(
            f"没有发现 {DEVICE_NAME}。"
            "请确认XIAO已烧录成功，并正在供电。"
        )

    print(f"发现设备：{device.name}")
    print(f"设备地址：{device.address}")
    print("正在连接...")

    received_count = 0
    invalid_count = 0
    previous_device_ms: int | None = None
    estimated_lost_count = 0
    last_notification_time = time.monotonic()
    connection_count = 0

    with OUTPUT_FILE.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as file:
        writer = csv.writer(file)

        writer.writerow(
            [
                "ms",
                "ax_g",
                "ay_g",
                "az_g",
                "gx_dps",
                "gy_dps",
                "gz_dps",
            ]
        )

        def on_notification(
            _characteristic: object,
            data: bytearray,
        ) -> None:
            nonlocal received_count
            nonlocal invalid_count
            nonlocal previous_device_ms
            nonlocal estimated_lost_count
            nonlocal last_notification_time

            last_notification_time = time.monotonic()

            if len(data) != PACKET_STRUCT.size:
                invalid_count += 1
                print(
                    f"收到异常长度数据："
                    f"{len(data)} bytes"
                )
                return

            (
                device_ms,
                ax_mg,
                ay_mg,
                az_mg,
                gx_dps10,
                gy_dps10,
                gz_dps10,
            ) = PACKET_STRUCT.unpack(data)

            ax_g = ax_mg / 1000.0
            ay_g = ay_mg / 1000.0
            az_g = az_mg / 1000.0

            gx_dps = gx_dps10 / 10.0
            gy_dps = gy_dps10 / 10.0
            gz_dps = gz_dps10 / 10.0

            if previous_device_ms is not None:
                delta_ms = device_ms - previous_device_ms

                # 正常应约为20ms。
                # 例如60ms说明中间可能少了两个数据包。
                if delta_ms > 30:
                    possible_missing = max(
                        round(delta_ms / 20) - 1,
                        0,
                    )
                    estimated_lost_count += possible_missing

            previous_device_ms = device_ms
            received_count += 1

            writer.writerow(
                [
                    device_ms,
                    f"{ax_g:.5f}",
                    f"{ay_g:.5f}",
                    f"{az_g:.5f}",
                    f"{gx_dps:.3f}",
                    f"{gy_dps:.3f}",
                    f"{gz_dps:.3f}",
                ]
            )

            if received_count % 50 == 0:
                file.flush()

                print(
                    f"{device_ms},"
                    f"{ax_g:.5f},"
                    f"{ay_g:.5f},"
                    f"{az_g:.5f},"
                    f"{gx_dps:.3f},"
                    f"{gy_dps:.3f},"
                    f"{gz_dps:.3f}"
                )

        deadline = time.monotonic() + DURATION_SECONDS

        while time.monotonic() < deadline:
            connection_count += 1
            last_notification_time = time.monotonic()
            client = BleakClient(device)

            try:
                await asyncio.wait_for(client.connect(), timeout=15.0)
                print(
                    f"连接状态：{client.is_connected} "
                    f"（第 {connection_count} 次连接）"
                )

                await client.start_notify(
                    IMU_CHARACTERISTIC_UUID,
                    on_notification,
                )

                if connection_count == 1:
                    print(
                        f"开始接收，持续 {DURATION_SECONDS} 秒..."
                    )

                while time.monotonic() < deadline:
                    await asyncio.sleep(1.0)

                    silent_seconds = (
                        time.monotonic() - last_notification_time
                    )
                    if silent_seconds >= STALL_TIMEOUT_SECONDS:
                        file.flush()
                        print(
                            f"连续 {silent_seconds:.1f} 秒无原始数据，"
                            "正在自动重连..."
                        )
                        break
            except Exception as error:
                file.flush()
                print(f"BLE连接异常：{error}，准备重连...")
            finally:
                if client.is_connected:
                    try:
                        await asyncio.wait_for(
                            client.stop_notify(
                                IMU_CHARACTERISTIC_UUID
                            ),
                            timeout=3.0,
                        )
                    except Exception:
                        pass

                    try:
                        await asyncio.wait_for(
                            client.disconnect(),
                            timeout=5.0,
                        )
                    except Exception:
                        print("BLE断开超时，程序继续结束或重连。")

            if time.monotonic() < deadline:
                await asyncio.sleep(RECONNECT_DELAY_SECONDS)

    expected_count = DURATION_SECONDS * 50

    print()
    print("接收完成")
    print(f"实际收到：{received_count}")
    print(f"理论数量：{expected_count}")
    print(f"异常数据：{invalid_count}")
    print(f"估算丢包：{estimated_lost_count}")
    print(f"CSV位置：{OUTPUT_FILE.resolve()}")


if __name__ == "__main__":
    asyncio.run(main())
