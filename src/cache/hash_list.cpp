#include "hash_list.h"
#include "http_sign.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

#define _LOG_PFX "HashList: "
#define _WARN(...) LOG_WARN(_LOG_PFX, __VA_ARGS__)

static const size_t MAX_LINE_SIZE_BYTES = 512;

static const char* MAGIC = "OUINET_HASH_LIST_V1";

using Digest = util::SHA512::digest_type;

bool HashList::verify() const {
    boost::optional<Digest> last_digest;

    size_t block_size = signed_head.block_size();
    size_t last_offset = 0;

    bool first = true;

    for (auto& digest : block_hashes) {
        util::SHA512 sha;

        if (last_digest) {
            sha.update(*last_digest);
        }

        sha.update(digest);

        last_digest = sha.close();

        if (first) {
            first = false;
        } else {
            last_offset += block_size;
        }
    }

    if (!last_digest) return false;


    return cache::Block::verify( signed_head.injection_id()
                               , last_offset
                               , *last_digest
                               , signature
                               , signed_head.public_key());
}

struct Parser {
    using Data = std::vector<uint8_t>;

    Data buffer;

    void append_data(const Data& data) {
        buffer.insert(buffer.end(), data.begin(), data.end());
    }

    // Returns a line of data (optionally)
    boost::optional<string> read_line() {
        auto nl_i = find_nl(buffer);

        if (nl_i == buffer.end()) {
            return boost::none;
        }

        string ret(buffer.begin(), nl_i);
        buffer.erase(buffer.begin(), std::next(nl_i));

        return ret;
    }

    boost::optional<util::Ed25519PublicKey::sig_array_t>
    read_signature() {
        return read_array<util::Ed25519PublicKey::sig_size>();
    }

    boost::optional<Digest>
    read_hash() {
        return read_array<util::SHA512::size()>();
    }

    template<size_t N>
    boost::optional<std::array<uint8_t, N>> read_array() {
        if (buffer.size() < N) return boost::none;
        auto b = buffer.begin();
        auto e = b + N;
        std::array<uint8_t, N> ret;
        std::copy(b, e, ret.begin());
        buffer.erase(b, e);
        return ret;
    }

    Data::iterator find_nl(Data& data) const {
        return std::find(data.begin(), data.end(), '\n');
    }
};

/* static */
HashList HashList::load(
        http_response::AbstractReader& r,
        const PubKey& pk,
        Cancel& c,
        asio::yield_context y)
{
    static const auto bad_msg = sys::errc::make_error_code(sys::errc::bad_message);

    assert(!c);

    sys::error_code ec;

    auto part = r.async_read_part(c, y[ec]);

    if (!ec && !part) {
        assert(0);
        ec = c ? asio::error::operation_aborted
               : sys::errc::make_error_code(sys::errc::bad_message);
    }

    if (c) ec = asio::error::operation_aborted;
    if (ec) return or_throw<HashList>(y, ec);

    return_or_throw_on_error(y, c, ec, HashList{});

    if (!part->is_head()) return or_throw<HashList>(y, bad_msg);

    auto head_o = SignedHead::verify_and_create(move(*part->as_head()), pk);

    if (!head_o) return or_throw<HashList>(y, bad_msg);

    Parser parser;

    bool magic_checked = false;
    boost::optional<PubKey::sig_array_t> signature;
    std::vector<Digest> hashes;

    while (true) {
        part = r.async_read_part(c, y[ec]);
        return_or_throw_on_error(y, c, ec, HashList{});

        if (!part) break;

        if (part->is_body()) {
            parser.append_data(*part->as_body());
        } else if (part->is_chunk_body()) {
            parser.append_data(*part->as_chunk_body());
        } else {
            continue;
        }

        bool progress = false;

        while (true) {
            if (!magic_checked) {
                auto magic_line = parser.read_line();
                if (magic_line) {
                    if (*magic_line != MAGIC)
                        return or_throw<HashList>(y, bad_msg);
                    progress = true;
                }
            } else if (!signature) {
                signature = parser.read_signature();
                if (signature) progress = true;
            } else {
                auto hash = parser.read_hash();
                if (hash) {
                    progress = true;
                    hashes.push_back(move(*hash));
                }
            }
            if (!progress) {
                if (parser.buffer.size() > MAX_LINE_SIZE_BYTES) {
                    _WARN("Line too long");
                    return or_throw<HashList>(y, bad_msg);
                }
            }
        }
    }

    if (!signature || hashes.empty()) return or_throw<HashList>(y, bad_msg);

    HashList hs{move(*head_o), move(*signature), move(hashes)};

    if (!hs.verify()) {
        return or_throw<HashList>(y, bad_msg);
    }

    return hs;
}
