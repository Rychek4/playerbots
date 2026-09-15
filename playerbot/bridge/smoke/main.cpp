// Smoke harness: runs the real BridgeServer with a stand-in "world thread"
// that drains the command queue every 100 ms and answers "echo" commands.
#include "playerbot/bridge/BridgeServer.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    const uint16 port = argc > 1 ? uint16(std::atoi(argv[1])) : 18890;
    BridgeServer server;
    Json hello;
    hello["type"] = "hello";
    hello["protocol"] = 1;
    hello["server"] = "smoke-harness";
    hello["build"] = "none";
    hello["realm"] = "0";
    if (!server.Start("127.0.0.1", port, 2, JsonLine(hello)))
        return 1;
    std::cout << "listening " << port << std::endl;

    uint64 seq = 0;
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(30))
    {
        std::deque<BridgeInbound> batch;
        server.Drain(batch);
        for (BridgeInbound const& in : batch)
        {
            const std::string name = in.message["name"];
            Json reply;
            reply["type"] = "reply";
            reply["id"] = in.message["id"];
            if (name == "echo")
            {
                reply["ok"] = true;
                reply["result"] = in.message["args"];
            }
            else if (name == "quit")
            {
                reply["ok"] = true;
                reply["result"] = Json::object();
                server.Send(in.from, JsonLine(reply));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                server.Stop();
                std::cout << "stopped on request" << std::endl;
                return 0;
            }
            else
            {
                reply["ok"] = false;
                reply["error"] = "unknown command: " + name;
            }
            server.Send(in.from, JsonLine(reply));
        }
        Json event;
        event["type"] = "event";
        event["name"] = "tick";
        event["seq"] = ++seq;
        event["t"] = 0;
        event["data"] = Json{{"clients", server.ClientCount()}};
        server.Broadcast(JsonLine(event));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.Stop();
    std::cout << "timed out" << std::endl;
    return 2;
}
