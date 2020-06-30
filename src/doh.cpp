#include "doh.h"

#include <cstring>
#include <sstream>
#include <vector>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/utility/string_view.hpp>
#include <dnsparser.h>

#include "logger.h"
#include "split_string.h"
#include "util.h"

namespace ouinet { namespace doh {

static const std::string doh_content_type = "application/dns-message";

boost::optional<Endpoint>
endpoint_from_base(const std::string& base)
{
    util::url_match um;
    if (!util::match_http_url(base, um) || !um.fragment.empty())
        return boost::none;
    um.query += um.query.empty() ? "dns=" : "&dns=";
    return um.reassemble();
}

static
boost::optional<std::string>
dns_query(const std::string& name)
{
    // Use string literals to avoid cutting on first NUL
    // (from <https://stackoverflow.com/a/164274>).
    using namespace std::literals::string_literals;

    // The hardwired values here are taken from a capture of
    // Firefox DoH traffic.
    static const std::string dq_prefix{
        // DNS message header
        "\x00\x00"  // ID set to 0 as per RFC8484#4.1
        "\x01\x00"  // query of type QUERY, recursive
        "\x00\x01"  // 1 question record
        "\x00\x00"  // 0 answer records
        "\x00\x00"  // 0 name server records

#ifdef FIREFOX_DOH
        // Firefox appends an EDNS RR.
        "\x00\x01"  // 1 additional record (EDNS)
#else
        // We keep the query minimal to increase the chances of sharing.
        "\x00\x00"  // 0 additional records
#endif

        ""s
    };
    static const std::string dq_suffix = {
        // DNS question
        // (queried name comes here)
        "\x00\x01"  // A (IPv4) type  // TODO: IPv6? (28)
        "\x00\x01"  // IN (Internet) class

#ifdef FIREFOX_DOH
        // EDNS (RFC6891#6.1.2)
        // All stuff from here on seems to explicitly tell the server that
        // no source address bits are relevant for choosing
        // between different possible answers.
        "\x00"      // root domain
        "\x00\x29"  // OPT (41)
        "\x10\x00"  // 4K payload size, i.e. the value of `payload_size`
        "\x00"      // unextended RCODE (RFC6891#6.1.3)
        "\x00"      // EDNS version 0 (RFC6891#6.1.3)
        "\x00\x00"  // DNSSEC not ok, zeros (RFC6891#6.1.4)
        "\x00\x08"  // RDATA length
        // EDNS RDATA
        // Actual EDNS option: client subnet (RFC7871#6)
        "\x00\x08"  // option code 8 (client subnet)
        "\x00\x04"  // option length
        "\x00\x01"  // family 1 (IPv4)  // TODO: IPv6? (2)
        "\x00"      // source prefix length
        "\x00"      // scope prefix-length, zero in queries
#endif

        ""s
    };

    // 1 (1st label len byte) + len(name) + 1 (root label len byte) <= 255
    // as per RFC1035#3.1.
    if (name.size() > 253) return boost::none;

    std::stringstream dq;

    dq << dq_prefix;

    // Turn "example.com" into "\x07example\x03com\x00" as per RC1035#3.1.
    for (auto l : SplitString(name, '.')) {
        uint8_t llen = l.size();
        if (llen < 1 || llen > 63) return boost::none;  // RFC1035#3.1
        dq << llen << l;
    }
    dq << '\0';

    dq << dq_suffix;

    return dq.str();
}

boost::optional<Request>
build_request( const std::string& name
             , const Endpoint& ep)
{
    auto dq_o = dns_query(name);
    if (!dq_o) return boost::none;

    // DoH uses unpadded base64url as defined in RFC4648#5 (RFC8484#6).
    auto dq_b64 = util::base64_encode(*dq_o);
    for (auto& c : dq_b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    auto target_padded = ep + dq_b64;
    boost::string_view target(target_padded);
    while (target.ends_with('=')) target.remove_suffix(1);

    // TODO: keep host in endpoint
    auto ep_hstart = ep.find("://") + 3;
    auto ep_hend = ep.find('/', ep_hstart);
    boost::string_view ep_host(ep);
    ep_host.remove_suffix(ep.size() - ep_hend);
    ep_host.remove_prefix(ep_hstart);

    // RFC8484#4.1
    Request rq{http::verb::get, target, 11 /* HTTP/1.1 */};
    rq.set(http::field::host, ep_host);
    rq.set(http::field::accept, doh_content_type);
    return rq;
}

using EndpointVector = std::vector<TcpLookup::endpoint_type>;

// Appends endpoints to the given vector on answers for the given host.
class Listener : public DnsParserListener {
public:
    Listener( const std::string& host, unsigned short port
            , EndpointVector& epv)
        : _host(host), _port(port), _epv(epv)
    {}

    void onDnsRec(in_addr addr, std::string name, std::string) override
    {
        if (name != _host) return;  // unrelated answer, ignore
        auto ip4addr = asio::ip::make_address_v4(::ntohl(addr.s_addr));
        LOG_DEBUG("DoH: ", name, " -> ", ip4addr);
        _epv.push_back({std::move(ip4addr), _port});
    }

    void onDnsRec(in6_addr addr, std::string name, std::string) override
    {
        if (name != _host) return;  // unrelated answer, ignore
        asio::ip::address_v6::bytes_type addrb;
        static_assert(addrb.size() == sizeof(addr.s6_addr));
        std::memcpy(addrb.data(), addr.s6_addr, addrb.size());
        auto ip6addr = asio::ip::make_address_v6(addrb);
        LOG_DEBUG("DoH: ", name, " -> ", ip6addr);
        _epv.push_back({std::move(ip6addr), _port});
    }

private:
    const std::string& _host;
    unsigned short _port;
    EndpointVector& _epv;
};

TcpLookup
parse_response( const Response& rs
              , const std::string& host
              , unsigned short port
              , sys::error_code& ec)
{
    if ( rs.result() != http::status::ok
       || rs[http::field::content_type] != doh_content_type) {  // RFC8484#5.1
        ec = asio::error::invalid_argument;
        return {};
    }

    EndpointVector epv;
    try {
        Listener dnsl(host, port, epv);
        std::unique_ptr<DnsParser> dnsp(DnsParserNew(&dnsl, false, true));  // no paths, no CNAMEs
        assert(dnsp);
        // The DNS parser not specifying pointer-to-const arguments
        // forces us to copy the body, plus C++17 support in some versions of GCC
        // does not implement the non-const `std::string::data()`.
        //if (dnsp->parse(rs.body().data(), rs.body().size()) == -1)
        auto body = rs.body();
        if (dnsp->parse(&body.front(), body.size()) == -1)
            ec = asio::error::invalid_argument;
    } catch (const std::exception&) {
        ec = asio::error::no_memory;
    }

    // Assume that the DoH server is not authoritative.
    if (!ec && epv.empty()) ec = asio::error::host_not_found_try_again;

    if (ec) return {};

    auto port_s = std::to_string(port);
    return TcpLookup::create(epv.begin(), epv.end(), host, port_s);
}

}} // ouinet::doh namespace
