#include "nmos/node.hpp"

#include "util/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(REPLAY_WITH_NMOS)
#include <limits>
#include "cpprest/host_utils.h"
#include "nmos/channels.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/rational.h"
#include "nmos/query_utils.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/slog.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "sdp/json.h"
#endif

namespace replay
{
struct NmosNode::Impl
{
    Config config;
    Engine& engine;
    NmosIds ids;
    std::atomic<bool> running{false};
    std::atomic<bool> deregistered{false};
    std::atomic<bool> finished{false};
#if defined(REPLAY_WITH_NMOS)
    std::thread thread;
    std::string error;
    std::mutex startMu;
    std::condition_variable startCv;
    bool startDone = false;
#endif
    Impl(Config c, Engine& e)
        : config(std::move(c))
        , engine(e)
        , ids(makeNmosIds(config.nmosSeed))
    {
    }
};

NmosNode::NmosNode(Config config, Engine& engine)
    : impl_(std::make_unique<Impl>(std::move(config), engine))
{
}

NmosNode::~NmosNode()
{
    stop();
}

bool NmosNode::running() const
{
    return impl_->running.load();
}

std::string NmosNode::summary() const
{
    return std::string("{\"node_id\":\"") + impl_->ids.node + "\",\"device_id\":\"" + impl_->ids.device + "\",\"device_label\":\"MXL Replay\",\"running\":" +
           (impl_->running.load() ? "true" : "false") + "}";
}

#if !defined(REPLAY_WITH_NMOS)
void NmosNode::start()
{
    if (impl_->config.nmosEnable)
    {
        logWarn("nmos_not_linked", {{"hint", "rebuild with REPLAY_WITH_NMOS"}});
    }
}
bool NmosNode::stop()
{
    return true;
}
#else
namespace
{
utility::string_t us(std::string const& text)
{
    return utility::conversions::to_string_t(text);
}
std::string su(utility::string_t const& text)
{
    return utility::conversions::to_utf8string(text);
}
// Labels the resource "<group> <role>" and adds the group hint. Without the label a
// sender or receiver showed the node label, and the platform connects by label.
void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
{
    resource.data[U("label")] = web::json::value::string(us(group + " " + role));
    if (!resource.data.has_field(nmos::fields::tags))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
}
void addTags(nmos::resource& resource, std::map<std::string, std::vector<std::string>> const& tags)
{
    if (tags.empty())
    {
        return;
    }
    if (!resource.data.has_field(nmos::fields::tags))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    for (auto const& [key, values] : tags)
    {
        if (!resource.data[U("tags")].has_field(us(key)))
        {
            resource.data[U("tags")][us(key)] = web::json::value::array();
        }
        for (auto const& value : values)
        {
            web::json::push_back(resource.data[U("tags")][us(key)], web::json::value::string(us(value)));
        }
    }
}
void restoreReceiver(nmos::resource& connection, Route const& saved)
{
    if (saved.senderId.empty() && saved.flowId.empty() && saved.domainId.empty() && !saved.active)
    {
        return;
    }
    connection.data[U("active")][U("master_enable")] = web::json::value::boolean(saved.active);
    connection.data[U("active")][U("sender_id")] = saved.senderId.empty() ? web::json::value::null() : web::json::value::string(us(saved.senderId));
    if (connection.data[U("active")].has_field(U("transport_params")) && connection.data[U("active")][U("transport_params")].is_array() &&
        connection.data[U("active")][U("transport_params")].size() > 0)
    {
        auto& params = connection.data[U("active")][U("transport_params")].at(0);
        if (!saved.domainId.empty())
        {
            params[U("mxl_domain_id")] = web::json::value::string(us(saved.domainId));
        }
        if (!saved.flowId.empty())
        {
            params[U("mxl_flow_id")] = web::json::value::string(us(saved.flowId));
        }
    }
}
} // namespace

void NmosNode::start()
{
    if (!impl_->config.nmosEnable)
    {
        return;
    }
    impl_->thread = std::thread([this] {
        try
        {
            nmos::experimental::log_model logModel;
            std::ostream errorLog(std::cerr.rdbuf());
            std::filebuf discarded;
            std::ostream accessLog(&discarded);
            nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
            nmos::node_model nodeModel;
            web::json::value settings = web::json::value::object();
            settings[U("http_port")] = impl_->config.nmosPort;
            settings[U("label")] = web::json::value::string(us(nodeLabel(impl_->config)));
            settings[U("description")] = web::json::value::string(U("mxl-replay"));
            settings[U("seed_id")] = web::json::value::string(us(impl_->ids.node));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-replay"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            settings[U("host_address")] = web::json::value::string(us(impl_->config.nmosHostAddress));
            web::json::value addresses = web::json::value::array();
            web::json::push_back(addresses, web::json::value::string(us(impl_->config.nmosHostAddress)));
            settings[U("host_addresses")] = addresses;
            settings[U("href_mode")] = 2;
            if (!impl_->config.nmosDnsSd)
            {
                settings[U("pri")] = std::numeric_limits<int>::max();
                settings[U("highest_pri")] = std::numeric_limits<int>::max();
                settings[U("authorization_highest_pri")] = std::numeric_limits<int>::max();
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                settings[U("registry_address")] = web::json::value::string(us(impl_->config.nmosRegistryAddress));
                settings[U("registration_port")] = impl_->config.nmosRegistryPort;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);
            auto implementation =
                nmos::experimental::node_implementation()
                    .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                 slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                    .on_resolve_auto([](nmos::resource const&, nmos::resource const&, web::json::value& params) {
                        if (!params.is_array() || params.size() == 0)
                        {
                            return;
                        }
                        nmos::details::resolve_auto(params.at(0), U("mxl_domain_id"), [] { return web::json::value::string(U("00000000-0000-0000-0000-000000000000")); });
                        nmos::details::resolve_auto(params.at(0), U("mxl_flow_id"), [] { return web::json::value::null(); });
                    })
                    .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) { transportFile = web::json::value::null(); })
                    .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                        auto const id = su(connection.id);
                        if (!connection.data.has_field(U("active")))
                        {
                            return;
                        }
                        auto const& active = connection.data.at(U("active"));
                        bool const enable = active.has_field(U("master_enable")) && active.at(U("master_enable")).as_bool();
                        std::string domain;
                        std::string flow;
                        std::string sender;
                        if (active.has_field(U("sender_id")) && active.at(U("sender_id")).is_string())
                        {
                            sender = su(active.at(U("sender_id")).as_string());
                        }
                        if (active.has_field(U("transport_params")) && active.at(U("transport_params")).is_array() && active.at(U("transport_params")).size() > 0)
                        {
                            auto const& params = active.at(U("transport_params")).at(0);
                            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
                            {
                                domain = su(params.at(U("mxl_domain_id")).as_string());
                            }
                            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
                            {
                                flow = su(params.at(U("mxl_flow_id")).as_string());
                            }
                        }
                        for (auto const& camera : impl_->config.cameras)
                        {
                            for (int phase = 1; phase <= camera.phases; ++phase)
                            {
                                if (id == impl_->ids.videoReceiver(camera.index, phase))
                                {
                                    impl_->engine.setRoute(camera.index, phase, true, Route{enable, domain, flow, sender, enable ? "running" : "waiting"}, true);
                                }
                            }
                            if (id == impl_->ids.audioReceiver(camera.index))
                            {
                                impl_->engine.setRoute(camera.index, 1, false, Route{enable, domain, flow, sender, enable ? "running" : "waiting"}, true);
                            }
                        }
                    });
            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel] {
                auto lock = nodeModel.write_lock();
                auto const clocks = web::json::value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
                auto const interfaces = nmos::experimental::node_interfaces(nmos::get_host_interfaces(nodeModel.settings));
                auto node = nmos::make_node(us(impl_->ids.node), clocks, nmos::make_node_interfaces(interfaces), nodeModel.settings);
                node.data[U("label")] = web::json::value::string(us(nodeLabel(impl_->config)));
                node.data[U("description")] = web::json::value::string(U("MXL Replay"));
                addTags(node, impl_->config.nmosTags);
                nmos::insert_resource(nodeModel.node_resources, std::move(node));
                std::vector<nmos::id> receivers;
                std::vector<nmos::id> senders;
                for (auto const& camera : impl_->config.cameras)
                {
                    for (int phase = 1; phase <= camera.phases; ++phase)
                    {
                        receivers.push_back(us(impl_->ids.videoReceiver(camera.index, phase)));
                    }
                    receivers.push_back(us(impl_->ids.audioReceiver(camera.index)));
                }
                for (auto const& channel : impl_->config.channelList)
                {
                    senders.push_back(us(impl_->ids.videoSender(channel.index)));
                    senders.push_back(us(impl_->ids.audioSender(channel.index)));
                    senders.push_back(us(impl_->ids.dataSender(channel.index)));
                }
                auto device = nmos::make_device(us(impl_->ids.device), us(impl_->ids.node), senders, receivers, nodeModel.settings);
                device.data[U("label")] = web::json::value::string(us(deviceLabel(impl_->config)));
                addTags(device, impl_->config.nmosTags);
                nmos::insert_resource(nodeModel.node_resources, std::move(device));
                nmos::rational const rate{impl_->config.format.rateNum, impl_->config.format.rateDen};
                for (auto const& camera : impl_->config.cameras)
                {
                    for (int phase = 1; phase <= camera.phases; ++phase)
                    {
                        auto const id = impl_->ids.videoReceiver(camera.index, phase);
                        auto receiver = nmos::make_receiver(us(id), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::video,
                            {nmos::media_types::video_v210}, nodeModel.settings);
                        auto const role = camera.phases > 1 ? "Video Phase " + std::to_string(phase) : std::string("Video");
                        receiver.data[U("label")] = web::json::value::string(us(camera.label + " " + role));
                        tagGroup(receiver, camera.label, role);
                        nmos::insert_resource(nodeModel.node_resources, std::move(receiver));
                        auto connection = nmos::make_connection_mxl_receiver(us(id), {});
                        connection.data[U("active")][U("master_enable")] = web::json::value::boolean(false);
                        restoreReceiver(connection, impl_->engine.route(camera.index, phase, true));
                        nmos::insert_resource(nodeModel.connection_resources, std::move(connection));
                    }
                    auto const audioId = impl_->ids.audioReceiver(camera.index);
                    auto audio = nmos::make_receiver(us(audioId), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::audio,
                        {nmos::media_types::audio_float32}, nodeModel.settings);
                    audio.data[U("label")] = web::json::value::string(us(camera.label + " Audio"));
                    tagGroup(audio, camera.label, "Audio");
                    nmos::insert_resource(nodeModel.node_resources, std::move(audio));
                    auto audioConnection = nmos::make_connection_mxl_receiver(us(audioId), {});
                    audioConnection.data[U("active")][U("master_enable")] = web::json::value::boolean(false);
                    restoreReceiver(audioConnection, impl_->engine.route(camera.index, 1, false));
                    nmos::insert_resource(nodeModel.connection_resources, std::move(audioConnection));
                }
                for (auto const& channel : impl_->config.channelList)
                {
                    auto source = nmos::make_video_source(us(impl_->ids.videoSource(channel.index)), us(impl_->ids.device), rate, nodeModel.settings);
                    tagGroup(source, channel.label, "Video");
                    nmos::insert_resource(nodeModel.node_resources, std::move(source));
                    auto flow = nmos::make_coded_video_flow(us(impl_->ids.videoFlow(channel.index, impl_->config.format.token())),
                        us(impl_->ids.videoSource(channel.index)), us(impl_->ids.device), rate, static_cast<unsigned>(impl_->config.format.width),
                        static_cast<unsigned>(impl_->config.format.height), nmos::interlace_modes::progressive, nmos::colorspaces::BT709,
                        nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10, nmos::media_types::video_v210, nodeModel.settings);
                    tagGroup(flow, channel.label, "Video");
                    nmos::insert_resource(nodeModel.node_resources, std::move(flow));
                    auto sender = nmos::make_sender(us(impl_->ids.videoSender(channel.index)), us(impl_->ids.videoFlow(channel.index, impl_->config.format.token())),
                        nmos::transports::mxl, us(impl_->ids.device), utility::string_t{}, std::vector<utility::string_t>{}, nodeModel.settings);
                    tagGroup(sender, channel.label, "Video");
                    nmos::insert_resource(nodeModel.node_resources, std::move(sender));
                    auto connection = nmos::make_connection_mxl_sender(us(impl_->ids.videoSender(channel.index)), us(impl_->config.outputDomainId),
                        us(impl_->ids.videoFlow(channel.index, impl_->config.format.token())));
                    connection.data[U("active")][U("master_enable")] = web::json::value::boolean(true);
                    nmos::insert_resource(nodeModel.connection_resources, std::move(connection));

                    std::vector<nmos::channel> audioChannels{{us("Left"), nmos::channel_symbols::L}, {us("Right"), nmos::channel_symbols::R}};
                    auto audioSource = nmos::make_audio_source(us(impl_->ids.audioSource(channel.index)), us(impl_->ids.device), nmos::rational{48000, 1},
                        audioChannels, nodeModel.settings);
                    tagGroup(audioSource, channel.label, "Audio");
                    nmos::insert_resource(nodeModel.node_resources, std::move(audioSource));
                    auto audioFlow = nmos::make_raw_audio_flow(us(impl_->ids.audioFlow(channel.index)), us(impl_->ids.audioSource(channel.index)),
                        us(impl_->ids.device), nmos::rational{48000, 1}, nmos::media_types::audio_float32, 32, nodeModel.settings);
                    audioFlow.data[U("channel_count")] = 2;
                    tagGroup(audioFlow, channel.label, "Audio");
                    nmos::insert_resource(nodeModel.node_resources, std::move(audioFlow));
                    auto audioSender = nmos::make_sender(us(impl_->ids.audioSender(channel.index)), us(impl_->ids.audioFlow(channel.index)), nmos::transports::mxl,
                        us(impl_->ids.device), utility::string_t{}, std::vector<utility::string_t>{}, nodeModel.settings);
                    tagGroup(audioSender, channel.label, "Audio");
                    nmos::insert_resource(nodeModel.node_resources, std::move(audioSender));
                    auto audioConnection = nmos::make_connection_mxl_sender(us(impl_->ids.audioSender(channel.index)), us(impl_->config.outputDomainId),
                        us(impl_->ids.audioFlow(channel.index)));
                    audioConnection.data[U("active")][U("master_enable")] = web::json::value::boolean(true);
                    nmos::insert_resource(nodeModel.connection_resources, std::move(audioConnection));

                    auto dataSource = nmos::make_data_source(us(impl_->ids.dataSource(channel.index)), us(impl_->ids.device), nmos::clock_names::clk0, rate,
                        nodeModel.settings);
                    tagGroup(dataSource, channel.label, "Data");
                    nmos::insert_resource(nodeModel.node_resources, std::move(dataSource));
                    auto dataFlow = nmos::make_sdianc_data_flow(us(impl_->ids.dataFlow(channel.index, impl_->config.format.token())),
                        us(impl_->ids.dataSource(channel.index)), us(impl_->ids.device), nodeModel.settings);
                    dataFlow.data[U("grain_rate")] = nmos::make_rational(rate);
                    dataFlow.data[U("media_type")] = web::json::value::string(nmos::media_types::video_smpte291.name);
                    tagGroup(dataFlow, channel.label, "Data");
                    nmos::insert_resource(nodeModel.node_resources, std::move(dataFlow));
                    auto dataSender = nmos::make_sender(us(impl_->ids.dataSender(channel.index)), us(impl_->ids.dataFlow(channel.index, impl_->config.format.token())),
                        nmos::transports::mxl, us(impl_->ids.device), utility::string_t{}, std::vector<utility::string_t>{}, nodeModel.settings);
                    tagGroup(dataSender, channel.label, "Data");
                    nmos::insert_resource(nodeModel.node_resources, std::move(dataSender));
                    auto dataConnection = nmos::make_connection_mxl_sender(us(impl_->ids.dataSender(channel.index)), us(impl_->config.outputDomainId),
                        us(impl_->ids.dataFlow(channel.index, impl_->config.format.token())));
                    dataConnection.data[U("active")][U("master_enable")] = web::json::value::boolean(true);
                    nmos::insert_resource(nodeModel.connection_resources, std::move(dataConnection));
                }
                nodeModel.notify();
                impl_->running.store(true);
                nodeModel.wait(lock, [&] { return nodeModel.shutdown || !impl_->running.load(); });
                if (!nodeModel.shutdown)
                {
                    auto rankOf = [](nmos::type const& type) {
                        if (type == nmos::types::receiver || type == nmos::types::sender)
                        {
                            return 0;
                        }
                        if (type == nmos::types::flow)
                        {
                            return 1;
                        }
                        if (type == nmos::types::source)
                        {
                            return 2;
                        }
                        if (type == nmos::types::device)
                        {
                            return 3;
                        }
                        if (type == nmos::types::node)
                        {
                            return 4;
                        }
                        return -1;
                    };
                    std::vector<std::pair<int, nmos::id>> ranked;
                    for (auto const& resource : nodeModel.node_resources)
                    {
                        int const rank = rankOf(resource.type);
                        if (rank >= 0 && resource.has_data())
                        {
                            ranked.push_back({rank, resource.id});
                        }
                    }
                    std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
                    for (auto const& item : ranked)
                    {
                        auto found = nodeModel.node_resources.find(item.second);
                        if (found == nodeModel.node_resources.end() || !found->has_data())
                        {
                            continue;
                        }
                        auto const pre = found->data;
                        auto const resourceUpdated = nmos::strictly_increasing_update(nodeModel.node_resources);
                        nodeModel.node_resources.modify(found, [&](nmos::resource& resource) {
                            resource.data = web::json::value::null();
                            resource.updated = resourceUpdated;
                        });
                        nmos::insert_resource_events(nodeModel.node_resources, found->version, found->downgrade_version, found->type, pre, found->data);
                    }
                    nodeModel.notify();
                    impl_->deregistered.store(true);
                    nodeModel.wait(lock, [&] { return nodeModel.shutdown; });
                }
            });
            nmos::server_guard guard(server);
            for (int i = 0; i < 200 && !impl_->running.load() && impl_->error.empty(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!impl_->running.load() && impl_->error.empty())
            {
                impl_->error = "NMOS node did not open NMOS_PORT " + std::to_string(impl_->config.nmosPort);
            }
            {
                std::lock_guard startLock{impl_->startMu};
                impl_->startDone = true;
            }
            impl_->startCv.notify_all();
            while (impl_->running.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.notify();
            }
            for (int i = 0; i < 100 && !impl_->deregistered.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
        }
        catch (std::exception const& ex)
        {
            impl_->error = ex.what();
            logError("nmos_node_failed", {{"error", ex.what()}});
            {
                std::lock_guard startLock{impl_->startMu};
                impl_->startDone = true;
            }
            impl_->startCv.notify_all();
        }
        impl_->finished.store(true);
    });
    std::unique_lock startLock{impl_->startMu};
    impl_->startCv.wait_for(startLock, std::chrono::seconds(15), [&] { return impl_->startDone; });
    if (!impl_->error.empty())
    {
        throw StartupError(75, impl_->error);
    }
    if (!impl_->running.load())
    {
        throw StartupError(75, "cannot bind NMOS_PORT " + std::to_string(impl_->config.nmosPort));
    }
}

bool NmosNode::stop()
{
    impl_->running.store(false);
    if (!impl_->thread.joinable())
    {
        return true;
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(impl_->config.shutdownTimeoutS);
    while (!impl_->finished.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (impl_->finished.load())
    {
        impl_->thread.join();
        return true;
    }
    impl_->thread.detach();
    logError("nmos_shutdown_timeout", {{"seconds", std::to_string(impl_->config.shutdownTimeoutS)}});
    return false;
}
#endif
} // namespace replay
