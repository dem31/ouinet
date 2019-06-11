
// Temporary, shall be removed once I'm done with this branch
#define SPEED_DEBUG 0

#include "dht.h"
#include "udp_multiplexer.h"
#include "code.h"
#include "collect.h"
#include "proximity_map.h"
#include "debug_ctx.h"
#include "is_martian.h"

#include "../async_sleep.h"
#include "../or_throw.h"
#include "../util/bytes.h"
#include "../util/condition_variable.h"
#include "../util/crypto.h"
#include "../util/success_condition.h"
#include "../util/wait_condition.h"
#include "../logger.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>
#if SPEED_DEBUG
#   include <boost/optional/optional_io.hpp>
#endif

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/rolling_mean.hpp>
#include <boost/accumulators/statistics/rolling_variance.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>

#include <chrono>
#include <random>
#include <set>

#include <iostream>

namespace ouinet {
namespace bittorrent {

using std::move;
using std::make_unique;
using std::vector;
using std::string;
using boost::string_view;
using std::cerr; using std::endl;
using dht::NodeContact;
using Candidates = std::vector<NodeContact>;
namespace accum = boost::accumulators;
using Clock = std::chrono::steady_clock;
using std::make_shared;

#define DEBUG_SHOW_MESSAGES 0

class Stat {
private:
    using AccumSet = accum::accumulator_set< float
                                           , accum::stats< accum::tag::rolling_mean
                                                         , accum::tag::rolling_variance
                                                         , accum::tag::rolling_count>>;

public:
    using Duration = std::chrono::steady_clock::duration;

public:
    Stat()
        : _accum_set(accum::tag::rolling_window::window_size = 10)
    {}

    void add_reply_time(Duration d)
    {
        using namespace std::chrono;
        float seconds = duration_cast<milliseconds>(d).count() / 1000.f;
        _accum_set(seconds);
    }

    Duration max_reply_wait_time() const {
        //// 2 Should cover ~97.6% of all responses
        //// 3 Should cover ~99.9% of all responses
        auto ov = mean_plus_deviation(3);
        if (!ov) return default_max_reply_wait_time();

        return std::min(*ov, default_max_reply_wait_time());
        //return std::min(3**ov/2, default_max_reply_wait_time());
    }

    static
    Duration default_max_reply_wait_time() {
        return std::chrono::seconds(3);
    }

    static
    Duration seconds_to_duration(float secs) {
        using namespace std::chrono;
        return milliseconds(int64_t(secs*1000.f));
    }

private:
    boost::optional<Duration> mean_plus_deviation(float deviation_multiply) const {
        auto count = accum::rolling_count(_accum_set);

        if (count < 5) return boost::none;

        auto mean     = accum::rolling_mean(_accum_set);
        auto variance = accum::rolling_variance(_accum_set);

        if (variance < 0) return boost::none;

        auto deviation = sqrt(variance);

        return seconds_to_duration(mean + deviation_multiply*deviation);
    }

private:
    AccumSet _accum_set;
};

class dht::DhtNode::Stats {
public:
    using Duration = Stat::Duration;

public:

    void add_reply_time(boost::string_view msg_type, Duration d)
    {
        find_or_create(msg_type).add_reply_time(d);
    }

    Duration max_reply_wait_time(const std::string& msg_type)
    {
        return find_or_create(msg_type).max_reply_wait_time();
    }

private:
    Stat& find_or_create(boost::string_view msg_type) {
        auto i = _per_msg_stat.find(msg_type);
        if (i == _per_msg_stat.end()) {
            auto p = _per_msg_stat.insert(std::make_pair( msg_type.to_string()
                                                        , Stat()));
            return p.first->second;
        }
        return i->second;
    }

private:
    std::map<std::string, Stat, std::less<>> _per_msg_stat;
};

static bool read_nodes( bool is_v4
                      , const BencodedMap& response
                      , util::AsyncQueue<NodeContact>& sink
                      , Cancel& cancel
                      , asio::yield_context yield)
{
    std::vector<NodeContact> nodes;

    if (is_v4) {
        auto i = response.find("nodes");
        if (i != response.end()) {
            boost::optional<std::string> os = i->second.as_string();
            if (os) NodeContact::decode_compact_v4(*os, nodes);
        }
    } else {
        auto i = response.find("nodes6");
        if (i != response.end()) {
            boost::optional<std::string> os = i->second.as_string();
            if (os) NodeContact::decode_compact_v6(*os, nodes);
        }
    }

    // Remove invalid endpoints
    nodes.erase( std::remove_if
                  ( nodes.begin()
                  , nodes.end()
                  , [] (auto& n) { return is_martian(n.endpoint); })
               , nodes.end());

    if (nodes.empty()) return false;

    sys::error_code ec;
    sink.async_push_many(nodes, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, false);

    return true;
}

std::string dht::NodeContact::to_string() const
{
    return id.to_hex() + " at " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}


dht::DhtNode::DhtNode(asio::io_service& ios):
    _ios(ios),
    _ready(false),
    _stats(new Stats())
{
}

void dht::DhtNode::start(udp::endpoint local_ep, asio::yield_context yield)
{
    if (local_ep.address().is_loopback()) {
        LOG_WARN( "BT DhtNode shall be bound to the loopback address and "
                , "thus won't be able to communicate with the world");
    }

    sys::error_code ec;
    auto m = asio_utp::udp_multiplexer(_ios);
    m.bind(local_ep, ec);
    if (ec) return or_throw(yield, ec);
    return start(move(m), yield);
}

void dht::DhtNode::start(asio_utp::udp_multiplexer m, asio::yield_context yield)
{
    _multiplexer = std::make_unique<UdpMultiplexer>(move(m));

    _tracker = std::make_unique<Tracker>(_ios);
    _data_store = std::make_unique<DataStore>(_ios);

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        receive_loop(yield);
    });

    sys::error_code ec;
    bootstrap(yield[ec]);

    return or_throw(yield, ec);
}

void dht::DhtNode::stop()
{
    _multiplexer = nullptr;
    _tracker = nullptr;
    _data_store = nullptr;
    _cancel();
}

dht::DhtNode::~DhtNode()
{
    stop();
}

std::set<tcp::endpoint> dht::DhtNode::tracker_get_peers(
    NodeID infohash,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;
    std::set<tcp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;
    tracker_do_search_peers(infohash, peers, responsible_nodes, cancel, yield[ec]);
    return or_throw(yield, ec, std::move(peers));
}

std::set<tcp::endpoint> dht::DhtNode::tracker_announce(
    NodeID infohash,
    boost::optional<int> port,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;
    std::set<tcp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;
    tracker_do_search_peers(infohash, peers, responsible_nodes, cancel, yield[ec]);
    if (ec) {
        return or_throw<std::set<tcp::endpoint>>(yield, ec, std::move(peers));
    }

    bool success = false;
    auto cancelled = cancel.connect([]{});
    WaitCondition wc(_ios);
    for (auto& i : responsible_nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
            sys::error_code ec;
            send_write_query(
                i.second.node_endpoint,
                i.first,
                "announce_peer",
                BencodedMap {
                    { "id", _node_id.to_bytestring() },
                    { "info_hash", infohash.to_bytestring() },
                    { "token", i.second.announce_token },
                    { "implied_port", port ? int64_t(0) : int64_t(1) },
                    { "port", port ? int64_t(*port) : int64_t(0) }
                },
                cancel,
                yield[ec]
            );
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = cancelled ? boost::asio::error::operation_aborted : success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<std::set<tcp::endpoint>>(yield, ec, std::move(peers));
}

boost::optional<BencodedValue> dht::DhtNode::data_get_immutable(
    const NodeID& key,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);
    boost::optional<BencodedValue> data;

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    collect(dbg, key, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Cancel& cancel,
        asio::yield_context yield
    ) {
        if (!candidate.id && responsible_nodes.full()) {
            return;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return;
        }

        /*
         * As soon as we have found a valid data value, we can stop the search.
         */
        if (data) {
            return;
        }

        boost::optional<BencodedMap> response_ = query_get_data(
            key,
            candidate,
            closer_nodes,
            wd, &dbg,
            cancel,
            yield
        );

        if (!response_) return;

        BencodedMap& response = *response_;

        if (candidate.id) {
            responsible_nodes.insert({ *candidate.id, boost::none });
        }

        if (response.count("v")) {
            BencodedValue value = response["v"];
            if (DataStore::immutable_get_id(value) == key) {
                data = value;
                return;
            }
        }
    }, cancel, yield[ec]);

    return or_throw<boost::optional<BencodedValue>>(yield, ec, std::move(data));
}

NodeID dht::DhtNode::data_put_immutable(
    const BencodedValue& data,
    Cancel& cancel,
    asio::yield_context yield
) {
    NodeID key = DataStore::immutable_get_id(data);

    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    collect(dbg, key, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Signal<void()>& cancel,
        asio::yield_context yield
    ) {
        if (!candidate.id && responsible_nodes.full()) {
            return;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return;
        }

        boost::optional<BencodedMap> response_ = query_get_data(
            key,
            candidate,
            closer_nodes,
            wd, &dbg,
            cancel,
            yield
        );

        if (!response_) return;

        BencodedMap& response = *response_;

        boost::optional<std::string> put_token = response["token"].as_string();

        if (!put_token) return;

        if (candidate.id) {
            responsible_nodes.insert({
                *candidate.id,
                { candidate.endpoint, *put_token }
            });
        }
    }, cancel, yield[ec]);

    if (ec) {
        return or_throw<NodeID>(yield, ec, std::move(key));
    }

    bool success = false;
    auto cancelled = cancel.connect([]{});
    WaitCondition wc(_ios);
    for (auto& i : responsible_nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
            sys::error_code ec;
            send_write_query(
                i.second.node_endpoint,
                i.first,
                "put",
                {
                    { "id", _node_id.to_bytestring() },
                    { "v", data },
                    { "token", i.second.put_token }
                },
                cancel,
                yield[ec]
            );
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = cancelled ? boost::asio::error::operation_aborted : success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<NodeID>(yield, ec, std::move(key));
}

boost::optional<MutableDataItem> dht::DhtNode::data_get_mutable(
    const util::Ed25519PublicKey& public_key,
    boost::string_view salt,
    Cancel& cancel,
    asio::yield_context yield
) {
    NodeID target_id = DataStore::mutable_get_id(public_key, salt);

    sys::error_code ec;
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);
    boost::optional<MutableDataItem> data;

    Cancel internal_cancel(cancel);

    boost::optional<WatchDog> cancel_wd;

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    collect(dbg, target_id, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Cancel& cancel,
        asio::yield_context yield
    ) {
        if (!candidate.id && responsible_nodes.full()) {
            return;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return;
        }

        /*
         * We want to find the latest version of the data, so don't stop early.
         */

        assert(!cancel);

        boost::optional<BencodedMap> response_ = query_get_data2(
            target_id,
            candidate,
            closer_nodes,
            wd,
            dbg,
            cancel,
            yield
        );

        if (cancel || !response_) return;

        BencodedMap& response = *response_;

        if (candidate.id) {
            responsible_nodes.insert({ *candidate.id, boost::none });
        }

        if (response["k"] != util::bytes::to_string(public_key.serialize())) {
            return;
        }

        boost::optional<int64_t> sequence_number = response["seq"].as_int();
        if (!sequence_number) return;

        boost::optional<std::string> signature = response["sig"].as_string();
        if (!signature || signature->size() != 64) return;

        MutableDataItem item {
            public_key,
            salt.to_string(),
            response["v"],
            *sequence_number,
            util::bytes::to_array<uint8_t, 64>(*signature)
        };
        if (item.verify()) {
            if (!data || *sequence_number > data->sequence_number) {
                data = std::move(item);
                /*
                 * XXX: This isn't correct! We shouldn't stop with the first
                 * validly signed item we get. Ideally we would get the item
                 * from some N closest nodes to `target_id`. But that is
                 * impractical because many of the closest nodes won't respond
                 * and make us wait for too long (and we sometimes time-out even
                 * though there _is_ some value already).
                 *
                 * TODO: Make this function not return a single value, but a
                 * "generator" of dht mutable items. Then the user of this
                 * function can have a look at it and decide whether it's
                 * "fresh enough" (e.g. if it's a http response, it may still
                 * be fresh).
                 */
                if (!cancel_wd) {
                    cancel_wd = WatchDog(_ios
                                        , std::chrono::seconds(5)
                                        , [&] { cancel(); });
                }
            }
        }
    }, internal_cancel, yield[ec]);

    if (ec == asio::error::operation_aborted && !cancel && data) {
        // Only internal cancel was called to indicate we're done
        ec = sys::error_code();
    }

    return or_throw(yield, ec, std::move(data));
}

NodeID dht::DhtNode::data_put_mutable(
    MutableDataItem data,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    Cancel local_cancel(cancel_signal);

    NodeID target_id = DataStore::mutable_get_id(data.public_key, data.salt);

    sys::error_code ec;
    ProximityMap<boost::none_t> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    using namespace std::chrono;

    DebugCtx dbg;

    auto write_to_node = [&] ( const NodeID& id
                             , udp::endpoint ep
                             , const std::string& put_token
                             , WatchDog& wd
                             , Cancel& cancel
                             , asio::yield_context yield) -> bool {
            BencodedMap put_message {
                { "id", _node_id.to_bytestring() },
                { "k", util::bytes::to_string(data.public_key.serialize()) },
                { "seq", data.sequence_number },
                { "sig", util::bytes::to_string(data.signature) },
                { "v", data.value },
                { "token", put_token }
            };

            if (!data.salt.empty()) {
                put_message["salt"] = data.salt;
            }

            wd.expires_after(_stats->max_reply_wait_time("put"));

            sys::error_code ec;
            send_write_query(ep, id, "put", put_message, cancel, yield[ec]);

            if (cancel) ec = asio::error::operation_aborted;

            return !ec ? true : false;
    };

    std::set<udp::endpoint> blacklist;

    collect(dbg, target_id, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Cancel& cancel,
        asio::yield_context yield
    ) {
        if (!candidate.id && responsible_nodes.full()) {
            return;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return;
        }

        if (blacklist.count(candidate.endpoint)) {
            return;
        }

        boost::optional<BencodedMap> response_ = query_get_data3(
            target_id,
            candidate,
            closer_nodes,
            wd,
            dbg,
            cancel,
            yield
        );

        if (cancel) return;

        if (!response_) {
            blacklist.insert(candidate.endpoint);
            return;
        }

        BencodedMap& response = *response_;

        boost::optional<std::string> put_token = response["token"].as_string();
        if (!put_token) {
            return;
        }

        if (candidate.id) {
            if (responsible_nodes.would_insert(*candidate.id)) {
                auto write_success = write_to_node( *candidate.id
                                                  , candidate.endpoint
                                                  , *put_token
                                                  , wd
                                                  , cancel
                                                  , yield);

                if (write_success) {
                    responsible_nodes.insert({*candidate.id, boost::none});
                    return;
                }
            }
        }

        if (cancel) return;

        if (response["k"] != util::bytes::to_string(data.public_key.serialize())) {
            return;
        }

        boost::optional<int64_t> response_seq = response["seq"].as_int();
        if (!response_seq) return;

        boost::optional<std::string> response_sig = response["sig"].as_string();
        if (!response_sig || response_sig->size() != 64) return;

        MutableDataItem item {
            data.public_key,
            data.salt,
            response["v"],
            *response_seq,
            util::bytes::to_array<uint8_t, 64>(*response_sig)
        };

        if (item.verify()) {
            if (*response_seq < data.sequence_number) {
                /*
                 * This node has an old version of this data entry.
                 * Update it even if it is no longer responsible.
                 */
                write_to_node( *candidate.id
                             , candidate.endpoint
                             , *put_token
                             , wd
                             , cancel
                             , yield);
            }
        }
    }, local_cancel, yield[ec]);

    if (cancel_signal) {
        ec = asio::error::operation_aborted;
    } else if (responsible_nodes.empty()) {
        ec = asio::error::network_down;
    }

    return or_throw<NodeID>(yield, ec, std::move(target_id));
}


void dht::DhtNode::receive_loop(asio::yield_context yield)
{
    while (true) {
        sys::error_code ec;

        /*
         * Later versions of boost::asio make it possible to (1) wait for a
         * datagram, (2) find out the size, (3) allocate a buffer, (4) recv
         * the datagram. Unfortunately, boost::asio 1.62 does not support that.
         */
        udp::endpoint sender;

        const boost::string_view packet
            = _multiplexer->receive(sender, _cancel, yield[ec]);

        if (ec) {
            break;
        }

        // TODO: The bencode parser should only need a string_view.
        boost::optional<BencodedValue> decoded_message = bencoding_decode(packet);

        if (!decoded_message) {
#           if DEBUG_SHOW_MESSAGES
            std::cerr << "recv: " << sender
                      << " Failed parsing \"" << packet << "\"" << std::endl;
#           endif

            continue;
        }

#       if DEBUG_SHOW_MESSAGES
        std::cerr << "recv: " << sender << " " << *decoded_message << std::endl;
#       endif

        boost::optional<BencodedMap> message_map = decoded_message->as_map();
        if (!message_map) {
            continue;
        }

        if (!message_map->count("y") || !message_map->count("t")) {
            continue;
        }

        boost::optional<std::string> message_type = (*message_map)["y"].as_string();
        boost::optional<std::string> transaction_id = (*message_map)["t"].as_string();
        if (!message_type || !transaction_id) {
            continue;
        }

        if (*message_type == "q") {
            handle_query(sender, *message_map);
        } else if (*message_type == "r" || *message_type == "e") {
            auto it = _active_requests.find(*transaction_id);
            if (it != _active_requests.end() && it->second.destination == sender) {
                it->second.callback(*message_map);
            }
        }
    }
}

std::string dht::DhtNode::new_transaction_string()
{
#if 0 // Useful for debugging
    std::stringstream ss;
    ss << _next_transaction_id++;
    return ss.str();
#else
    uint32_t transaction_id = _next_transaction_id++;

    if (transaction_id == 0) {
        return std::string(1 /* count */, '\0');
    }

    std::string ret;

    while (transaction_id) {
        unsigned char c = transaction_id & 0xff;
        transaction_id = transaction_id >> 8;
        ret += c;
    }

    return ret;
#endif
}

void dht::DhtNode::send_datagram(
    udp::endpoint destination,
    const BencodedMap& message
) {
#   if DEBUG_SHOW_MESSAGES
    std::cerr << "send: " << destination << " " << message << " :: " << i->second << std::endl;
#   endif
    _multiplexer->send(bencoding_encode(message), destination);
}

void dht::DhtNode::send_datagram(
    udp::endpoint destination,
    const BencodedMap& message,
    Cancel& cancel,
    asio::yield_context yield
) {
#   if DEBUG_SHOW_MESSAGES
    std::cerr << "send: " << destination << " " << message << " :: " << i->second << std::endl;
#   endif
    _multiplexer->send(bencoding_encode(message), destination, cancel, yield);
}

void dht::DhtNode::send_query(
    udp::endpoint destination,
    std::string transaction,
    std::string query_type,
    BencodedMap query_arguments,
    Cancel& cancel,
    asio::yield_context yield
) {
    send_datagram(
        destination,
        BencodedMap {
            { "y", "q" },
            { "q", std::move(query_type) },
            { "a", std::move(query_arguments) },
            // TODO: version string
            { "t", std::move(transaction) }
        },
        cancel,
        yield
    );
}

/*
 * Send a query message to a destination, and wait for either a reply, an error
 * reply, or a timeout.
 *
 * If destination_id is set, update the routing table in accordance with
 * whether a successful reply was received.
 */
BencodedMap dht::DhtNode::send_query_await_reply(
    Contact dst,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    WatchDog* dms,
    DebugCtx* dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    using namespace std::chrono;

    assert(!cancel_signal);

    sys::error_code ec;

    //auto timeout = _stats->max_reply_wait_time(query_type);
    auto timeout = std::chrono::seconds(10);

    if (dms) {
        auto d1 = dms->time_to_finish();
        auto d2 = _stats->max_reply_wait_time(query_type);

        dms->expires_after(std::max(d1,d2));
    }

    auto start = Clock::now();

    BencodedMap response; // Return value

    ConditionVariable reply_and_timeout_condition(_ios);
    boost::optional<sys::error_code> first_error_code;

    asio::steady_timer timeout_timer(_ios);
    timeout_timer.expires_from_now(timeout);

    bool timer_handler_executed = false;

    timeout_timer.async_wait([&] (const sys::error_code&) {
        timer_handler_executed = true;
        if (!first_error_code) {
            first_error_code = asio::error::timed_out;
        }
        reply_and_timeout_condition.notify();
    });

    auto cancelled = cancel_signal.connect([&] {
        first_error_code = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    auto terminated = _cancel.connect([&] {
        first_error_code = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    std::string transaction = new_transaction_string();

    _active_requests[transaction] = {
        dst.endpoint,
        [&] (const BencodedMap& response_) {
            /*
             * This function is never called when the Dht object is
             * destructed, thus the terminate_slot.
             */
            if (first_error_code) {
                return;
            }
            first_error_code = sys::error_code(); // success;
            response = response_;
            timeout_timer.cancel();
        }
    };

    send_query(
        dst.endpoint,
        transaction,
        std::move(query_type),
        std::move(query_arguments),
        cancel_signal,
        yield[ec]
    );

    if (ec) {
        first_error_code = ec;
        timeout_timer.cancel();
    }

    if (!timer_handler_executed) {
        reply_and_timeout_condition.wait(yield);
    }

    if (terminated) {
        return or_throw<BencodedMap>(yield, asio::error::operation_aborted);
    }

    /*
     * We do this cleanup when cancelling the operation, but NOT when
     * the Dht object has been destroyed.
     */
    _active_requests.erase(transaction);

    assert(first_error_code);

    if (cancelled || *first_error_code == asio::error::operation_aborted) {
        return or_throw<BencodedMap>(yield, asio::error::operation_aborted);
    }

    if (!*first_error_code) {
        _stats->add_reply_time(query_type, Clock::now() - start);
    }

    if (dst.id) {
        NodeContact contact{ .id = *dst.id, .endpoint = dst.endpoint };

        if (*first_error_code || response["y"] != "r") {
            /*
             * Record the failure in the routing table.
             */
            _routing_table->fail_node(contact);
        } else {
            /*
             * Add the node to the routing table, subject to space limitations.
             */
            _routing_table->try_add_node(contact, true);
        }
    }

    return or_throw<BencodedMap>(yield, *first_error_code, std::move(response));
}

void dht::DhtNode::handle_query(udp::endpoint sender, BencodedMap query)
{
    assert(query["y"] == "q");

    boost::optional<std::string> transaction_ = query["t"].as_string();

    if (!transaction_) { return; }

    std::string transaction = *transaction_;

    auto send_error = [&] (int code, std::string description) {
        send_datagram(
            sender,
            BencodedMap {
                { "y", "e" },
                { "t", transaction },
                { "e", BencodedList{code, description} }
            }
        );
    };

    auto send_reply = [&] (BencodedMap reply) {
        reply["id"] = _node_id.to_bytestring();

        send_datagram(
            sender,
            BencodedMap {
                { "y", "r" },
                { "t", transaction },
                { "e", std::move(reply) }
            }
        );
    };

    if (!query["q"].is_string()) {
        return send_error(203, "Missing field 'q'");
    }
    std::string query_type = *query["q"].as_string();

    if (!query["a"].is_map()) {
        return send_error(203, "Missing field 'a'");
    }
    BencodedMap arguments = *query["a"].as_map();

    boost::optional<std::string> sender_id = arguments["id"].as_string();
    if (!sender_id) {
        return send_error(203, "Missing argument 'id'");
    }
    if (sender_id->size() != 20) {
        return send_error(203, "Malformed argument 'id'");
    }
    NodeContact contact;
    contact.id = NodeID::from_bytestring(*sender_id);
    contact.endpoint = sender;

    /*
     * Per BEP 43, if the query contains a read-only flag, do not consider the
     * sender for any routing purposes.
     */
    boost::optional<int64_t> read_only_flag = arguments["ro"].as_int();
    if (_routing_table && (!read_only_flag || *read_only_flag != 1)) {
        /*
        * Add the sender to the routing table.
        */
        _routing_table->try_add_node(contact, false);
    }

    if (query_type == "ping") {
        return send_reply({});
    } else if (query_type == "find_node") {
        boost::optional<std::string> target_id_ = arguments["target"].as_string();
        if (!target_id_) {
            return send_error(203, "Missing argument 'target'");
        }
        if (target_id_->size() != 20) {
            return send_error(203, "Malformed argument 'target'");
        }
        NodeID target_id = NodeID::from_bytestring(*target_id_);

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts;

        if (_routing_table) {
            contacts = _routing_table->find_closest_routing_nodes(target_id, RoutingTable::BUCKET_SIZE);
        }

        std::string nodes;
        if (!contacts.empty() && contacts[0].id == target_id) {
            nodes += contacts[0].id.to_bytestring();
            nodes += encode_endpoint(contacts[0].endpoint);
        } else {
            for (auto& contact : contacts) {
                nodes += contact.id.to_bytestring();
                nodes += encode_endpoint(contact.endpoint);
            }
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        return send_reply(reply);
    } else if (query_type == "get_peers") {
        boost::optional<std::string> infohash_ = arguments["info_hash"].as_string();
        if (!infohash_) {
            return send_error(203, "Missing argument 'info_hash'");
        }
        if (infohash_->size() != 20) {
            return send_error(203, "Malformed argument 'info_hash'");
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts;

        if (_routing_table) {
            contacts = _routing_table->find_closest_routing_nodes(infohash, RoutingTable::BUCKET_SIZE);
        }

        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _tracker->generate_token(sender.address(), infohash);

        /*
         * 50 peers will comfortably fit in a single UDP packet even in the
         * worst case.
         */
        const int NUM_PEERS = 50;
        std::vector<tcp::endpoint> peers = _tracker->list_peers(infohash, NUM_PEERS);
        if (!peers.empty()) {
            BencodedList peer_list;
            for (auto& peer : peers) {
                peer_list.push_back(encode_endpoint(peer));
            }
            reply["values"] = peer_list;
        }

        return send_reply(reply);
    } else if (query_type == "announce_peer") {
        boost::optional<std::string> infohash_ = arguments["info_hash"].as_string();
        if (!infohash_) {
            return send_error(203, "Missing argument 'info_hash'");
        }
        if (infohash_->size() != 20) {
            return send_error(203, "Malformed argument 'info_hash'");
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        boost::optional<std::string> token_ = arguments["token"].as_string();
        if (!token_) {
            return send_error(203, "Missing argument 'token'");
        }
        std::string token = *token_;
        boost::optional<int64_t> port_ = arguments["port"].as_int();
        if (!port_) {
            return send_error(203, "Missing argument 'port'");
        }
        boost::optional<int64_t> implied_port_ = arguments["implied_port"].as_int();
        int effective_port;
        if (implied_port_ && *implied_port_ == 1) {
            effective_port = sender.port();
        } else {
            effective_port = *port_;
        }

        /*
         * Reject announce_peer requests for which there are more than enough
         * better responsible known nodes.
         *
         * TODO: This can be done in a more efficient way once the routing
         * table code stabilizes.
         */
        if (_routing_table) {
            bool contains_self = false;
            std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(infohash, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
            for (auto& i : closer_nodes) {
                if (infohash.closer_to(_node_id, i.id)) {
                    contains_self = true;
                }
            }
            if (!contains_self) {
                return send_error(201, "This torrent is not my responsibility");
            }
        }

        if (!_tracker->verify_token(sender.address(), infohash, token)) {
            return send_error(203, "Incorrect announce token");
        }

        _tracker->add_peer(infohash, tcp::endpoint(sender.address(), effective_port));

        return send_reply({});
    } else if (query_type == "get") {
        boost::optional<std::string> target_ = arguments["target"].as_string();
        if (!target_) {
            return send_error(203, "Missing argument 'target'");
        }
        if (target_->size() != 20) {
            return send_error(203, "Malformed argument 'target'");
        }
        NodeID target = NodeID::from_bytestring(*target_);

        boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts
            = _routing_table->find_closest_routing_nodes(target, RoutingTable::BUCKET_SIZE);
        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _data_store->generate_token(sender.address(), target);

        if (!sequence_number_) {
            boost::optional<BencodedValue> immutable_value = _data_store->get_immutable(target);
            if (immutable_value) {
                reply["v"] = *immutable_value;
                return send_reply(reply);
            }
        }

        boost::optional<MutableDataItem> mutable_item = _data_store->get_mutable(target);
        if (mutable_item) {
            if (sequence_number_ && *sequence_number_ <= mutable_item->sequence_number) {
                return send_reply(reply);
            }

            reply["k"] = util::bytes::to_string(mutable_item->public_key.serialize());
            reply["seq"] = mutable_item->sequence_number;
            reply["sig"] = util::bytes::to_string(mutable_item->signature);
            reply["v"] = mutable_item->value;
            return send_reply(reply);
        }

        return send_reply(reply);
    } else if (query_type == "put") {
        boost::optional<std::string> token_ = arguments["token"].as_string();
        if (!token_) {
            return send_error(203, "Missing argument 'token'");
        }

        if (!arguments.count("v")) {
            return send_error(203, "Missing argument 'v'");
        }
        BencodedValue value = arguments["v"];
        /*
         * Size limit specified in BEP 44
         */
        if (bencoding_encode(value).size() >= 1000) {
            return send_error(205, "Argument 'v' too big");
        }

        if (arguments["k"].is_string()) {
            /*
             * This is a mutable data item.
             */
            boost::optional<std::string> public_key_ = arguments["k"].as_string();
            if (!public_key_) {
                return send_error(203, "Missing argument 'k'");
            }
            if (public_key_->size() != 32) {
                return send_error(203, "Malformed argument 'k'");
            }
            util::Ed25519PublicKey public_key(util::bytes::to_array<uint8_t, 32>(*public_key_));

            boost::optional<std::string> signature_ = arguments["sig"].as_string();
            if (!signature_) {
                return send_error(203, "Missing argument 'sig'");
            }
            if (signature_->size() != 64) {
                return send_error(203, "Malformed argument 'sig'");
            }
            std::array<uint8_t, 64> signature = util::bytes::to_array<uint8_t, 64>(*signature_);

            boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();
            if (!sequence_number_) {
                return send_error(203, "Missing argument 'seq'");
            }
            int64_t sequence_number = *sequence_number_;

            boost::optional<std::string> salt_ = arguments["salt"].as_string();
            /*
             * Size limit specified in BEP 44
             */
            if (salt_ && salt_->size() > 64) {
                return send_error(207, "Argument 'salt' too big");
            }
            std::string salt = salt_ ? *salt_ : "";

            NodeID target = _data_store->mutable_get_id(public_key, salt);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                return send_error(203, "Incorrect put token");
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    return send_error(201, "This data item is not my responsibility");
                }
            }

            MutableDataItem item {
                public_key,
                salt,
                value,
                sequence_number,
                signature
            };
            if (!item.verify()) {
                return send_error(206, "Invalid signature");
            }

            boost::optional<MutableDataItem> existing_item = _data_store->get_mutable(target);
            if (existing_item) {
                if (sequence_number < existing_item->sequence_number) {
                    return send_error(302, "Sequence number less than current");
                }

                if (
                       sequence_number == existing_item->sequence_number
                    && bencoding_encode(value) != bencoding_encode(existing_item->value)
                ) {
                    return send_error(302, "Sequence number not updated");
                }

                boost::optional<int64_t> compare_and_swap_ = arguments["cas"].as_int();
                if (compare_and_swap_ && *compare_and_swap_ != existing_item->sequence_number) {
                    return send_error(301, "Compare-and-swap mismatch");
                }
            }

            _data_store->put_mutable(item);

            return send_reply({});
        } else {
            /*
             * This is an immutable data item.
             */
            NodeID target = _data_store->immutable_get_id(value);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                return send_error(203, "Incorrect put token");
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    return send_error(201, "This data item is not my responsibility");
                }
            }

            _data_store->put_immutable(value);

            return send_reply({});
        }
    } else {
        return send_error(204, "Query type not implemented");
    }
}


asio::ip::udp::endpoint resolve(
    asio::io_context& ioc,
    const std::string& addr,
    const std::string& port,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    using asio::ip::udp;

    sys::error_code ec;

    udp::resolver::query bootstrap_query(addr, port);
    udp::resolver bootstrap_resolver(ioc);

    auto cancelled = cancel_signal.connect([&] {
        bootstrap_resolver.cancel();
    });

    udp::resolver::iterator it = bootstrap_resolver.async_resolve(bootstrap_query, yield[ec]);

    if (cancelled) {
        return or_throw<udp::endpoint>(yield, asio::error::operation_aborted);
    }

    if (ec) {
        return or_throw<udp::endpoint>(yield, ec);
    }

    if (it != udp::resolver::iterator()) {
        return it->endpoint();
    }

    return or_throw<udp::endpoint>(yield, asio::error::not_found);
}

std::pair< asio::ip::udp::endpoint
         , asio::ip::udp::endpoint
         >
dht::DhtNode::bootstrap_single( std::string bootstrap_domain
                              , asio::yield_context yield)
{
    using T = std::pair<asio::ip::udp::endpoint, asio::ip::udp::endpoint>;
    sys::error_code ec;

    auto bootstrap_ep = resolve(
        _ios,
        bootstrap_domain,
        "6881",
        _cancel,
        yield[ec]
    );

    if (ec == asio::error::operation_aborted) {
        return or_throw<T>(yield, asio::error::operation_aborted);
    }
    if (ec) {
        cerr << "Unable to resolve bootstrap server "
             << bootstrap_domain << " (" << ec.message() << ") giving up\n";
        return or_throw<T>(yield, ec);
    }

    BencodedMap initial_ping_reply = send_query_await_reply(
        { bootstrap_ep, boost::none },
        "ping",
        BencodedMap{{ "id" , _node_id.to_bytestring() }},
        nullptr,
        nullptr,
        _cancel,
        yield[ec]
    );

    if (ec == asio::error::operation_aborted) {
        return or_throw<T>(yield, asio::error::operation_aborted);
    }

    if (ec) {
        std::cerr << "Bootstrap server " << bootstrap_domain
                  << " does not reply (" << ec.message() << ") giving up\n";
        return or_throw<T>(yield, ec);
    }

    boost::optional<std::string> my_ip = initial_ping_reply["ip"].as_string();

    if (!my_ip) {
        std::cerr << "Unexpected bootstrap server reply, giving up\n";
        return or_throw<T>(yield, asio::error::fault);
    }

    boost::optional<asio::ip::udp::endpoint> my_endpoint = decode_endpoint(*my_ip);

    if (!my_endpoint) {
        std::cerr << "Unexpected bootstrap server reply, giving up\n";
        return or_throw<T>(yield, asio::error::fault);
    }

    return std::make_pair(*my_endpoint, bootstrap_ep);
}

void dht::DhtNode::bootstrap(asio::yield_context yield)
{
    sys::error_code ec;

    asio::ip::udp::endpoint my_endpoint;
    asio::ip::udp::endpoint bootstrap_ep;

    vector<string> bootstraps { "router.bittorrent.com"
                              , "router.utorrent.com"
                              , "router.transmissionbt.com" };

    {
        bool done = false;

        std::random_device r;
        auto rng = std::default_random_engine(r());
        std::shuffle(bootstraps.begin(), bootstraps.end(), rng);

        do {
            for (const auto bs : bootstraps) {
                ec = sys::error_code();

                std::tie(my_endpoint, bootstrap_ep) = bootstrap_single(bs, yield[ec]);

                if (ec == asio::error::operation_aborted)
                    return or_throw(yield ,ec);

                if (!ec) { done = true; break; }
            }
            if (!async_sleep(_ios, std::chrono::seconds(10), _cancel, yield))
                return or_throw(yield, asio::error::operation_aborted);
        }
        while (!done);
    }

    _node_id = NodeID::generate(my_endpoint.address());
    _wan_endpoint = my_endpoint;

    LOG_DEBUG("BT WAN Endpoint: ", _wan_endpoint);

    _routing_table =
        std::make_unique<RoutingTable>( _node_id
                                      , [&](const NodeContact& c) {
                                            send_ping(c);
                                        });

    /*
     * TODO: Make bootstrap node handling and ID determination more reliable.
     *
     * Ideally, this process should start a coroutine that continuously tries
     * to keep a list of hardcoded bootstrap servers up to date, resolving and
     * pinging them; and the find_node procedure can then use these endpoints
     * as additional start points for any node searches.
     *
     * There also needs to be vastly more retrying and fallbacks here.
     */

    _bootstrap_endpoints.push_back(bootstrap_ep);

    /*
     * Lookup our own ID, constructing a basic path to ourselves.
     */
    find_closest_nodes(_node_id, _cancel, yield[ec]);

    if (ec) return or_throw(yield, ec);

    /*
     * We now know enough nodes that general DHT queries should succeed. The
     * remaining work is part of our participation in the DHT, but is not
     * necessary for implementing queries.
     */
    _ready = true;
}


template<class Evaluate>
void dht::DhtNode::collect(
    DebugCtx& dbg,
    const NodeID& target_id,
    Evaluate&& evaluate,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    if (!_routing_table) {
        // We're not yet bootstrapped.
        return or_throw(yield, asio::error::try_again);
    }

    // (Note: can't use lambda because we need default constructibility now)
    struct Compare {
        NodeID target_id;

        // Bootstrap nodes (those with id == boost::none) shall be ordered
        // last.
        bool operator()(const Contact& l, const Contact& r) const {
            if (!l.id && !r.id) return l.endpoint < r.endpoint;
            if ( l.id && !r.id) return true;
            if (!l.id &&  r.id) return false;
            return target_id.closer_to(*l.id, *r.id);
        }
    };

    using CandidateSet = std::set<Contact, Compare>;

    CandidateSet seed_candidates(Compare{target_id});

    std::set<udp::endpoint> added_endpoints;

    auto table_contacts =
        _routing_table->find_closest_routing_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    for (auto& contact : table_contacts) {
        seed_candidates.insert(contact);
        added_endpoints.insert(contact.endpoint);
    }

    for (auto ep : _bootstrap_endpoints) {
        if (added_endpoints.count(ep) != 0) continue;
        seed_candidates.insert({ ep, boost::none });
    }

    auto terminated = _cancel.connect([]{});
    ::ouinet::bittorrent::collect(
        dbg,
        _ios,
        std::move(seed_candidates),
        std::forward<Evaluate>(evaluate),
        cancel_signal,
        yield
    );
    if (terminated) {
        or_throw(yield, asio::error::operation_aborted);
    }
}

std::vector<dht::NodeContact> dht::DhtNode::find_closest_nodes(
    NodeID target_id,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;
    ProximityMap<udp::endpoint> out(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    collect(dbg, target_id, [&](
        const Contact& candidate,
        WatchDog& dms,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Cancel& cancel_signal,
        asio::yield_context yield
    ) {
        if (!candidate.id && out.full()) {
            return;
        }

        if (candidate.id && !out.would_insert(*candidate.id)) {
            return;
        }

        bool accepted = query_find_node2( target_id
                                        , candidate
                                        , closer_nodes
                                        , dms
                                        , &dbg
                                        , cancel_signal
                                        , yield[ec]);

        if (accepted && candidate.id) {
            out.insert({ *candidate.id, candidate.endpoint });
        }

        return;
    }
    , cancel_signal, yield[ec]);

    std::vector<NodeContact> output_set;
    for (auto& c : out) {
        output_set.push_back({ c.first, c.second });
    }

    return or_throw<std::vector<dht::NodeContact>>(yield, ec, std::move(output_set));
}

BencodedMap dht::DhtNode::send_ping(
    NodeContact contact,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;

    return send_query_await_reply(
        contact,
        "ping",
        BencodedMap{{ "id", _node_id.to_bytestring() }},
        nullptr,
        nullptr,
        cancel,
        yield[ec]
    );
}

void dht::DhtNode::send_ping(NodeContact contact)
{
    // It is currently expected that this function returns immediately, due to
    // that we need to spawn an unlimited number of coroutines.  Perhaps it
    // would be better if functions using this send_ping function would only
    // spawn a limited number of coroutines and use only that.
    asio::spawn(_ios, [this, contact] (asio::yield_context yield) {
        sys::error_code ec;
        Signal<void()> cancel_signal;

        send_ping(contact, cancel_signal, yield[ec]);
    });
}

/*
 * Send a query that writes data to the DHT. Repeat up to 5 times until we
 * get a positive response.
 */
void dht::DhtNode::send_write_query(
    udp::endpoint destination,
    NodeID destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    /*
     * Retry the write message a couple of times.
     */
    const int TRIES = 3;
    sys::error_code ec;
    for (int i = 0; i < TRIES; i++) {
        BencodedMap write_reply = send_query_await_reply(
            { destination, destination_id },
            query_type,
            query_arguments,
            nullptr,
            nullptr,
            cancel_signal,
            yield[ec]
        );

        if (ec == asio::error::operation_aborted) {
            break;
        } else if (!ec) {
            break;
        }
    }
    or_throw(yield, ec);
}

/**
 * Send a find_node query to a target node, and parse the reply.
 * @return True when received a valid response, false otherwise.
 */
// http://bittorrent.org/beps/bep_0005.html#find-node
bool dht::DhtNode::query_find_node(
    NodeID target_id,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", target_id.to_bytestring() }
        },
        nullptr,
        nullptr,
        cancel_signal,
        yield[ec]
    );

    if (ec) {
        return false;
    }
    if (find_node_reply["y"] != "r") {
        return false;
    }
    boost::optional<BencodedMap> response = find_node_reply["r"].as_map();
    if (!response) {
        return false;
    }

    if (is_v4()) {
        boost::optional<std::string> nodes = (*response)["nodes"].as_string();
        if (!NodeContact::decode_compact_v4(*nodes, closer_nodes)) {
            return false;
        }
    } else {
        boost::optional<std::string> nodes6 = (*response)["nodes6"].as_string();
        if (!NodeContact::decode_compact_v6(*nodes6, closer_nodes)) {
            return false;
        }
    }

    return !closer_nodes.empty();
}

bool dht::DhtNode::query_find_node2(
    NodeID target_id,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    assert(!cancel_signal);

    Cancel cancel(cancel_signal);

    sys::error_code ec;

    BencodedMap find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", target_id.to_bytestring() }
        },
        &dms,
        dbg,
        cancel,
        yield[ec]
    );

    return_or_throw_on_error(yield, cancel, ec, false);

    if (find_node_reply["y"] != "r") {
        return false;
    }

    boost::optional<BencodedMap> response = find_node_reply["r"].as_map();
    if (!response) {
        return false;
    }

    return read_nodes(is_v4(), *response, closer_nodes, cancel, yield[ec]);
}

// http://bittorrent.org/beps/bep_0005.html#get-peers
boost::optional<BencodedMap> dht::DhtNode::query_get_peers(
    NodeID infohash,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap get_peers_reply = send_query_await_reply(
        node,
        "get_peers",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "info_hash", infohash.to_bytestring() }
        },
        &dms,
        dbg,
        cancel_signal,
        yield[ec]
    );

    if (ec) {
        return boost::none;
    }
    if (get_peers_reply["y"] != "r") {
        return boost::none;
    }
    boost::optional<BencodedMap> response = get_peers_reply["r"].as_map();
    if (!response) {
        return boost::none;
    }

    std::vector<NodeContact> closer_nodes_v;

    if (is_v4()) {
        boost::optional<std::string> nodes = (*response)["nodes"].as_string();
        if (!NodeContact::decode_compact_v4(*nodes, closer_nodes_v)) {
            return boost::none;
        }
    } else {
        boost::optional<std::string> nodes6 = (*response)["nodes6"].as_string();
        if (!NodeContact::decode_compact_v6(*nodes6, closer_nodes_v)) {
            return boost::none;
        }
    }

    if (closer_nodes_v.empty()) {
        /*
         * We got a reply to get_peers, but it does not contain nodes.
         * Follow up with a find_node to fill the gap.
         */
        auto cancelled = cancel_signal.connect([]{});
        query_find_node(
            infohash,
            node,
            closer_nodes_v,
            cancel_signal,
            yield
        );
        if (cancelled) {
            return boost::none;
        }
    }

    closer_nodes.async_push_many(closer_nodes_v, cancel_signal, yield[ec]);

    return response;
}

// http://bittorrent.org/beps/bep_0044.html#get-message
boost::optional<BencodedMap> dht::DhtNode::query_get_data(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap get_reply = send_query_await_reply(
        node,
        "get",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", key.to_bytestring() }
        },
        nullptr,
        nullptr,
        cancel,
        yield[ec]
    );

    if (ec == asio::error::operation_aborted) {
        return boost::none;
    }

    if (ec) {
        /*
         * Ideally, nodes that do not implement BEP 44 would reply to this
         * query with a "not implemented" error. But in practice, most do not
         * reply at all. If such nodes make up the entire routing table (as is
         * often the case), the lookup might fail entirely. But doing an entire
         * search through nodes without BEP 44 support slows things down quite
         * a lot. Hm.
         *
         * TODO: Perhaps using a separate routing table for BEP 44 nodes would
         * improve things here?
         */
        query_find_node2(
            key,
            node,
            closer_nodes,
            dms, dbg,
            cancel,
            yield
        );
        return boost::none;
    }

    if (get_reply["y"] != "r") {
        /*
         * This is probably a node that does not implement BEP 44.
         * Query it using find_node instead. Ignore errors and hope for
         * the best; we are just trying to find some closer nodes here.
         */
        query_find_node2(
            key,
            node,
            closer_nodes,
            dms, dbg,
            cancel,
            yield
        );
        return boost::none;
    }

    boost::optional<BencodedMap> response = get_reply["r"].as_map();

    if (!response) return boost::none;

    read_nodes(is_v4(), *response, closer_nodes, cancel, yield[ec]);

    return response;
}

boost::optional<BencodedMap> dht::DhtNode::query_get_data2(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx& dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    assert(!cancel_signal);
    dms.expires_after( _stats->max_reply_wait_time("get")
                     + _stats->max_reply_wait_time("find_node"));

    Cancel local_cancel(cancel_signal);
    WaitCondition wc(_ios);

    // Ideally, nodes that do not implement BEP 44 would reply to this query
    // with a "not implemented" error. But in practice, most do not reply at
    // all. If such nodes make up the entire routing table (as is often the
    // case), the lookup might fail entirely. But doing an entire search
    // through nodes without BEP 44 support slows things down quite a lot.
    WatchDog wd(_ios, _stats->max_reply_wait_time("get"), [&] () mutable {
        if (local_cancel) return;
        asio::spawn(_ios, [&, lock = wc.lock()] ( asio::yield_context yield) {
            if (dbg) cerr << dbg << "query_find_node2 start " << node << "\n";
            sys::error_code ec;
            query_find_node2(key, node, closer_nodes, dms, &dbg, local_cancel, yield[ec]);
            if (dbg) cerr << dbg << "query_find_node2 end " << node << "\n";
            local_cancel();
        });
    });

    assert(!cancel_signal);
    assert(!local_cancel);
    if (dbg) cerr << dbg << "send_query_await_reply get start " << node << "\n";
    BencodedMap get_reply = send_query_await_reply(
        node,
        "get",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", key.to_bytestring() }
        },
        &dms,
        &dbg,
        local_cancel,
        yield[ec]
    );

    if (dbg) cerr << dbg << "send_query_await_reply get end " << node << " " << ec.message() << "\n";
    sys::error_code ec_;

    if (cancel_signal) ec = asio::error::operation_aborted;

    if (ec || get_reply["y"] != "r") {
        wc.wait(yield[ec_]);
        return boost::none;
    }

    local_cancel();
    wc.wait(yield[ec_]);

    std::vector<NodeContact> closer_nodes_v;

    boost::optional<BencodedMap> response = get_reply["r"].as_map();

    if (!response) return boost::none;

    read_nodes(is_v4(), *response, closer_nodes, cancel_signal, yield[ec]);

    return response;
}

boost::optional<BencodedMap> dht::DhtNode::query_get_data3(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx& dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    assert(!cancel_signal);
    //dms.expires_after( _stats->max_reply_wait_time("get")
    //                 + _stats->max_reply_wait_time("find_node"));

    Cancel local_cancel(cancel_signal);
    //WaitCondition wc(_ios);

    assert(!cancel_signal);
    assert(!local_cancel);
    if (dbg) cerr << dbg << "send_query_await_reply get start " << node << "\n";
    BencodedMap get_reply = send_query_await_reply(
        node,
        "get",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", key.to_bytestring() }
        },
        &dms,
        &dbg,
        local_cancel,
        yield[ec]
    );

    if (dbg) cerr << dbg << "send_query_await_reply get end " << node << " " << ec.message() << "\n";
    sys::error_code ec_;

    if (cancel_signal) ec = asio::error::operation_aborted;

    if (ec || get_reply["y"] != "r") {
        //wc.wait(yield[ec_]);
        return boost::none;
    }

    local_cancel();
    //wc.wait(yield[ec_]);

    std::vector<NodeContact> closer_nodes_v;

    boost::optional<BencodedMap> response = get_reply["r"].as_map();

    if (!response) return boost::none;

    read_nodes(is_v4(), *response, closer_nodes, cancel_signal, yield[ec]);

    return response;
}

/**
 * Perform a get_peers search. Returns the peers found, as well as necessary
 * data to later perform an announce operation.
 */
void dht::DhtNode::tracker_do_search_peers(
    NodeID infohash,
    std::set<tcp::endpoint>& peers,
    std::map<NodeID, TrackerNode>& responsible_nodes,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::vector<tcp::endpoint> peers;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes_full(infohash, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    collect(dbg, infohash, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Cancel& cancel_signal,
        asio::yield_context yield
    ) {
        if (!candidate.id && responsible_nodes_full.full()) {
            return;
        }
        if (candidate.id && !responsible_nodes_full.would_insert(*candidate.id)) {
            return;
        }

        boost::optional<BencodedMap> response_ = query_get_peers(
            infohash,
            candidate,
            closer_nodes,
            wd,
            &dbg,
            cancel_signal,
            yield
        );

        if (!response_) return;

        BencodedMap& response = *response_;

        boost::optional<std::string> announce_token = response["token"].as_string();

        if (!announce_token) return;

        if (candidate.id) {
            ResponsibleNode node{ candidate.endpoint, {}, *announce_token };
            boost::optional<BencodedList> encoded_peers = response["values"].as_list();
            if (encoded_peers) {
                for (auto& peer : *encoded_peers) {
                    boost::optional<std::string> peer_string = peer.as_string();
                    if (!peer_string) continue;

                    boost::optional<udp::endpoint> endpoint = decode_endpoint(*peer_string);
                    if (!endpoint) continue;

                    node.peers.push_back({endpoint->address(), endpoint->port()});
                }
            }
            responsible_nodes_full.insert({ *candidate.id, std::move(node) });
        }
    }, cancel_signal, yield[ec]);

    peers.clear();
    responsible_nodes.clear();
    for (auto& i : responsible_nodes_full) {
        peers.insert(i.second.peers.begin(), i.second.peers.end());
        responsible_nodes[i.first] = { i.second.node_endpoint, i.second.put_token };
    }

    or_throw(yield, ec);
}


MainlineDht::MainlineDht(asio::io_service& ios)
    : _ios(ios)
{
}

MainlineDht::~MainlineDht()
{
    _cancel();
}

void MainlineDht::set_endpoints(const std::set<udp::endpoint>& eps)
{
    // Remove nodes whose address is not listed in `eps`
    for (auto it = _nodes.begin(); it != _nodes.end(); ) {
        if (eps.count(it->first)) {
            ++it;
        } else {
            it = _nodes.erase(it);
        }
    }

    for (auto ep : eps) {
        if (_nodes.count(ep)) continue;

        asio_utp::udp_multiplexer m(_ios);
        sys::error_code ec;
        m.bind(ep, ec);
        assert(!ec);
        set_endpoint(move(m));
    }
}

void MainlineDht::set_endpoint(asio_utp::udp_multiplexer m)
{
    auto it = _nodes.find(m.local_endpoint());

    if (it != _nodes.end()) {
        it = _nodes.erase(it);
    }

    _nodes[m.local_endpoint()] = make_unique<dht::DhtNode>(_ios);

    asio::spawn(_ios, [&, m = move(m)] (asio::yield_context yield) mutable {
        auto ep = m.local_endpoint();
        auto con = _cancel.connect([&] { _nodes.erase(ep); });

        sys::error_code ec;
        _nodes[ep]->start(move(m), yield[ec]);
        assert(!con || ec == asio::error::operation_aborted);
    });
}

std::set<tcp::endpoint> MainlineDht::tracker_announce(
    NodeID infohash,
    boost::optional<int> port,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    std::set<tcp::endpoint> output;

    SuccessCondition condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [&, lock = condition.lock()] (asio::yield_context yield) {
            sys::error_code ec;
            Signal<void()> cancel_dummy;
            std::set<tcp::endpoint> peers = i.second->tracker_announce(infohash, port, cancel_dummy, yield[ec]);

            if (ec) {
                return;
            }

            output.insert(peers.begin(), peers.end());

            /*
             * TODO: We should distinguish here between
             * "did not query successfully" and "did not find any peers".
             * This needs error detection in _announce(), which does not exist.
             */
            if (peers.size()) {
                lock.release(true);
            }
        });
    }

    auto cancelled = cancel_signal.connect([&] {
        condition.cancel();
    });

    auto terminated = _cancel.connect([&] {
        condition.cancel();
    });

    if (!condition.wait_for_success(yield)) {
        if (condition.cancelled()) {
            return or_throw<std::set<tcp::endpoint>>(yield, asio::error::operation_aborted, std::move(output));
        } else {
            return or_throw<std::set<tcp::endpoint>>(yield, asio::error::network_unreachable);
        }
    }

    return output;
}

void MainlineDht::mutable_put(
    const MutableDataItem& data,
    Cancel& top_cancel,
    asio::yield_context yield
) {
    Cancel cancel(top_cancel);

    SuccessCondition condition(_ios);
    WaitCondition wait_all(_ios);

    for (auto& i : _nodes) {
        asio::spawn(_ios, [ &
                          , lock = condition.lock()
                          , lock_all = wait_all.lock()
                          ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            i.second->data_put_mutable(data, cancel, yield[ec]);

            if (ec) return;

            lock.release(true);
        });
    }

    auto cancelled = cancel.connect([&] {
        condition.cancel();
    });

    auto terminated = _cancel.connect([&] {
        condition.cancel();
    });

    sys::error_code ec;

    if (condition.wait_for_success(yield)) {
        cancel();
    } else {
        if (condition.cancelled()) { ec = asio::error::operation_aborted;   }
        else                       { ec = asio::error::network_unreachable; }
    }

    wait_all.wait(yield);

    return or_throw(yield, ec);
}

std::set<tcp::endpoint> MainlineDht::tracker_get_peers(NodeID infohash, Cancel& cancel_signal, asio::yield_context yield)
{
    std::set<tcp::endpoint> output;
    sys::error_code ec;

    Cancel cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            std::set<tcp::endpoint> peers = i.second->tracker_get_peers(infohash, cancel_attempts, yield[ec]);

            output.insert(peers.begin(), peers.end());

            if (peers.size()) {
                success.release(true);
            }
        });
    }

    auto cancelled = cancel_signal.connect([&] {
        success_condition.cancel();
    });
    auto terminated = _cancel.connect([&] {
        success_condition.cancel();
    });
    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::network_unreachable;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<std::set<tcp::endpoint>>(yield, ec);
}

boost::optional<BencodedValue> MainlineDht::immutable_get(
        NodeID key,
        Cancel& cancel_signal,
        asio::yield_context yield
) {
    boost::optional<BencodedValue> output;
    sys::error_code ec;

    Signal<void()> cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            boost::optional<BencodedValue> data = i.second->data_get_immutable(key, cancel_attempts, yield[ec]);

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancelled = cancel_signal.connect([&] {
        success_condition.cancel();
    });

    auto terminated = _cancel.connect([&] {
        success_condition.cancel();
    });

    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::not_found;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<boost::optional<BencodedValue>>(yield, ec);
}

boost::optional<MutableDataItem> MainlineDht::mutable_get(
    const util::Ed25519PublicKey& public_key,
    boost::string_view salt,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    boost::optional<MutableDataItem> output;
    sys::error_code ec;

    Signal<void()> cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);

    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            //if (!i.second->ready()) {
            //    return;
            //}

            sys::error_code ec;
            boost::optional<MutableDataItem> data = i.second->data_get_mutable(
                public_key,
                salt,
                cancel_attempts,
                yield[ec]
            );

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancelled = cancel_signal.connect([&] {
        success_condition.cancel();
    });
    auto terminated = _cancel.connect([&] {
        success_condition.cancel();
    });

    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::not_found;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw(yield, ec, std::move(output));
}


void MainlineDht::wait_all_ready(
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    auto cancelled = _cancel.connect([&] {
        cancel_signal();
    });
    sys::error_code ec;
    while (!ec && !all_ready()) {
        async_sleep(_ios, std::chrono::milliseconds(200), cancel_signal, yield[ec]);
    }
    return or_throw(yield, ec);
}


std::ostream& operator<<(std::ostream& os, const Contact& c)
{
    os << "(Contact " << c.endpoint << " id:";
    if (c.id) {
        os << *c.id;
    } else {
        os << "none";
    }
    return os << ")";
}


} // bittorrent namespace
} // ouinet namespace
