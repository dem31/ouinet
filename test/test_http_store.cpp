#define BOOST_TEST_MODULE http_store
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <sstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem.hpp>

#include <cache/http_sign.h>
#include <cache/http_store.h>
#include <defer.h>
#include <response_part.h>
#include <session.h>
#include <util/bytes.h>
#include <util/file_io.h>
#include <util/str.h>

#include <namespaces.h>
#include "connected_pair.h"

// For checks to be able to report errors.
namespace ouinet { namespace http_response {
    std::ostream& operator<<(std::ostream& os, const ChunkHdr& hdr) {
        return os << "ChunkHdr(" << hdr.size << ", \"" << hdr.exts << "\")";
    }

    std::ostream& operator<<(std::ostream& os, const Trailer& trailer) {
        return os << static_cast<Trailer::Base>(trailer);
    }
}} // namespace ouinet::http_response

using first_last = std::pair<unsigned, unsigned>;
// <https://stackoverflow.com/a/33965517>
namespace boost { namespace test_tools { namespace tt_detail {
    template<>
    struct print_log_value<first_last> {
        void operator()(std::ostream& os, const first_last& p) {
            os << "{" << p.first << ", " << p.second << "}";
        }
    };
}}} // namespace boost::test_tools::tt_detail

BOOST_AUTO_TEST_SUITE(ouinet_http_store)

using namespace std;
using namespace ouinet;

// This signed response used below comes from `test-http-sign`.

static const string _rs_status_origin =
    "HTTP/1.1 200 OK\r\n";
static const string _rs_fields_origin = (
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
);

static const string _rs_head_origin =
    ( _rs_status_origin
    + _rs_fields_origin);

static const string _rs_head_injection = (
    "X-Ouinet-Version: 3\r\n"
    "X-Ouinet-URI: https://example.com/foo\r\n"
    "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"
    "X-Ouinet-BSigs: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",size=65536\r\n"
);

static const string _rs_head_sig0 = (
    "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048310,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs\","
    "signature=\"tnVAAW/8FJs2PRgtUEwUYzMxBBlZpd7Lx3iucAt9q5hYXuY5ci9T7nEn7UxyKMGA1ZvnDMDBbs40dO1OQUkdCA==\"\r\n"
);

static const string _rs_head_framing = (
    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"
);

static const string rs_head =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + _rs_head_framing
    + "\r\n");

static const string _rs_head_digest = (
    "X-Ouinet-Data-Size: 131076\r\n"
    "Digest: SHA-256=E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=\r\n"
);

static const string _rs_head_sig1 = (
    "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048311,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs "
    "x-ouinet-data-size "
    "digest\","
    "signature=\"h/PmOlFvScNzDAUvV7tLNjoA0A39OL67/9wbfrzqEY7j47IYVe1ipXuhhCfTnPeCyXBKiMlc4BP+nf0VmYzoAw==\"\r\n"
);

static const string rs_trailer =
    ( _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

static const string _rs_block0_head = "0123";
static const string _rs_block0_tail = "4567";
static const string _rs_block1_head = "89AB";
static const string _rs_block1_tail = "CDEF";
static const string _rs_block2 = "abcd";
static const char _rs_block_fill_char = 'x';
static const size_t _rs_block_fill = ( http_::response_data_block
                                     - _rs_block0_head.size()
                                     - _rs_block0_tail.size());

static const array<string, 3> rs_block_data{
    _rs_block0_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block0_tail,
    _rs_block1_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block1_tail,
    _rs_block2,
};

static const array<string, 3> rs_block_hash{
    "",
    "aERfr5o+kpvR4ZH7xC0mBJ4QjqPUELDzjmzt14WmntxH2p3EQmATZODXMPoFiXaZL6KNI50Ve4WJf/x3ma4ieA==",
    "slwciqMQBddB71VWqpba+MpP9tBiyTE/XFmO5I1oiVJy3iFniKRkksbP78hCEWOM6tH31TGEFWP1loa4pqrLww==",
};

static const array<string, 3> rs_block_sig{
    "AwiYuUjLYh/jZz9d0/ev6dpoWqjU/sUWUmGL36/D9tI30oaqFgQGgcbVCyBtl0a7x4saCmxRHC4JW7cYEPWwCw==",
    "c+ZJUJI/kc81q8sLMhwe813Zdc+VPa4DejdVkO5ZhdIPPojbZnRt8OMyFMEiQtHYHXrZIK2+pKj2AO03j70TBA==",
    "m6sz1NpU/8iF6KNN6drY+Yk361GiW0lfa0aaX5TH0GGW/L5GsHyg8ozA0ejm29a+aTjp/qIoI1VrEVj1XG/gDA==",
};

static const array<string, 4> rs_chunk_ext{
    "",
    ";ouisig=\"" + rs_block_sig[0] + "\"",
    ";ouisig=\"" + rs_block_sig[1] + "\"",
    ";ouisig=\"" + rs_block_sig[2] + "\"",
};

template<class F>
static void run_spawned(asio::io_context& ctx, F&& f) {
    asio::spawn(ctx, [&ctx, f = forward<F>(f)] (auto yield) {
            try {
                f(yield);
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ctx.run();
}

static const string rs_head_incomplete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + "\r\n");

static const string rs_body_incomplete =
    ( rs_block_data[0]
    + rs_block_data[1]);

static const string rs_head_complete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

static const string rs_body_complete =
    ( rs_block_data[0]
    + rs_block_data[1]
    + rs_block_data[2]);

static string rs_sigs(bool complete) {
    stringstream ss;
    ss << hex;
    // Last signature missing when incomplete.
    auto last_b = complete ? rs_block_data.size() : rs_block_data.size() - 1;
    for (size_t b = 0; b < last_b; ++b)
        ss << (b * http_::response_data_block)
           << ' ' << rs_block_sig[b] << ' ' << rs_block_hash[b]
           << endl;
    return ss.str();
}

static const bool true_false[] = {true, false};

BOOST_DATA_TEST_CASE(test_write_response, boost::unit_test::data::make(true_false), complete) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, complete, lock = wc.lock()] (auto y) {
            // Head (raw).
            asio::async_write( signed_w
                             , asio::const_buffer(rs_head.data(), rs_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                ch.async_write(signed_w, y);
                // For the incomplete test, produce the chunk header of last block
                // but not its body; the last block signature should be missing.
                if (!complete && bi == rs_block_data.size() - 1) break;
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            if (!complete) {  // no last chunk nor trailer
                signed_w.close();
                return;
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            chZ.async_write(signed_w, y);
            asio::async_write( signed_w
                             , asio::const_buffer(rs_trailer.data(), rs_trailer.size())
                             , y);

            signed_w.close();
        });

        // Store response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir, complete
                         , &ctx, lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader signed_rr(std::move(signed_r));
            cache::http_store_v1(signed_rr, tmpdir, ctx.get_executor(), c, y[e]);
            BOOST_CHECK(!complete || !e);
        });

        wc.wait(yield);

        auto read_file = [&] (auto fname, auto c, auto y) -> string {
            sys::error_code e;
            auto f = util::file_io::open_readonly(ctx.get_executor(), tmpdir / fname, e);
            if (e) return or_throw(y, e, "");

            size_t fsz = util::file_io::file_size(f, e);
            if (e) return or_throw(y, e, "");

            std::string fdata(fsz, '\0');
            util::file_io::read(f, asio::buffer(fdata), c, y[e]);
            return_or_throw_on_error(y, c, e, "");

            return fdata;
        };

        Cancel cancel;
        sys::error_code ec;

        auto head = read_file("head", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(head, complete ? rs_head_complete :  rs_head_incomplete);

        auto body = read_file("body", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(body, complete ? rs_body_complete : rs_body_incomplete);

        auto sigs = read_file("sigs", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(sigs, rs_sigs(complete));
    });
}

BOOST_AUTO_TEST_CASE(test_read_response_missing) {
    auto tmpdir = fs::unique_path();
    asio::io_context ctx;
    sys::error_code ec;
    auto store_rr = cache::http_store_reader_v1(tmpdir, ctx.get_executor(), ec);
    BOOST_CHECK(!store_rr);
    BOOST_CHECK_EQUAL(ec, sys::errc::no_such_file_or_directory);
}


static const string rrs_head_incomplete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + "Transfer-Encoding: chunked\r\n"
    + "\r\n");

static const string rrs_head_complete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_digest
    + _rs_head_sig1
    + "Transfer-Encoding: chunked\r\n"
    + "\r\n");

static const array<string, 4> rrs_chunk_ext{
    "",
    ";ouisig=\"" + rs_block_sig[0] + "\"",
    ";ouisig=\"" + rs_block_sig[1] + "\";ouihash=\"" + rs_block_hash[1] + "\"",
    ";ouisig=\"" + rs_block_sig[2] + "\";ouihash=\"" + rs_block_hash[2] + "\"",
};

// Trailers are merged into the initial head,
// so the loaded trailer is always empty.
static const http_response::Trailer rrs_trailer{};

BOOST_DATA_TEST_CASE(test_read_response, boost::unit_test::data::make(true_false), complete) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, complete, lock = wc.lock()] (auto y) {
            // Head (raw).
            asio::async_write( signed_w
                             , asio::const_buffer(rs_head.data(), rs_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                ch.async_write(signed_w, y);
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            if (!complete) {  // no last chunk nor trailer
                // Last block signature should be missing
                // and its data should not be sent even if available on disk.
                signed_w.close();
                return;
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            chZ.async_write(signed_w, y);
            asio::async_write( signed_w
                             , asio::const_buffer(rs_trailer.data(), rs_trailer.size())
                             , y);

            signed_w.close();
        });

        // Store response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir, complete
                         , &ctx, lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader signed_rr(std::move(signed_r));
            cache::http_store_v1(signed_rr, tmpdir, ctx.get_executor(), c, y[e]);
            BOOST_CHECK(!complete || !e);
        });

        wc.wait(yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        // Load response.
        asio::spawn(ctx, [ &loaded_w, &tmpdir, complete
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            auto store_rr = cache::http_store_reader_v1(tmpdir, ctx.get_executor(), e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK(!complete || !e);
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r), complete
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , complete ? rrs_head_complete : rrs_head_incomplete);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                part = loaded_rr.async_read_part(c, y[e]);
                BOOST_CHECK_EQUAL(e.message(), "Success");
                BOOST_REQUIRE(part);
                BOOST_REQUIRE(part->is_chunk_hdr());
                BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                                   , http_response::ChunkHdr( rs_block_data[bi].size()
                                                            , rrs_chunk_ext[bi]));

                // For the incomplete test, the last block signature should be missing,
                // so we will not get its data.
                if (!complete && bi == rs_block_data.size() - 1) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_REQUIRE(!part);
                    break;
                }
                std::vector<uint8_t> bd;  // accumulate data here
                for (bool done = false; !done; ) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_CHECK_EQUAL(e.message(), "Success");
                    BOOST_REQUIRE(part);
                    BOOST_REQUIRE(part->is_chunk_body());
                    auto& d = *(part->as_chunk_body());
                    bd.insert(bd.end(), d.cbegin(), d.cend());
                    done = (d.remain == 0);
                }
                BOOST_REQUIRE_EQUAL( util::bytes::to_string(bd)
                                   , rs_block_data[bi]);
            }

            if (!complete) return;

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , rrs_chunk_ext[bi]));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
        });

        wc.wait(yield);
    });
}

// About the blocks in the requested data range:
//
//     We have: [ 64K ][ 64K ][ 4B ]
//     We want:          [32K][2B]
//     We get:         [ 64K ][ 4B ]
//
static string rrs_head_partial(unsigned first_block, unsigned last_block) {
    size_t first = first_block * http_::response_data_block;
    size_t last = ( (last_block * http_::response_data_block)
                  + rs_block_data[last_block].size() - 1);
    return util::str
        ( "HTTP/1.1 206 Partial Content\r\n"
        , _rs_fields_origin
        , _rs_head_injection
        , _rs_head_digest
        , _rs_head_sig1
        , "X-Ouinet-HTTP-Status: 200\r\n"
        , "Content-Range: bytes ", first, '-', last, "/131076\r\n"
        , "Transfer-Encoding: chunked\r\n"
        , "\r\n");
}

static const first_last block_ranges[] = {
    {0, 0},  // just first block
    {0, 1},  // two first blocks
    {0, 2},  // all blocks
    {1, 2},  // two last blocks
    {2, 2},  // just last block
};

BOOST_DATA_TEST_CASE( test_read_response_partial
                    , boost::unit_test::data::make(block_ranges), firstb_lastb) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, lock = wc.lock()] (auto y) {
            // Head (raw).
            asio::async_write( signed_w
                             , asio::const_buffer(rs_head.data(), rs_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                ch.async_write(signed_w, y);
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            chZ.async_write(signed_w, y);
            asio::async_write( signed_w
                             , asio::const_buffer(rs_trailer.data(), rs_trailer.size())
                             , y);

            signed_w.close();
        });

        // Store response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir
                         , &ctx, lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader signed_rr(std::move(signed_r));
            cache::http_store_v1(signed_rr, tmpdir, ctx.get_executor(), c, y);
        });

        wc.wait(yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        // Load partial response:
        // request from middle first block to middle last block.
        // Use first byte *after* middle last block
        // to avoid using an inverted range
        // when first and last blocks match.
        unsigned first_block, last_block;
        tie(first_block, last_block) = firstb_lastb;
        asio::spawn(ctx, [ &loaded_w, &tmpdir
                         , first_block, last_block
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            size_t first = (first_block * http_::response_data_block) + rs_block_data[first_block].size() / 2;
            size_t last = (last_block * http_::response_data_block) + rs_block_data[last_block].size() / 2;
            auto store_rr = cache::http_store_range_reader_v1
                (tmpdir, ctx.get_executor(), first, last, e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r)
                         , first_block, last_block
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , rrs_head_partial(first_block, last_block));

            // Chunk headers and bodies (one chunk per block).
            // We start on the first block of the partial range.
            bool first_chunk = true;
            unsigned bi;
            for (bi = first_block; bi <= last_block; ++bi, first_chunk=false) {
                part = loaded_rr.async_read_part(c, y[e]);
                BOOST_CHECK_EQUAL(e.message(), "Success");
                BOOST_REQUIRE(part);
                BOOST_REQUIRE(part->is_chunk_hdr());
                BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                                   , http_response::ChunkHdr( rs_block_data[bi].size()
                                                            , first_chunk ? "" : rrs_chunk_ext[bi]));

                std::vector<uint8_t> bd;  // accumulate data here
                for (bool done = false; !done; ) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_CHECK_EQUAL(e.message(), "Success");
                    BOOST_REQUIRE(part);
                    BOOST_REQUIRE(part->is_chunk_body());
                    auto& d = *(part->as_chunk_body());
                    bd.insert(bd.end(), d.cbegin(), d.cend());
                    done = (d.remain == 0);
                }
                BOOST_REQUIRE_EQUAL( util::bytes::to_string(bd)
                                   , rs_block_data[bi]);
            }

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , rrs_chunk_ext[bi]));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_read_response_partial_off) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, lock = wc.lock()] (auto y) {
            // Head (raw).
            asio::async_write( signed_w
                             , asio::const_buffer(rs_head.data(), rs_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                ch.async_write(signed_w, y);
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            chZ.async_write(signed_w, y);
            asio::async_write( signed_w
                             , asio::const_buffer(rs_trailer.data(), rs_trailer.size())
                             , y);

            signed_w.close();
        });

        // Store response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir
                         , &ctx, lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader signed_rr(std::move(signed_r));
            cache::http_store_v1(signed_rr, tmpdir, ctx.get_executor(), c, y);
        });

        wc.wait(yield);

        sys::error_code e;
        auto store_rr = cache::http_store_range_reader_v1
            ( tmpdir, ctx.get_executor()
            , 0, 42'000'000  // off limits
            , e);
        BOOST_CHECK_EQUAL(e, sys::errc::invalid_seek);
        BOOST_CHECK(!store_rr);
    });
}

BOOST_AUTO_TEST_SUITE_END()
