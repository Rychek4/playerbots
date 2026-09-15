#include "playerbot/bridge/BridgeServer.h"

#include "Log/Log.h"

#include <istream>

using boost::asio::ip::tcp;

namespace
{
    std::string ErrorLine(const std::string& error)
    {
        Json message;
        message["type"] = "error";
        message["error"] = error;
        return JsonLine(message);
    }
}

// BridgeServer ---------------------------------------------------------------

BridgeServer::BridgeServer() = default;

BridgeServer::~BridgeServer()
{
    Stop();
}

bool BridgeServer::Start(const std::string& bindIp, uint16 port, uint32 maxClients, std::string helloLine)
{
    if (running_)
        return true;

    hello_ = std::move(helloLine);
    maxClients_ = maxClients;

    try
    {
        io_.restart();
        work_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io_));
        tcp::endpoint endpoint(boost::asio::ip::make_address(bindIp), port);
        acceptor_ = std::make_unique<tcp::acceptor>(io_);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen();
    }
    catch (std::exception const& e)
    {
        sLog.outError("Bridge: cannot listen on %s:%u: %s", bindIp.c_str(), uint32(port), e.what());
        acceptor_.reset();
        work_.reset();
        return false;
    }

    running_ = true;
    DoAccept();
    thread_ = std::thread([this]()
    {
        try
        {
            io_.run();
        }
        catch (std::exception const& e)
        {
            sLog.outError("Bridge: io thread terminated: %s", e.what());
        }
    });
    return true;
}

void BridgeServer::Stop()
{
    if (!running_)
        return;

    running_ = false;
    io_.stop();   // run() returns; sockets close with the io_context's objects
    if (thread_.joinable())
        thread_.join();
    clients_.clear();
    clientCount_ = 0;
    acceptor_.reset();
    work_.reset();
    std::lock_guard<std::mutex> guard(inboundMutex_);
    inbound_.clear();
}

void BridgeServer::DoAccept()
{
    acceptor_->async_accept([this](boost::system::error_code ec, tcp::socket socket)
    {
        if (!running_)
            return;

        if (!ec)
        {
            if (clientCount_ >= maxClients_)
            {
                sLog.outError("Bridge: refusing a client, limit of %u reached", maxClients_);
                boost::system::error_code ignored;
                socket.close(ignored);
            }
            else
            {
                auto connection = std::make_shared<BridgeConnection>(*this, std::move(socket));
                clients_.insert(connection);
                ++clientCount_;
                sLog.outString("Bridge: client connected from %s", connection->Peer().c_str());
                connection->Start(hello_);
            }
        }
        else if (ec != boost::asio::error::operation_aborted)
            sLog.outError("Bridge: accept failed: %s", ec.message().c_str());

        if (running_)
            DoAccept();
    });
}

void BridgeServer::Broadcast(std::string line)
{
    if (!running_ || clientCount_ == 0)
        return;

    boost::asio::post(io_, [this, line = std::move(line)]()
    {
        // Send never erases from clients_ synchronously, so iterating is safe.
        for (auto const& client : clients_)
            client->Send(line);
    });
}

void BridgeServer::Send(const std::weak_ptr<BridgeConnection>& to, std::string line)
{
    if (!running_)
        return;

    boost::asio::post(io_, [to, line = std::move(line)]()
    {
        if (auto client = to.lock())
            client->Send(line);
    });
}

void BridgeServer::Drain(std::deque<BridgeInbound>& out)
{
    std::lock_guard<std::mutex> guard(inboundMutex_);
    if (out.empty())
        out.swap(inbound_);
    else
    {
        out.insert(out.end(), std::make_move_iterator(inbound_.begin()), std::make_move_iterator(inbound_.end()));
        inbound_.clear();
    }
}

void BridgeServer::OnLine(const std::shared_ptr<BridgeConnection>& from, std::string line)
{
    Json message = Json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object() || !message.contains("type") || !message["type"].is_string())
    {
        from->Send(ErrorLine("line is not a JSON object with a string 'type'"));
        return;
    }

    const std::string type = message["type"];
    if (type == "ping")
    {
        Json pong;
        pong["type"] = "pong";
        if (message.contains("id"))
            pong["id"] = message["id"];
        from->Send(JsonLine(pong));
        return;
    }
    if (type != "command")
    {
        from->Send(ErrorLine("unexpected message type '" + type + "'"));
        return;
    }
    if (!message.contains("name") || !message["name"].is_string() || message["name"].get<std::string>().empty())
    {
        from->Send(ErrorLine("command 'name' must be a non-empty string"));
        return;
    }
    if (!message.contains("id") || !(message["id"].is_string() || message["id"].is_number_integer()))
    {
        from->Send(ErrorLine("command 'id' must be a string or integer"));
        return;
    }
    if (message.contains("args") && !message["args"].is_object())
    {
        from->Send(ErrorLine("command 'args' must be an object"));
        return;
    }
    if (!message.contains("args"))
        message["args"] = Json::object();

    std::lock_guard<std::mutex> guard(inboundMutex_);
    inbound_.push_back(BridgeInbound{from, std::move(message)});
}

void BridgeServer::OnConnectionClosed(const std::shared_ptr<BridgeConnection>& connection)
{
    if (clients_.erase(connection))
    {
        --clientCount_;
        sLog.outString("Bridge: client %s disconnected", connection->Peer().c_str());
    }
}

// BridgeConnection -----------------------------------------------------------

BridgeConnection::BridgeConnection(BridgeServer& server, tcp::socket socket)
    : server_(server), socket_(std::move(socket)), readBuffer_(BridgeServer::MAX_LINE_BYTES + 1)
{
    boost::system::error_code ec;
    tcp::endpoint endpoint = socket_.remote_endpoint(ec);
    peer_ = ec ? std::string("unknown") : endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

void BridgeConnection::Start(const std::string& hello)
{
    Send(hello);
    ReadLine();
}

void BridgeConnection::ReadLine()
{
    auto self = shared_from_this();
    boost::asio::async_read_until(socket_, readBuffer_, '\n', [this, self](boost::system::error_code ec, std::size_t)
    {
        if (closed_)
            return;

        if (ec)
        {
            if (ec == boost::asio::error::not_found)
                sLog.outError("Bridge: client %s sent a line over %u bytes, dropping it", peer_.c_str(), uint32(BridgeServer::MAX_LINE_BYTES));
            Close();
            return;
        }

        std::istream stream(&readBuffer_);
        std::string line;
        std::getline(stream, line);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            server_.OnLine(self, std::move(line));

        ReadLine();
    });
}

void BridgeConnection::Send(std::string line)
{
    if (closed_)
        return;

    if (writeQueue_.size() >= BridgeServer::MAX_QUEUED_LINES_PER_CLIENT)
    {
        // A client that stopped reading must not grow the queue forever. Close
        // on the next io turn, never from inside a broadcast loop.
        sLog.outError("Bridge: client %s is not reading, dropping it", peer_.c_str());
        auto self = shared_from_this();
        boost::asio::post(socket_.get_executor(), [self]() { self->Close(); });
        return;
    }

    writeQueue_.push_back(std::move(line));
    if (!writing_)
        WriteNext();
}

void BridgeConnection::WriteNext()
{
    if (closed_ || writeQueue_.empty())
    {
        writing_ = false;
        return;
    }

    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(writeQueue_.front()), [this, self](boost::system::error_code ec, std::size_t)
    {
        if (closed_)
            return;
        if (ec)
        {
            Close();
            return;
        }
        writeQueue_.pop_front();
        WriteNext();
    });
}

void BridgeConnection::Close()
{
    if (closed_)
        return;

    closed_ = true;
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    server_.OnConnectionClosed(shared_from_this());
}
