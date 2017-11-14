#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/client.h>
#include <ipfs_cache/error.h>
#include <ipfs_cache/timer.h>

#include "namespaces.h"
#include "connect_to_host.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;

static string REPO_ROOT;

//------------------------------------------------------------------------------
static
void handle_bad_request( GenericConnection& con
                       , const Request& req
                       , string message
                       , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = message;
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void forward(GenericConnection& in, GenericConnection& out, asio::yield_context yield)
{
    sys::error_code ec;
    array<uint8_t, 2048> data;

    for (;;) {
        size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
        if (ec) break;

        asio::async_write(out, asio::buffer(data, length), yield[ec]);
        if (ec) break;
    }
}

//------------------------------------------------------------------------------
static
void handle_connect_request( const shared_ptr<GenericConnection>& client_c
                           , const Request& req
                           , asio::yield_context yield)
{
    sys::error_code ec;
    asio::io_service& ios = client_c->get_io_service();

    auto origin_c = connect_to_host(ios, req["host"], ec, yield);
    if (ec) return fail(ec, "connect");

    http::response<http::empty_body> res{http::status::ok, req.version()};

    // Send the client an OK message indicating that the tunnel
    // has been established. TODO: Reply with an error otherwise.
    http::async_write(*client_c, res, yield[ec]);
    if (ec) return fail(ec, "sending connect response");

    struct State {
        shared_ptr<GenericConnection> client_c, origin_c;
    };

    auto s = make_shared<State>(State{client_c, move(origin_c)});

    asio::spawn
        ( yield
        , [s](asio::yield_context yield) {
              forward(*s->client_c, *s->origin_c, yield);
          });

    asio::spawn
        ( yield
        , [s](asio::yield_context yield) {
              forward(*s->origin_c, *s->client_c, yield);
          });
}

//------------------------------------------------------------------------------
static bool is_front_end_request(const Request& req)
{
    auto host = req["Host"].to_string();

    if (host.substr(0, sizeof("localhost")) != "localhost") {
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
static void serve_request( shared_ptr<GenericConnection> con
                         , string injector
                         , shared_ptr<ipfs_cache::Client> cache_client
                         , shared_ptr<ClientFrontEnd> front_end
                         , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(*con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        if (req.method() == http::verb::connect) {
            return handle_connect_request(con, req, yield);
        }

        if (is_front_end_request(req)) {
            return front_end->serve(*con, req, cache_client, yield);
        }

        // TODO: We're not handling HEAD requests correctly.
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return handle_bad_request(*con, req, "Bad request", yield);
        }

        if (cache_client && front_end->is_ipfs_cache_enabled()) {
            // Get the content from cache
            auto key = req.target();

            string content = cache_client->get_content(key.to_string(), yield[ec]);

            if (!ec) {
                asio::async_write(*con, asio::buffer(content), yield[ec]);
                if (ec) return fail(ec, "async_write");
                continue;
            }

            if (ec != ipfs_cache::error::key_not_found) {
                cout << "Failed to fetch from DB " << ec.message()
                     << " " << req.target() << endl;
            }
        }

        if (!front_end->is_injector_proxying_enabled()) {
            return handle_bad_request(*con , req , "Not cached" , yield);
        }

        // Forward the request to the injector
        auto res = fetch_http_page(con->get_io_service(), injector, req, ec, yield);
        if (ec) return fail(ec, "fetch_http_page");

        // Forward back the response
        http::async_write(*con, res, yield[ec]);
        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "write");
    }

}

//------------------------------------------------------------------------------
static void async_sleep( asio::io_service& ios
                       , asio::steady_timer::duration duration
                       , asio::yield_context yield)
{
    asio::steady_timer timer(ios);
    timer.expires_from_now(duration);
    sys::error_code ec;
    timer.async_wait(yield[ec]);
}

//------------------------------------------------------------------------------
void do_listen( asio::io_service& ios
              , tcp::endpoint local_endpoint
              , string injector
              , string ipns
              , asio::yield_context yield)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    shared_ptr<ipfs_cache::Client> ipfs_cache_client;

    if (ipns.size()) {
        ipfs_cache_client = make_shared<ipfs_cache::Client>(ios, ipns, REPO_ROOT + "/ipfs");
    }

    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    auto front_end = make_shared<ClientFrontEnd>();

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
            async_sleep(ios, chrono::seconds(1), yield);
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , ipfs_cache_client
                         , injector
                         , front_end
                         ](asio::yield_context yield) mutable {
                             using Con = GenericConnectionImpl<tcp::socket>;
                             auto con = make_shared<Con>(move(s));

                             serve_request( con
                                          , move(injector)
                                          , move(ipfs_cache_client)
                                          , move(front_end)
                                          , yield);

                             sys::error_code ec;
                             con->get_impl().shutdown(tcp::socket::shutdown_send, ec);
                         });
        }
    }
}

//------------------------------------------------------------------------------
#include <sys/resource.h>
// Temporary, until this is merged https://github.com/ipfs/go-ipfs/pull/4288
// into IPFS.
void bump_file_limit(rlim_t new_value)
{
   struct rlimit rl;

   int r = getrlimit(RLIMIT_NOFILE, &rl);

   if (r != 0) {
       cerr << "Failed to get the current RLIMIT_NOFILE value" << endl;
       return;
   }

   cout << "Default RLIMIT_NOFILE value is: " << rl.rlim_cur << endl;

   if (rl.rlim_cur >= new_value) {
       cout << "Leaving RLIMIT_NOFILE value unchanged." << endl;
       return;
   }

   rl.rlim_cur = new_value;

   r = setrlimit(RLIMIT_NOFILE, &rl);

   if (r != 0) {
       cerr << "Failed to set the RLIMIT_NOFILE value to " << rl.rlim_cur << endl;
       return;
   }

   r = getrlimit(RLIMIT_NOFILE, &rl);
   assert(r == 0);
   cout << "RLIMIT_NOFILE value changed to: " << rl.rlim_cur << endl;
}

//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    namespace po = boost::program_options;

    po::options_description desc("\nOptions");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen")
        ("injector-tcp", po::value<string>(), "Injector's IP:PORT endpoint")
        ("injector-ipns"
         , po::value<string>()->default_value("")
         , "IPNS of the injector's database")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = vm["repo"].as<string>();

    ifstream ouinet_conf(REPO_ROOT + "/ouinet.conf");

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        bump_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (!vm.count("listen-on-tcp")) {
        cerr << "The parameter 'listen-on-tcp' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    if (!vm.count("injector-tcp")) {
        cerr << "The parameter 'injector-tcp' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    auto const local_ep = util::parse_endpoint(vm["listen-on-tcp"].as<string>());
    auto const injector = vm["injector-tcp"].as<string>();

    string ipns;

    if (vm.count("injector-ipns")) {
        ipns = vm["injector-ipns"].as<string>();
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {
              do_listen( ios
                       , local_ep
                       , injector
                       , ipns
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
