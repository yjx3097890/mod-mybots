#include "MyBotsHttpServer.h"
#include "MyBotsConfig.h"
#include "MyBotsIntentQueue.h"

#include "Log.h"
#include "Define.h"

#include <atomic>
#include <boost/asio.hpp>
#include <cctype>
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>

namespace
{
std::atomic<bool> gRunning{false};
std::thread gThread;

std::string UrlDecode(std::string const& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out.push_back(static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2])));
            i += 2;
        }
        else if (in[i] == '+')
            out.push_back(' ');
        else
            out.push_back(in[i]);
    }
    return out;
}

std::string HeaderValue(std::string const& headers, std::string const& key)
{
    std::string lowerKey = key;
    for (char& c : lowerKey)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerHeaders = headers;
    for (char& c : lowerHeaders)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto pos = lowerHeaders.find(lowerKey + ":");
    if (pos == std::string::npos)
        return {};
    pos = headers.find(':', pos);
    if (pos == std::string::npos)
        return {};
    ++pos;
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t'))
        ++pos;
    auto end = headers.find("\r\n", pos);
    if (end == std::string::npos)
        end = headers.size();
    return headers.substr(pos, end - pos);
}

bool TokenOk(std::string const& headers)
{
    std::string const expected = sMyBotsConfig.ApiToken();
    if (expected.empty() || expected == "change-me")
        return false;

    std::string auth = HeaderValue(headers, "Authorization");
    std::string const prefix = "Bearer ";
    if (auth.size() > prefix.size() && auth.compare(0, prefix.size(), prefix) == 0)
        return auth.substr(prefix.size()) == expected;

    return HeaderValue(headers, "X-MyBots-Token") == expected;
}

void QueryParam(std::string const& query, std::string const& key, std::string& value)
{
    auto pos = query.find(key + "=");
    if (pos == std::string::npos)
        return;
    pos += key.size() + 1;
    auto end = query.find('&', pos);
    value = UrlDecode(query.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

void Send(boost::asio::ip::tcp::socket& sock, int status, std::string const& body)
{
    char const* reason = "OK";
    if (status == 202) reason = "Accepted";
    else if (status == 400) reason = "Bad Request";
    else if (status == 401) reason = "Unauthorized";
    else if (status == 404) reason = "Not Found";
    else if (status == 409) reason = "Conflict";
    else if (status == 429) reason = "Too Many Requests";
    else if (status == 503) reason = "Service Unavailable";
    else if (status >= 500) reason = "Internal Server Error";

    std::ostringstream res;
    res << "HTTP/1.1 " << status << " " << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(res.str()), ec);
}

bool JsonBool(std::string const& body, char const* key, bool defaultValue)
{
    auto pos = body.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos)
        return defaultValue;
    auto t = body.find("true", pos);
    auto f = body.find("false", pos);
    if (f != std::string::npos && (t == std::string::npos || f < t))
        return false;
    if (t != std::string::npos)
        return true;
    return defaultValue;
}

std::string JsonString(std::string const& body, char const* key)
{
    auto pos = body.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos)
        return {};
    auto colon = body.find(':', pos);
    auto q1 = body.find('"', colon + 1);
    auto q2 = body.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos)
        return {};
    return body.substr(q1 + 1, q2 - q1 - 1);
}

void HandleRequest(boost::asio::ip::tcp::socket sock)
{
    try
    {
        boost::asio::streambuf buf;
        boost::system::error_code ec;
        std::size_t const n = boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
        if (ec)
            return;

        std::istream is(&buf);
        std::string request(n, '\0');
        is.read(request.data(), static_cast<std::streamsize>(n));

        auto const lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos)
        {
            Send(sock, 400, "{\"ok\":false,\"code\":\"bad_request\"}");
            return;
        }

        std::string const requestLine = request.substr(0, lineEnd);
        std::string method;
        std::string target;
        {
            std::istringstream ls(requestLine);
            std::string version;
            ls >> method >> target >> version;
        }

        std::string const headers = request.substr(lineEnd + 2);
        uint32 contentLength = 0;
        if (std::string cl = HeaderValue(headers, "Content-Length"); !cl.empty())
            contentLength = static_cast<uint32>(std::stoul(cl));

        std::string body;
        std::string leftover(std::istreambuf_iterator<char>(is), {});
        body = leftover;
        while (body.size() < contentLength)
        {
            char tmp[1024];
            std::size_t const got = sock.read_some(boost::asio::buffer(tmp), ec);
            if (ec || got == 0)
                break;
            body.append(tmp, got);
        }
        if (body.size() > contentLength)
            body.resize(contentLength);

        if (method == "GET" && (target == "/health" || target == "/v1/health"))
        {
            Send(sock, 200, "{\"ok\":true,\"service\":\"mod-mybots\",\"version\":\"0.2.0\"}");
            return;
        }

        if (!TokenOk(headers))
        {
            Send(sock, 401, "{\"ok\":false,\"code\":\"unauthorized\",\"message\":\"Invalid or missing API token\"}");
            return;
        }

        std::string path = target;
        std::string query;
        if (auto q = target.find('?'); q != std::string::npos)
        {
            path = target.substr(0, q);
            query = target.substr(q + 1);
        }

        // Global patrol routes (no character)
        if (method == "GET" && path == "/v1/patrols")
        {
            MyBotsIntent intent;
            intent.op = MyBotsIntentOp::ListPatrols;
            auto queued = sMyBotsIntentQueue.Submit(std::move(intent));
            if (!queued)
            {
                Send(sock, 429, "{\"ok\":false,\"code\":\"queue_full\"}");
                return;
            }
            if (!sMyBotsIntentQueue.Wait(queued, sMyBotsConfig.ApiTimeoutMs()))
            {
                Send(sock, 503, "{\"ok\":false,\"code\":\"timeout\"}");
                return;
            }
            Send(sock, queued->httpStatus, queued->response);
            return;
        }
        if (method == "POST" && path == "/v1/patrols")
        {
            MyBotsIntent intent;
            intent.op = MyBotsIntentOp::UpsertPatrol;
            intent.payload = body.empty() ? "{}" : body;
            auto queued = sMyBotsIntentQueue.Submit(std::move(intent));
            if (!queued)
            {
                Send(sock, 429, "{\"ok\":false,\"code\":\"queue_full\"}");
                return;
            }
            if (!sMyBotsIntentQueue.Wait(queued, sMyBotsConfig.ApiTimeoutMs()))
            {
                Send(sock, 503, "{\"ok\":false,\"code\":\"timeout\"}");
                return;
            }
            Send(sock, queued->httpStatus, queued->response);
            return;
        }

        std::string name;
        std::string guidStr;
        QueryParam(query, "name", name);
        QueryParam(query, "guid", guidStr);

        std::string restAfterChar;
        auto consumeCharacterPath = [&](std::string const& prefix) -> bool {
            if (path.rfind(prefix, 0) != 0)
                return false;
            std::string rest = path.substr(prefix.size());
            auto slash = rest.find('/');
            std::string ident = slash == std::string::npos ? rest : rest.substr(0, slash);
            restAfterChar = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
            if (ident.empty())
                return false;
            ident = UrlDecode(ident);
            bool numeric = !ident.empty();
            for (char c : ident)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    numeric = false;
            if (numeric)
                guidStr = ident;
            else
                name = ident;
            return true;
        };

        if (!consumeCharacterPath("/v1/characters/"))
        {
            Send(sock, 404, "{\"ok\":false,\"code\":\"not_found\",\"message\":\"Unknown route\"}");
            return;
        }

        uint32 guidLow = guidStr.empty() ? 0 : static_cast<uint32>(std::stoul(guidStr));
        if (name.empty() && guidLow == 0)
        {
            Send(sock, 400, "{\"ok\":false,\"code\":\"bad_request\",\"message\":\"Provide character name or guid\"}");
            return;
        }

        MyBotsIntent intent;
        intent.playerName = name;
        intent.guidLow = guidLow;
        intent.payload = body.empty() ? "{}" : body;
        intent.replace = JsonBool(body, "replace", sMyBotsConfig.JobReplace());

        if (method == "GET" && restAfterChar.empty())
            intent.op = MyBotsIntentOp::Status;
        else if (method == "POST" && restAfterChar == "selfbot")
            intent.op = JsonBool(body, "enabled", true) ? MyBotsIntentOp::SelfbotOn : MyBotsIntentOp::SelfbotOff;
        else if (method == "POST" && restAfterChar == "jobs")
        {
            intent.op = MyBotsIntentOp::AssignJob;
            intent.jobType = JsonString(body, "type");
            if (intent.jobType.empty())
                intent.jobType = "move_to";
        }
        else if (method == "GET" && restAfterChar == "jobs")
            intent.op = MyBotsIntentOp::ListJobs;
        else if (method == "GET" && restAfterChar.rfind("jobs/", 0) == 0)
        {
            intent.op = MyBotsIntentOp::GetJob;
            intent.jobId = restAfterChar.substr(5);
            auto slash = intent.jobId.find('/');
            if (slash != std::string::npos)
                intent.jobId = intent.jobId.substr(0, slash);
        }
        else if (method == "POST" && restAfterChar.rfind("jobs/", 0) == 0 && restAfterChar.find("/pause") != std::string::npos)
        {
            intent.op = MyBotsIntentOp::PauseJob;
            intent.jobId = restAfterChar.substr(5, restAfterChar.find("/pause") - 5);
        }
        else if (method == "POST" && restAfterChar.rfind("jobs/", 0) == 0 && restAfterChar.find("/resume") != std::string::npos)
        {
            intent.op = MyBotsIntentOp::ResumeJob;
            intent.jobId = restAfterChar.substr(5, restAfterChar.find("/resume") - 5);
        }
        else if (method == "DELETE" && restAfterChar.rfind("jobs/", 0) == 0)
        {
            intent.op = MyBotsIntentOp::CancelJob;
            intent.jobId = restAfterChar.substr(5);
        }
        else if (method == "DELETE" && restAfterChar == "jobs")
            intent.op = MyBotsIntentOp::CancelJob;
        else if (method == "GET" && restAfterChar == "events")
            intent.op = MyBotsIntentOp::ListEvents;
        else
        {
            Send(sock, 404, "{\"ok\":false,\"code\":\"not_found\",\"message\":\"Unknown route\"}");
            return;
        }

        auto queued = sMyBotsIntentQueue.Submit(std::move(intent));
        if (!queued)
        {
            Send(sock, 429, "{\"ok\":false,\"code\":\"queue_full\",\"message\":\"Intent queue is full\"}");
            return;
        }
        if (!sMyBotsIntentQueue.Wait(queued, sMyBotsConfig.ApiTimeoutMs()))
        {
            Send(sock, 503, "{\"ok\":false,\"code\":\"timeout\",\"message\":\"World thread did not process the intent in time\"}");
            return;
        }
        Send(sock, queued->httpStatus, queued->response);
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module.mybots", "API request failed: {}", e.what());
    }
}

void RunServer()
{
    using boost::asio::ip::tcp;
    try
    {
        boost::asio::io_context io;
        auto const addr = boost::asio::ip::make_address(sMyBotsConfig.ApiBind());
        tcp::acceptor acceptor(io, tcp::endpoint(addr, sMyBotsConfig.ApiPort()));
        LOG_INFO("module.mybots", "JSON API listening on {}:{}", sMyBotsConfig.ApiBind(), sMyBotsConfig.ApiPort());

        while (gRunning.load())
        {
            tcp::socket sock(io);
            boost::system::error_code ec;
            acceptor.accept(sock, ec);
            if (ec)
            {
                if (!gRunning.load())
                    break;
                continue;
            }
            std::thread(HandleRequest, std::move(sock)).detach();
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module.mybots", "JSON API failed to start: {}", e.what());
    }
}
} // namespace

void MyBotsHttpServer::Start()
{
    if (!sMyBotsConfig.Enable() || !sMyBotsConfig.ApiEnable())
        return;

    if (sMyBotsConfig.ApiToken().empty() || sMyBotsConfig.ApiToken() == "change-me")
    {
        LOG_ERROR("module.mybots", "JSON API not started: set MyBots.Api.Token to a non-default secret");
        return;
    }

    if (gRunning.exchange(true))
        return;

    gThread = std::thread(RunServer);
}

void MyBotsHttpServer::Stop()
{
    if (!gRunning.exchange(false))
        return;

    try
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket sock(io);
        auto const addr = boost::asio::ip::make_address(sMyBotsConfig.ApiBind());
        sock.connect(boost::asio::ip::tcp::endpoint(addr, sMyBotsConfig.ApiPort()));
    }
    catch (...)
    {
    }

    if (gThread.joinable())
        gThread.join();
}
