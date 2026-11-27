"""CLI entry point for BlueSpin Pi."""

from __future__ import annotations

import asyncio
import logging
import os
import signal

from .app import BlueSpinApp
from .config import BlueSpinConfig


async def _run() -> None:
    config = BlueSpinConfig.from_env()
    app = BlueSpinApp(config)
    stop_event = asyncio.Event()

    loop = asyncio.get_running_loop()
    for sig_name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, sig_name, None)
        if sig is not None:
            try:
                loop.add_signal_handler(sig, stop_event.set)
            except NotImplementedError:
                pass

    await app.start()
    try:
        await stop_event.wait()
    finally:
        await app.stop()


def main() -> None:
    level = os.environ.get("BLUESPIN_LOG_LEVEL", "INFO").upper()
    logging.basicConfig(
        level=getattr(logging, level, logging.INFO),
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
