#include "MyBotsLlm.h"
#include "MyBotsConfig.h"
#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsIntentQueue.h"
#include "MyBotsJob.h"
#include "MyBotsQuestPlan.h"
#include "MyBotsUtil.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "SharedDefines.h"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <condition_variable>
#include <mutex>
#include <openssl/ssl.h>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace
{
std::atomic<bool> gRunning{false};
std::thread gThread;
std::mutex gMutex;
std::condition_variable gCv;
struct PlanRequest
{
    std::string jobId;
    uint32 charGuid = 0;
    uint32 questId = 0;
    std::string contextJson;
    bool replan = false;
};
std::queue<PlanRequest> gQueue;

std::string TruncateEventMsg(std::string s, size_t maxLen = 3500)
{
    if (s.size() <= maxLen)
        return s;
    s.resize(maxLen);
    s += "...(truncated)";
    return s;
}

std::string StepsToEventJson(std::vector<MyBotsJobStep> const& steps)
{
    std::ostringstream ss;
    ss << "{\"count\":" << steps.size() << ",\"steps\":[";
    for (size_t i = 0; i < steps.size(); ++i)
    {
        if (i)
            ss << ",";
        ss << "{\"op\":\"" << MyBotsJsonEscapeCopy(steps[i].op)
           << "\",\"detail\":" << (steps[i].detail.empty() ? "{}" : steps[i].detail) << "}";
    }
    ss << "]}";
    return TruncateEventMsg(ss.str());
}

void LogLlmEvent(uint32 charGuid, std::string const& jobId, char const* kind, std::string const& message)
{
    if (!charGuid || jobId.empty())
        return;
    sMyBotsJobStore.AppendEvent(charGuid, jobId, kind, TruncateEventMsg(message));
}

char const* kSystemPrompt =
    "You are a World of Warcraft (3.3.5) quest HTN planner for mod-mybots.\n"
    "Output ONLY one JSON object: {\"steps\":[{\"op\":\"...\",\"detail\":\"{...}\"}]}\n"
    "Allowed ops: ensure_selfbot, travel_to, move_to, interact, gossip_select, accept_quest, "
    "turnin_quest, wait, until, use_item, use_hearthstone.\n"
    "Rules:\n"
    "- First step must be ensure_selfbot with detail {}.\n"
    "- If character.map != hints.hubMap, FIRST emit travel_to with map=hints.hubMap and "
    "x/y/z from hints.hub (or use_hearthstone {\"map\":hubMap} when bind is on that map).\n"
    "- When character.questStatus is incomplete or complete, hints.hub is the TURN-IN NPC "
    "(hints.hubIsTurnin=true). Do NOT travel back to giverEntry.\n"
    "- map id 0 (Eastern Kingdoms) is a REAL map — never omit \"map\" for travel_to.\n"
    "- NEVER skip travel_to just because you know an NPC entry — entry-only move_to "
    "on the wrong continent walks a straight line on the current map.\n"
    "- travel_to is preferred for cross-map; move_to may use map+x/y/z only for pinned sites.\n"
    "- Same-map move_to SHOULD use creature \"entry\" from allowedEntries. Never invent entries.\n"
    "- accept_quest / turnin_quest / until must use the exact questId from the context.\n"
    "- For speak/event objectives (hints.speakObjective=true), prefer move_to entry then until "
    "with speak:1, or interact + gossip_select near that NPC.\n"
    "- For kill/loot objectives, move_to entries then until with those entries.\n"
    "- For summoned kill objectives (hints.summonedObjective=true): NEVER move_to those "
    "summonedEntries. move_to turninEntry (or giver), then use_item with hints.useItemId, "
    "then until with the summoned entries and useItemId.\n"
    "- End with move_to turninEntry (if any) and turnin_quest.\n"
    "- Keep the plan short. No markdown, no commentary.";

char const* kReplanSystemPrompt =
    "You are replanning a World of Warcraft (3.3.5) mod-mybots job after NAVIGATION FAILURE.\n"
    "The low-level navmesh pathfinder already failed (stuck/unreachable/circling). "
    "Do NOT invent fine paths. Only emit HIGH-LEVEL HTN steps.\n"
    "Output ONLY: {\"steps\":[{\"op\":\"...\",\"detail\":\"{...}\"}]}\n"
    "Allowed ops: travel_to, move_to, interact, gossip_select, accept_quest, turnin_quest, "
    "wait, until, use_item, use_hearthstone.\n"
    "Rules:\n"
    "- If on the wrong continent (character.map != hints.hubMap), emit travel_to or "
    "use_hearthstone before any same-map move_to.\n"
    "- If quest is already incomplete/complete, hub is the turn-in — never return to giver.\n"
    "- travel_to may use map + x/y/z from hints.hub only.\n"
    "- Use ONLY creature entries listed in allowedEntries for move_to.\n"
    "- Prefer a different approach than the failed step (other NPC, wait briefly, interact, "
    "speak until, then turn-in).\n"
    "- If hints.summonedObjective=true, use use_item near turninEntry instead of move_to "
    "the summoned creature.\n"
    "- Do NOT repeat accept_quest if character.questStatus is incomplete/complete.\n"
    "- Do NOT invent entries/GUIDs.\n"
    "- Keep the plan short: only what remains to finish the job from the current position.\n"
    "- No markdown, no commentary.";

std::string ExtractJsonArray(std::string const& payload, char const* key)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = payload.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = payload.find('[', pos);
    if (pos == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = pos; i < payload.size(); ++i)
    {
        if (payload[i] == '[')
            ++depth;
        else if (payload[i] == ']')
        {
            --depth;
            if (depth == 0)
                return payload.substr(pos, i - pos + 1);
        }
    }
    return {};
}

bool HasQuestDbScript(uint32 questId)
{
    if (!questId)
        return false;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT 1 FROM mybots_quest_script WHERE quest_id = {} LIMIT 1", questId))
        return true;
    return false;
}

std::string QuestStatusName(QuestStatus st)
{
    switch (st)
    {
        case QUEST_STATUS_NONE: return "none";
        case QUEST_STATUS_COMPLETE: return "complete";
        case QUEST_STATUS_INCOMPLETE: return "incomplete";
        case QUEST_STATUS_FAILED: return "failed";
        case QUEST_STATUS_REWARDED: return "rewarded";
        default: return "unknown";
    }
}

void CollectAllowedEntries(std::string const& contextJson, std::unordered_set<uint32>& out)
{
    std::string arr = ExtractJsonArray(contextJson, "allowedEntries");
    if (arr.empty())
        return;
    size_t i = 0;
    while (i < arr.size())
    {
        while (i < arr.size() && (arr[i] < '0' || arr[i] > '9'))
            ++i;
        if (i >= arr.size())
            break;
        uint32 v = 0;
        while (i < arr.size() && arr[i] >= '0' && arr[i] <= '9')
        {
            v = v * 10 + uint32(arr[i] - '0');
            ++i;
        }
        if (v)
            out.insert(v);
    }
}

bool IsAllowedOp(std::string const& op)
{
    return op == "ensure_selfbot" || op == "move_to" || op == "travel_to" || op == "interact"
        || op == "gossip_select" || op == "accept_quest" || op == "turn_in_quest" || op == "turnin_quest"
        || op == "wait" || op == "until" || op == "use_item" || op == "use_hearthstone"
        || op == "hearthstone";
}

std::string StripCodeFence(std::string s)
{
    auto start = s.find("```");
    if (start == std::string::npos)
        return s;
    start = s.find('\n', start);
    if (start == std::string::npos)
        return s;
    ++start;
    auto end = s.rfind("```");
    if (end == std::string::npos || end <= start)
        return s;
    return s.substr(start, end - start);
}

std::string ExtractStepsObject(std::string const& content)
{
    std::string s = StripCodeFence(content);
    auto pos = s.find('{');
    if (pos == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = pos; i < s.size(); ++i)
    {
        if (s[i] == '{')
            ++depth;
        else if (s[i] == '}')
        {
            --depth;
            if (depth == 0)
                return s.substr(pos, i - pos + 1);
        }
    }
    return {};
}

void ParseStepsArray(std::string const& arr, std::vector<MyBotsJobStep>& out)
{
    size_t i = 0;
    while (i < arr.size())
    {
        auto opPos = arr.find("\"op\"", i);
        if (opPos == std::string::npos)
            break;
        auto colon = arr.find(':', opPos);
        auto q1 = arr.find('"', colon + 1);
        auto q2 = arr.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos)
            break;
        MyBotsJobStep step;
        step.op = arr.substr(q1 + 1, q2 - q1 - 1);
        auto dPos = arr.find("\"detail\"", q2);
        auto nextOp = arr.find("\"op\"", q2 + 1);
        if (dPos != std::string::npos && (nextOp == std::string::npos || dPos < nextOp))
        {
            auto dc = arr.find(':', dPos);
            while (dc + 1 < arr.size() && (arr[dc + 1] == ' ' || arr[dc + 1] == '\t'))
                ++dc;
            if (dc + 1 < arr.size() && arr[dc + 1] == '{')
            {
                int depth = 0;
                size_t start = dc + 1;
                for (size_t k = start; k < arr.size(); ++k)
                {
                    if (arr[k] == '{')
                        ++depth;
                    else if (arr[k] == '}')
                    {
                        --depth;
                        if (depth == 0)
                        {
                            step.detail = arr.substr(start, k - start + 1);
                            i = k + 1;
                            break;
                        }
                    }
                }
            }
            else
            {
                auto dq1 = arr.find('"', dc + 1);
                if (dq1 != std::string::npos)
                {
                    std::string detail;
                    for (size_t k = dq1 + 1; k < arr.size(); ++k)
                    {
                        if (arr[k] == '\\' && k + 1 < arr.size())
                        {
                            detail.push_back(arr[k + 1]);
                            ++k;
                            continue;
                        }
                        if (arr[k] == '"')
                        {
                            step.detail = detail;
                            i = k + 1;
                            break;
                        }
                        detail.push_back(arr[k]);
                    }
                }
            }
        }
        else
        {
            auto objStart = arr.rfind('{', opPos);
            auto objEnd = arr.find('}', q2);
            if (objStart != std::string::npos && objEnd != std::string::npos)
                step.detail = arr.substr(objStart, objEnd - objStart + 1);
            i = objEnd == std::string::npos ? arr.size() : objEnd + 1;
        }
        if (step.detail.empty())
            step.detail = "{}";
        out.push_back(step);
        if (i <= opPos)
            i = q2 + 1;
    }
}

bool ParseUrl(std::string const& baseUrl, std::string& host, std::string& port, std::string& basePath, bool& https)
{
    host.clear();
    port = "443";
    basePath.clear();
    https = true;
    std::string u = baseUrl;
    if (u.rfind("https://", 0) == 0)
    {
        https = true;
        u = u.substr(8);
        port = "443";
    }
    else if (u.rfind("http://", 0) == 0)
    {
        https = false;
        u = u.substr(7);
        port = "80";
    }
    auto slash = u.find('/');
    std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    basePath = slash == std::string::npos ? std::string() : u.substr(slash);
    auto colon = hostPort.find(':');
    if (colon == std::string::npos)
        host = hostPort;
    else
    {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    while (!basePath.empty() && basePath.back() == '/')
        basePath.pop_back();
    return !host.empty();
}

bool HttpPostJson(std::string const& baseUrl, std::string const& targetPath, std::string const& body,
    std::string const& bearer, uint32 timeoutMs, std::string& responseBody, std::string& error)
{
    responseBody.clear();
    error.clear();
    std::string host, port, basePath;
    bool https = true;
    if (!ParseUrl(baseUrl, host, port, basePath, https))
    {
        error = "bad_base_url";
        return false;
    }
    std::string target = basePath + targetPath;
    if (target.empty() || target[0] != '/')
        target = "/" + target;

    try
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(host, port);

        if (!https)
        {
            boost::asio::ip::tcp::socket socket(io);
            boost::asio::connect(socket, endpoints);
            std::ostringstream req;
            req << "POST " << target << " HTTP/1.0\r\n"
                << "Host: " << host << "\r\n"
                << "Content-Type: application/json\r\n"
                << "Accept: application/json\r\n"
                << "Connection: close\r\n"
                << "Content-Length: " << body.size() << "\r\n";
            if (!bearer.empty())
                req << "Authorization: Bearer " << bearer << "\r\n";
            req << "\r\n" << body;
            std::string reqStr = req.str();
            boost::asio::write(socket, boost::asio::buffer(reqStr));

            boost::asio::streambuf buf;
            boost::system::error_code ec;
            while (boost::asio::read(socket, buf, boost::asio::transfer_at_least(1), ec))
            {
                if (ec)
                    break;
            }
            std::istream is(&buf);
            std::string http((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            auto hdrEnd = http.find("\r\n\r\n");
            if (hdrEnd == std::string::npos)
            {
                error = "bad_http_response";
                return false;
            }
            responseBody = http.substr(hdrEnd + 4);
            if (http.rfind("HTTP/1.", 0) != 0 || http.find(" 2") == std::string::npos
                || http.find(" 2") > 12)
            {
                // Accept any 2xx: "HTTP/1.0 200"
                auto sp = http.find(' ');
                if (sp == std::string::npos || sp + 1 >= http.size() || http[sp + 1] != '2')
                {
                    error = "http_status";
                    return false;
                }
            }
            (void)timeoutMs;
            return true;
        }

        boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_client);
        ctx.set_default_verify_paths();
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        {
            error = "sni_failed";
            return false;
        }
        boost::asio::connect(stream.next_layer(), endpoints);
        // Private game hosts often lack a full CA store; planner traffic is API-key gated.
        stream.set_verify_mode(boost::asio::ssl::verify_none);
        stream.handshake(boost::asio::ssl::stream_base::client);

        std::ostringstream req;
        req << "POST " << target << " HTTP/1.0\r\n"
            << "Host: " << host << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Accept: application/json\r\n"
            << "Connection: close\r\n"
            << "Content-Length: " << body.size() << "\r\n";
        if (!bearer.empty())
            req << "Authorization: Bearer " << bearer << "\r\n";
        req << "\r\n" << body;
        std::string reqStr = req.str();
        boost::asio::write(stream, boost::asio::buffer(reqStr));

        boost::asio::streambuf buf;
        boost::system::error_code ec;
        while (boost::asio::read(stream, buf, boost::asio::transfer_at_least(1), ec))
        {
            if (ec)
                break;
        }
        std::istream is(&buf);
        std::string http((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
        auto hdrEnd = http.find("\r\n\r\n");
        if (hdrEnd == std::string::npos)
        {
            error = "bad_http_response";
            return false;
        }
        responseBody = http.substr(hdrEnd + 4);
        auto sp = http.find(' ');
        if (sp == std::string::npos || sp + 1 >= http.size() || http[sp + 1] != '2')
        {
            error = "http_status:" + http.substr(0, std::min<size_t>(http.size(), 32));
            return false;
        }
        (void)timeoutMs;
        return true;
    }
    catch (std::exception const& e)
    {
        error = e.what();
        return false;
    }
}

std::string ExtractAssistantContent(std::string const& apiJson)
{
    // Prefer message.content
    auto key = apiJson.find("\"content\"");
    while (key != std::string::npos)
    {
        auto colon = apiJson.find(':', key);
        if (colon == std::string::npos)
            break;
        auto q1 = apiJson.find('"', colon + 1);
        if (q1 == std::string::npos)
            break;
        std::string out;
        for (size_t k = q1 + 1; k < apiJson.size(); ++k)
        {
            if (apiJson[k] == '\\' && k + 1 < apiJson.size())
            {
                char n = apiJson[k + 1];
                if (n == 'n')
                    out.push_back('\n');
                else if (n == 'r')
                    out.push_back('\r');
                else if (n == 't')
                    out.push_back('\t');
                else
                    out.push_back(n);
                ++k;
                continue;
            }
            if (apiJson[k] == '"')
                return out;
            out.push_back(apiJson[k]);
        }
        break;
    }
    return {};
}

void PostPlanIntent(MyBotsIntentOp op, std::string jobId, uint32 charGuid, std::string payload)
{
    MyBotsIntent intent;
    intent.op = op;
    intent.jobId = std::move(jobId);
    intent.guidLow = charGuid;
    intent.payload = std::move(payload);
    if (!sMyBotsIntentQueue.Submit(std::move(intent)))
        LOG_ERROR("module.mybots", "MyBots LLM: intent queue full when posting plan result");
}

void WorkerLoop()
{
    while (gRunning.load())
    {
        PlanRequest req;
        {
            std::unique_lock<std::mutex> lock(gMutex);
            gCv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return !gQueue.empty() || !gRunning.load();
            });
            if (!gRunning.load() && gQueue.empty())
                break;
            if (gQueue.empty())
                continue;
            req = std::move(gQueue.front());
            gQueue.pop();
        }

        auto result = MyBotsLlm::PlanSync(req.questId, req.contextJson);
        if (!result.rawContent.empty())
            LogLlmEvent(req.charGuid, req.jobId, "llm_raw",
                std::string(req.replan ? "replan;" : "plan;") + result.rawContent);

        if (result.ok)
        {
            LogLlmEvent(req.charGuid, req.jobId, "llm_decision", StepsToEventJson(result.steps));
            std::ostringstream ss;
            ss << "{\"steps\":[";
            for (size_t i = 0; i < result.steps.size(); ++i)
            {
                if (i)
                    ss << ",";
                ss << "{\"op\":\"" << MyBotsJsonEscapeCopy(result.steps[i].op)
                   << "\",\"detail\":\"" << MyBotsJsonEscapeCopy(result.steps[i].detail) << "\"}";
            }
            ss << "]}";
            std::string body = ss.str();
            if (req.replan)
            {
                // Mark replan for ApplyLlmPlan splice.
                if (body.size() >= 2 && body.front() == '{')
                    body = "{\"replan\":1," + body.substr(1);
            }
            PostPlanIntent(MyBotsIntentOp::ApplyLlmPlan, req.jobId, req.charGuid, body);
            continue;
        }

        LOG_WARN("module.mybots", "MyBots LLM plan failed for job {}: {}", req.jobId, result.error);
        LogLlmEvent(req.charGuid, req.jobId, "llm_decision_failed",
            std::string("error=") + (result.error.empty() ? "unknown" : result.error)
                + (result.rawContent.empty() ? "" : ";raw=" + TruncateEventMsg(result.rawContent, 2000)));
        if (req.replan)
        {
            // Mid-job replan failure: fail the job (do not wipe with full rule rebuild).
            PostPlanIntent(MyBotsIntentOp::FailPlan, req.jobId, req.charGuid,
                "{\"error\":\"" + MyBotsJsonEscapeCopy(result.error.empty() ? "replan_failed" : result.error) + "\"}");
            continue;
        }
        if (sMyBotsConfig.LlmFallbackRules())
            PostPlanIntent(MyBotsIntentOp::ApplyRuleFallback, req.jobId, req.charGuid,
                "{\"error\":\"" + MyBotsJsonEscapeCopy(result.error) + "\"}");
        else
            PostPlanIntent(MyBotsIntentOp::FailPlan, req.jobId, req.charGuid,
                "{\"error\":\"" + MyBotsJsonEscapeCopy(result.error.empty() ? "plan_failed" : result.error) + "\"}");
    }
}
} // namespace

void MyBotsLlm::Start()
{
    if (!IsReady())
        return;
    if (gRunning.exchange(true))
        return;
    gThread = std::thread(WorkerLoop);
    LOG_INFO("module.mybots", "MyBots LLM worker started (model={})", sMyBotsConfig.LlmModel());
}

void MyBotsLlm::Stop()
{
    if (!gRunning.exchange(false))
        return;
    gCv.notify_all();
    if (gThread.joinable())
        gThread.join();
    std::lock_guard<std::mutex> lock(gMutex);
    while (!gQueue.empty())
        gQueue.pop();
    LOG_INFO("module.mybots", "MyBots LLM worker stopped");
}

bool MyBotsLlm::IsReady()
{
    return sMyBotsConfig.LlmEnable() && !sMyBotsConfig.LlmApiKey().empty();
}

bool MyBotsLlm::ShouldPlanCompleteQuest(uint32 questId, std::string const& payload)
{
    if (!IsReady() || !questId)
        return false;
    if (!ExtractJsonArray(payload, "steps").empty())
        return false;
    if (HasQuestDbScript(questId))
        return false;
    return true;
}

std::string MyBotsLlm::BuildPlanContext(Player* player, uint32 questId, std::string const& payload)
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"questId\":" << questId;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        ss << ",\"title\":\"" << MyBotsJsonEscapeCopy(quest->GetTitle()) << "\"";
        ss << ",\"specialFlags\":{"
           << "\"explorationOrEvent\":"
           << (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) ? "true" : "false")
           << ",\"speakTo\":"
           << (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_SPEAKTO) ? "true" : "false")
           << "}";
        ss << ",\"objectives\":{";
        ss << "\"npcOrGo\":[";
        bool first = true;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            if (!quest->RequiredNpcOrGoCount[i])
                continue;
            if (!first)
                ss << ",";
            first = false;
            ss << "{\"id\":" << quest->RequiredNpcOrGo[i]
               << ",\"count\":" << quest->RequiredNpcOrGoCount[i] << "}";
        }
        ss << "],\"items\":[";
        first = true;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!quest->RequiredItemId[i] || !quest->RequiredItemCount[i])
                continue;
            if (!first)
                ss << ",";
            first = false;
            ss << "{\"id\":" << quest->RequiredItemId[i]
               << ",\"count\":" << quest->RequiredItemCount[i] << "}";
        }
        ss << "],\"itemDrops\":[";
        first = true;
        for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        {
            if (!quest->ItemDrop[i] || !quest->ItemDropQuantity[i])
                continue;
            if (!first)
                ss << ",";
            first = false;
            ss << "{\"id\":" << quest->ItemDrop[i]
               << ",\"count\":" << quest->ItemDropQuantity[i] << "}";
        }
        ss << "]}";
    }

    MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, payload, player);
    ss << ",\"hints\":{"
       << "\"hasHub\":" << (plan.hasHub ? "true" : "false")
       << ",\"giverEntry\":" << plan.giverEntry
       << ",\"turninEntry\":" << plan.turninEntry
       << ",\"hubEntry\":" << plan.hubEntry
       << ",\"hubIsTurnin\":" << (plan.hubIsTurnin ? "true" : "false")
       << ",\"hubMap\":" << plan.hubMap
       << ",\"hub\":{\"x\":" << plan.hubX << ",\"y\":" << plan.hubY << ",\"z\":" << plan.hubZ << "}"
       << ",\"summonMap\":" << plan.summonMap
       << ",\"speakObjective\":" << (plan.speakObjective ? "true" : "false")
       << ",\"hasObjectives\":" << (plan.hasObjectives ? "true" : "false")
       << ",\"summonedObjective\":" << (plan.HasSummonedObjective() ? "true" : "false")
       << ",\"useItemId\":" << plan.useItemId
       << ",\"objectiveEntries\":[";
    for (size_t i = 0; i < plan.objectiveEntries.size(); ++i)
    {
        if (i)
            ss << ",";
        ss << plan.objectiveEntries[i];
    }
    ss << "],\"summonedEntries\":[";
    for (size_t i = 0; i < plan.summonedEntries.size(); ++i)
    {
        if (i)
            ss << ",";
        ss << plan.summonedEntries[i];
    }
    ss << "]}";

    ss << ",\"allowedEntries\":[";
    {
        bool first = true;
        auto emit = [&](uint32 e) {
            if (!e)
                return;
            if (!first)
                ss << ",";
            first = false;
            ss << e;
        };
        emit(plan.giverEntry);
        emit(plan.turninEntry);
        // Summoned creatures have no world spawn — do not allow move_to them.
        for (uint32 e : plan.objectiveEntries)
        {
            if (std::find(plan.summonedEntries.begin(), plan.summonedEntries.end(), e)
                != plan.summonedEntries.end())
                continue;
            emit(e);
        }
    }
    ss << "]";

    ss << ",\"allowedOps\":[\"ensure_selfbot\",\"travel_to\",\"move_to\",\"interact\",\"gossip_select\","
          "\"accept_quest\",\"turnin_quest\",\"wait\",\"until\",\"use_item\",\"use_hearthstone\"]";
    if (plan.useItemId)
        ss << ",\"allowedItemIds\":[" << plan.useItemId << "]";

    if (player && player->IsInWorld())
    {
        QuestStatus const st = player->GetQuestStatus(questId);
        ss << ",\"character\":{"
           << "\"guid\":" << player->GetGUID().GetCounter()
           << ",\"name\":\"" << MyBotsJsonEscapeCopy(player->GetName()) << "\""
           << ",\"map\":" << player->GetMapId()
           << ",\"homebindMap\":" << player->m_homebindMapId
           << ",\"zone\":" << player->GetZoneId()
           << ",\"x\":" << player->GetPositionX()
           << ",\"y\":" << player->GetPositionY()
           << ",\"z\":" << player->GetPositionZ()
           << ",\"level\":" << uint32(player->GetLevel())
           << ",\"class\":" << uint32(player->getClass())
           << ",\"questStatus\":\"" << QuestStatusName(st) << "\""
           << "}";
    }

    ss << "}";
    return ss.str();
}

std::string MyBotsLlm::BuildReplanContext(Player* player, MyBotsJob const& job, std::string const& reason)
{
    uint32 questId = 0;
    MyBotsExecutor::ParseUInt(job.payload, "questId", questId);
    if (!questId)
        MyBotsExecutor::ParseUInt(job.payload, "quest_id", questId);

    std::string base = BuildPlanContext(player, questId, job.payload);
    if (base.size() < 2 || base.back() != '}')
        return base;

    base.pop_back(); // trailing }
    std::ostringstream ss;
    ss << base;
    ss << ",\"replan\":true";
    ss << ",\"failure\":\"" << MyBotsJsonEscapeCopy(reason) << "\"";
    ss << ",\"stepIndex\":" << job.stepIndex;
    ss << ",\"navAttempts\":" << job.navAttempts;
    ss << ",\"llmReplanCount\":" << job.llmReplanCount;

    if (job.stepIndex >= 0 && job.stepIndex < static_cast<int>(job.steps.size()))
    {
        auto const& st = job.steps[static_cast<size_t>(job.stepIndex)];
        ss << ",\"failedStep\":{\"op\":\"" << MyBotsJsonEscapeCopy(st.op)
           << "\",\"detail\":\"" << MyBotsJsonEscapeCopy(st.detail)
           << "\",\"result\":\"" << MyBotsJsonEscapeCopy(st.result) << "\"}";
    }

    ss << ",\"remainingSteps\":[";
    bool first = true;
    for (int i = std::max(0, job.stepIndex); i < static_cast<int>(job.steps.size()); ++i)
    {
        if (!first)
            ss << ",";
        first = false;
        auto const& st = job.steps[static_cast<size_t>(i)];
        ss << "{\"op\":\"" << MyBotsJsonEscapeCopy(st.op)
           << "\",\"detail\":\"" << MyBotsJsonEscapeCopy(st.detail) << "\"}";
    }
    ss << "]";

    // Expand allowlist with entries already referenced in remaining / failed steps.
    std::ostringstream extra;
    extra << ",\"extraEntries\":[";
    bool firstE = true;
    auto emitE = [&](uint32 e) {
        if (!e)
            return;
        if (!firstE)
            extra << ",";
        firstE = false;
        extra << e;
    };
    for (int i = std::max(0, job.stepIndex); i < static_cast<int>(job.steps.size()); ++i)
    {
        uint32 e = 0;
        MyBotsExecutor::ParseUInt(job.steps[static_cast<size_t>(i)].detail, "entry", e);
        emitE(e);
        // until.entries array — best-effort scan for numbers after "entries"
        std::string const& d = job.steps[static_cast<size_t>(i)].detail;
        auto pos = d.find("\"entries\"");
        if (pos != std::string::npos)
        {
            for (size_t k = pos; k < d.size(); ++k)
            {
                if (d[k] < '0' || d[k] > '9')
                    continue;
                uint32 v = 0;
                while (k < d.size() && d[k] >= '0' && d[k] <= '9')
                {
                    v = v * 10 + uint32(d[k] - '0');
                    ++k;
                }
                emitE(v);
            }
        }
    }
    extra << "]";

    ss << extra.str();
    ss << ",\"note\":\"Prefer alternate entries from allowedEntries/extraEntries; no raw coordinates.\"";
    ss << "}";
    return ss.str();
}

bool MyBotsLlm::TryRequestNavReplan(Player* player, MyBotsJob& job, std::string const& reason)
{
    if (!IsReady() || !sMyBotsConfig.LlmReplanOnStuck())
        return false;
    if (!player || job.status == MyBotsJobStatus::Planning)
        return false;
    if (job.llmReplanCount >= sMyBotsConfig.LlmReplanMax())
        return false;

    uint32 const now = MyBotsNow();
    if (job.lastLlmReplanAt && now - job.lastLlmReplanAt < sMyBotsConfig.LlmReplanCooldownSec())
        return false;

    uint32 questId = 0;
    MyBotsExecutor::ParseUInt(job.payload, "questId", questId);
    if (!questId)
        MyBotsExecutor::ParseUInt(job.payload, "quest_id", questId);

    ++job.llmReplanCount;
    job.lastLlmReplanAt = now;
    job.status = MyBotsJobStatus::Planning;
    job.error.clear();
    MyBotsExecutor::HaltControl(player, &job);

    std::string context = BuildReplanContext(player, job, reason);
    EnqueuePlan(job.id, job.charGuid, questId, std::move(context), true);
    sMyBotsJobStore.Save(job);
    sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "llm_replan_queued", reason);
    LOG_INFO("module.mybots", "MyBots LLM replan queued for job {} ({})", job.id, reason);
    return true;
}

void MyBotsLlm::EnqueuePlan(std::string jobId, uint32 charGuid, uint32 questId, std::string contextJson,
    bool replan)
{
    if (!IsReady())
        return;
    Start();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gQueue.push(PlanRequest{std::move(jobId), charGuid, questId, std::move(contextJson), replan});
    }
    gCv.notify_one();
}

MyBotsLlmPlanResult MyBotsLlm::ValidateAndParseSteps(uint32 questId, std::string const& contextJson,
    std::string const& modelContent)
{
    MyBotsLlmPlanResult result;
    result.rawContent = modelContent;
    std::string obj = ExtractStepsObject(modelContent);
    if (obj.empty())
    {
        result.error = "no_json_object";
        return result;
    }
    std::string arr = ExtractJsonArray(obj, "steps");
    if (arr.empty())
    {
        result.error = "no_steps_array";
        return result;
    }

    std::vector<MyBotsJobStep> steps;
    ParseStepsArray(arr, steps);
    if (steps.empty())
    {
        result.error = "empty_steps";
        return result;
    }
    if (steps.size() > sMyBotsConfig.LlmMaxSteps())
    {
        result.error = "too_many_steps";
        return result;
    }

    std::unordered_set<uint32> allowed;
    CollectAllowedEntries(contextJson, allowed);
    // Replan contexts may list extraEntries separately.
    {
        std::string arr = ExtractJsonArray(contextJson, "extraEntries");
        if (!arr.empty())
        {
            size_t i = 0;
            while (i < arr.size())
            {
                while (i < arr.size() && (arr[i] < '0' || arr[i] > '9'))
                    ++i;
                if (i >= arr.size())
                    break;
                uint32 v = 0;
                while (i < arr.size() && arr[i] >= '0' && arr[i] <= '9')
                {
                    v = v * 10 + uint32(arr[i] - '0');
                    ++i;
                }
                if (v)
                    allowed.insert(v);
            }
        }
    }

    if (steps.front().op != "ensure_selfbot")
    {
        bool const isReplan = contextJson.find("\"replan\":true") != std::string::npos
            || contextJson.find("\"replan\":1") != std::string::npos;
        if (!isReplan)
        {
            MyBotsJobStep ensure;
            ensure.op = "ensure_selfbot";
            ensure.detail = "{}";
            steps.insert(steps.begin(), ensure);
        }
    }
    else
    {
        bool const isReplan = contextJson.find("\"replan\":true") != std::string::npos
            || contextJson.find("\"replan\":1") != std::string::npos;
        // Remaining-work replans should not restart Selfbot setup.
        if (isReplan && steps.size() > 1)
            steps.erase(steps.begin());
    }

    for (auto& step : steps)
    {
        if (!IsAllowedOp(step.op))
        {
            result.error = "bad_op:" + step.op;
            return result;
        }
        if (step.op == "move_to")
        {
            uint32 entry = 0;
            MyBotsExecutor::ParseUInt(step.detail, "entry", entry);
            uint32 map = 0;
            bool const hasMap = MyBotsExecutor::ParseUInt(step.detail, "map", map);
            float tx = 0.f, ty = 0.f, tz = 0.f;
            bool const hasXYZ = MyBotsExecutor::ParseMoveXYZ(step.detail, tx, ty, tz);
            if (!entry && !(hasMap && hasXYZ))
            {
                // Prefer entry; allow map+xyz only when crossing / pinning a site
                // (same shape as rule-built summon-site steps).
                result.error = "move_to_requires_entry_or_map_xyz";
                return result;
            }
            if (entry && !allowed.empty() && !allowed.count(entry))
            {
                result.error = "entry_not_allowed:" + std::to_string(entry);
                return result;
            }
        }
        if (step.op == "travel_to")
        {
            // map 0 (Eastern Kingdoms) is valid — require the key, not a truthy id.
            uint32 map = 0;
            if (!MyBotsExecutor::ParseUInt(step.detail, "map", map))
            {
                result.error = "travel_to_requires_map";
                return result;
            }
            float tx = 0.f, ty = 0.f, tz = 0.f;
            bool const hasXYZ = MyBotsExecutor::ParseMoveXYZ(step.detail, tx, ty, tz);
            uint32 entry = 0;
            MyBotsExecutor::ParseUInt(step.detail, "entry", entry);
            if (!hasXYZ && !entry)
            {
                result.error = "travel_to_requires_xyz_or_entry";
                return result;
            }
            if (entry && !allowed.empty() && !allowed.count(entry))
            {
                result.error = "entry_not_allowed:" + std::to_string(entry);
                return result;
            }
        }
        if (step.op == "use_hearthstone" || step.op == "hearthstone")
        {
            // Optional map pin; no further validation required.
        }
        if (step.op == "interact" || step.op == "gossip_select")
        {
            uint32 entry = 0;
            MyBotsExecutor::ParseUInt(step.detail, "entry", entry);
            if (entry && !allowed.empty() && !allowed.count(entry))
            {
                result.error = "entry_not_allowed:" + std::to_string(entry);
                return result;
            }
        }
        if (step.op == "accept_quest" || step.op == "turnin_quest" || step.op == "turn_in_quest"
            || step.op == "until")
        {
            uint32 q = 0;
            MyBotsExecutor::ParseUInt(step.detail, "questId", q);
            if (!q)
                MyBotsExecutor::ParseUInt(step.detail, "quest_id", q);
            if (q && q != questId)
            {
                result.error = "quest_id_mismatch";
                return result;
            }
            if (!q && (step.op == "accept_quest" || step.op == "turnin_quest" || step.op == "turn_in_quest"
                    || step.op == "until"))
            {
                // Inject questId if the model omitted it.
                if (step.detail.empty() || step.detail == "{}")
                    step.detail = "{\"questId\":" + std::to_string(questId) + "}";
                else if (step.detail.back() == '}')
                {
                    step.detail.pop_back();
                    if (step.detail.size() > 1 && step.detail.back() != '{')
                        step.detail += ",";
                    step.detail += "\"questId\":" + std::to_string(questId) + "}";
                }
            }
        }
        if (step.op == "use_item")
        {
            uint32 itemId = 0;
            MyBotsExecutor::ParseUInt(step.detail, "itemId", itemId);
            if (!itemId)
                MyBotsExecutor::ParseUInt(step.detail, "item", itemId);
            if (!itemId)
            {
                result.error = "use_item_requires_itemId";
                return result;
            }
            std::string itemsArr = ExtractJsonArray(contextJson, "allowedItemIds");
            if (!itemsArr.empty())
            {
                std::unordered_set<uint32> items;
                CollectAllowedEntries("{\"allowedEntries\":" + itemsArr + "}", items);
                if (!items.empty() && !items.count(itemId))
                {
                    result.error = "item_not_allowed:" + std::to_string(itemId);
                    return result;
                }
            }
        }
    }

    result.ok = true;
    result.steps = std::move(steps);
    return result;
}

MyBotsLlmPlanResult MyBotsLlm::PlanSync(uint32 questId, std::string const& contextJson)
{
    MyBotsLlmPlanResult result;
    if (!IsReady())
    {
        result.error = "llm_disabled";
        return result;
    }

    bool const isReplan = contextJson.find("\"replan\":true") != std::string::npos
        || contextJson.find("\"replan\":1") != std::string::npos;
    char const* systemPrompt = isReplan ? kReplanSystemPrompt : kSystemPrompt;

    std::ostringstream body;
    body << "{"
         << "\"model\":\"" << MyBotsJsonEscapeCopy(sMyBotsConfig.LlmModel()) << "\","
         << "\"stream\":false,"
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\"" << MyBotsJsonEscapeCopy(systemPrompt) << "\"},"
         << "{\"role\":\"user\",\"content\":\"" << MyBotsJsonEscapeCopy(contextJson) << "\"}"
         << "]}";

    std::string response;
    std::string error;
    if (!HttpPostJson(sMyBotsConfig.LlmBaseUrl(), "/chat/completions", body.str(),
            sMyBotsConfig.LlmApiKey(), sMyBotsConfig.LlmTimeoutMs(), response, error))
    {
        result.error = error.empty() ? "http_failed" : error;
        return result;
    }

    std::string content = ExtractAssistantContent(response);
    if (content.empty())
    {
        result.error = "empty_content";
        result.rawContent = response.substr(0, std::min<size_t>(response.size(), 500));
        return result;
    }
    return ValidateAndParseSteps(questId, contextJson, content);
}
