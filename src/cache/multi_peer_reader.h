#pragma once

#include <set>
#include <boost/asio/ip/udp.hpp>
#include "../response_reader.h"
#include "../namespaces.h"
#include "../bittorrent/dht.h"
#include "dht_lookup.h"
#include "hash_list.h"
#include "../util/async_generator.h"
#include "../session.h"

namespace ouinet { namespace cache {

class MultiPeerReader : public http_response::AbstractReader {
private:
    class Peer;
    class Peers;
    struct Block;
    struct NextJob;

    enum class State { active, done, closed };

public:
    MultiPeerReader( asio::executor ex
                   , util::Ed25519PublicKey cache_pk
                   , std::set<asio::ip::udp::endpoint> local_peers
                   , std::string key
                   , std::shared_ptr<bittorrent::MainlineDht> dht
                   , std::string dht_group
                   , std::shared_ptr<DhtLookup> dht_lookup
                   , std::shared_ptr<unsigned> newest_proto_seen
                   , const std::string& dbg_tag);

    MultiPeerReader(MultiPeerReader&&) = delete;
    MultiPeerReader(const MultiPeerReader&) = delete;

    boost::optional<http_response::Part> async_read_part(Cancel, asio::yield_context) override;

    bool is_done() const override
    {
        return _state == State::done;
    }

    void close() override;

    ~MultiPeerReader();

    asio::executor get_executor() override
    {
        return _executor;
    }

private:
    boost::optional<http_response::Part> async_read_part_impl(Cancel&, asio::yield_context);
    boost::optional<Block> fetch_block(size_t block_id, Cancel& cancel, asio::yield_context yield);
    void unmark_as_good(Peer& peer);

    void mark_done();

private:
    asio::executor _executor;
    Cancel _lifetime_cancel;

    boost::optional<HashList> _reference_hash_list;
    std::unique_ptr<Peers> _peers;
    std::string _dbg_tag;
    bool _head_sent = false;
    size_t _block_id = 0;

    std::string _next_chunk_hdr_ext;
    boost::optional<http_response::ChunkBody> _next_chunk_body;
    boost::optional<http_response::Trailer> _next_trailer;
    bool _last_chunk_hdr_sent = false;

    State _state = State::active;

    std::unique_ptr<NextJob> _next_job;
};

}}
