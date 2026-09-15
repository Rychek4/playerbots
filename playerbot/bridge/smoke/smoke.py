"""Drive the C++ BridgeServer harness with the real Python client."""
import asyncio, sys
sys.path.insert(0, sys.argv[2] if len(sys.argv) > 2 else "../../../../Azeroth_Narrator")
from narrator import protocol as P
from narrator.bridge import BridgeClient, CommandError

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18890

async def main():
    client = BridgeClient(port=PORT)
    hello = await client.connect()
    assert hello["server"] == "smoke-harness", hello
    print("hello ok:", hello)

    rtt = await client.ping()
    print(f"ping ok: {rtt*1000:.1f} ms")

    result = await client.send_command("echo", {"x": 1, "text": "héllo wörld", "nested": {"a": [1, 2]}})
    assert result == {"x": 1, "text": "héllo wörld", "nested": {"a": [1, 2]}}, result
    print("echo ok:", result)

    try:
        await client.send_command("does.not.exist")
        raise SystemExit("expected an error reply")
    except CommandError as exc:
        print("unknown command ok:", exc.error)

    # events keep flowing with increasing seq
    seqs = []
    async for ev in client.events():
        seqs.append(ev["seq"])
        if len(seqs) >= 5:
            break
    assert seqs == sorted(seqs) and len(set(seqs)) == 5, seqs
    print("events ok: seq", seqs, "clients reported:", ev["data"]["clients"])

    # a second client at the same time sees the broadcast too and can send commands
    second = BridgeClient(port=PORT)
    await second.connect()
    r2 = await second.send_command("echo", {"who": "second"})
    assert r2 == {"who": "second"}
    async for ev in second.events():
        assert ev["data"]["clients"] == 2, ev
        break
    print("second client ok, broadcast reaches both")
    await second.close()

    # raw protocol errors: garbage, bad command, oversize line
    reader, writer = await asyncio.open_connection("127.0.0.1", PORT)
    await reader.readline()  # hello
    writer.write(b"garbage\n")
    while True:
        line = P.decode(await reader.readline())
        if line["type"] == "error":
            print("garbage ok:", line["error"]); break
    writer.write(P.encode({"type": "command", "name": "echo"}))  # no id
    while True:
        line = P.decode(await reader.readline())
        if line["type"] == "error":
            print("bad command ok:", line["error"]); break
    writer.write(b"x" * (P.MAX_LINE_BYTES + 10) + b"\n")
    await writer.drain()
    # the server drops the link
    deadline = asyncio.get_running_loop().time() + 5
    while asyncio.get_running_loop().time() < deadline:
        line = await reader.readline()
        if not line:
            print("oversize line ok: server closed the link"); break
    else:
        raise SystemExit("server did not close after an oversize line")
    writer.close()

    await client.send_command("quit")
    async for _ in client.events():
        pass
    assert not client.connected
    print("quit ok: link closed cleanly")

asyncio.run(main())
