#pragma once

// Transport for the narrator bridge: a TCP listener speaking newline-delimited
// JSON, run on its own thread with boost::asio. Nothing in this file touches
// game objects. Inbound commands are queued for the world thread to drain;
// outbound lines are posted from any thread onto the io thread, which owns the
// sockets. See PROTOCOL.md in the Azeroth_Narrator repository.

#include "Common.h"
#include "playerbot/bridge/BridgeJson.h"

#include <boost/asio.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

class BridgeConnection;

struct BridgeInbound
{
    std::weak_ptr<BridgeConnection> from;
    Json message;   // a validated "command" message: name, id, args
};

class BridgeServer
{
public:
    static constexpr size_t MAX_LINE_BYTES = 64 * 1024;
    static constexpr size_t MAX_QUEUED_LINES_PER_CLIENT = 10000;

    BridgeServer();
    ~BridgeServer();

    bool Start(const std::string& bindIp, uint16 port, uint32 maxClients, std::string helloLine);
    void Stop();
    bool IsRunning() const { return running_; }
    uint32 ClientCount() const { return clientCount_; }

    // Thread-safe. Lines must be newline terminated (use JsonLine).
    void Broadcast(std::string line);
    void Send(const std::weak_ptr<BridgeConnection>& to, std::string line);

    // World thread: move every queued command into out.
    void Drain(std::deque<BridgeInbound>& out);

    // Called by connections on the io thread.
    void OnLine(const std::shared_ptr<BridgeConnection>& from, std::string line);
    void OnConnectionClosed(const std::shared_ptr<BridgeConnection>& connection);

private:
    void DoAccept();

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread thread_;
    std::set<std::shared_ptr<BridgeConnection>> clients_;   // io thread only
    std::string hello_;
    uint32 maxClients_ = 4;
    std::atomic<bool> running_{false};
    std::atomic<uint32> clientCount_{0};

    std::mutex inboundMutex_;
    std::deque<BridgeInbound> inbound_;
};

class BridgeConnection : public std::enable_shared_from_this<BridgeConnection>
{
public:
    BridgeConnection(BridgeServer& server, boost::asio::ip::tcp::socket socket);

    // All three run on the io thread only.
    void Start(const std::string& hello);
    void Send(std::string line);
    void Close();

    const std::string& Peer() const { return peer_; }

private:
    void ReadLine();
    void WriteNext();

    BridgeServer& server_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::streambuf readBuffer_;
    std::deque<std::string> writeQueue_;
    std::string peer_;
    bool writing_ = false;
    bool closed_ = false;
};
