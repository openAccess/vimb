#!/usr/bin/env python3
"""A websocket server for testing vimb's archiving of websockets: on
ws://127.0.0.1:PORT/echo it says "hello", then answers each message with
"echo " and the message. Speaks the subprotocol vimb-test. Needs the
websockets package.

    python3 tests/archive/ws-server.py PORT"""

import asyncio
import sys

from websockets.asyncio.server import serve


async def echo(ws):
    await ws.send("hello")
    async for message in ws:
        await ws.send("echo " + message)


async def main(port):
    async with serve(echo, "127.0.0.1", port, subprotocols=["vimb-test"]):
        await asyncio.get_running_loop().create_future()


asyncio.run(main(int(sys.argv[1])))
