#include "boost_asio.h"
#include "connection.h"
#include "parser.h"
#include "request.h"
#include "response.h"
#include "service.h"
#include "stream.h"
#include "utils.h"

#include <thread>

namespace crequests {


    namespace {

        template <class StreamBufT>
        headers_t parse_headers(StreamBufT&& response_buf) {
            std::istream response_stream(&response_buf);
            headers_t headers;

            string_t header;
            while (std::getline(response_stream, header) && header != "\r") {
                const auto ind = header.find(":");
                if (ind != string_t::npos)
                    headers.emplace(
                        trim(header.substr(0, ind)),
                        trim(header.substr(ind + 2))
                    );
            }

            return headers;
        }

        template <class T>
        optional_t<std::size_t> parse_chunk_length(T&& chunkHeader) {
            try {
                return std::stoull(std::forward<T>(chunkHeader), 0, 16);
            } catch (...) {
                return boost::none;
            }
        }

        template <class StreamBufT>
        string_t read_buffer(StreamBufT& response_buf, const std::size_t length) {
            if (length == 0)
                return "";

            vector_t<char> buf(length);
            response_buf.sgetn(&buf[0], length);

            return string_t(buf.begin(), buf.end());
        }

        template <class StreamBufT>
        bool consume_crlf(StreamBufT& buf) {
            std::istream in(&buf);
            char c {};

            return in.get(c) and c == '\r' and in.get(c) and c == '\n';
        }

        template <class StreamBufT>
        optional_t<size_t> peek_crlf(StreamBufT& sbuf) {
            const auto begin = boost::asio::buffers_begin(sbuf.data());
            const auto end = boost::asio::buffers_end(sbuf.data());

            for (auto it = begin; it != end; ++it)
                if (*it == '\r' && (it + 1) != end && *(it + 1) == '\n')
                    return std::distance(begin, it);

            return boost::none;
        }

        template <class ErrorT>
        bool is_socket_closed(const ErrorT& ec) {
            return
                ec == boost::asio::error::eof or
                ec == boost::asio::error::connection_reset or
                ec == boost::asio::error::connection_aborted or
                ec == boost::asio::error::broken_pipe or
                ec == boost::asio::ssl::error::stream_truncated;
        }

        template <class ErrorT>
        bool is_eof(const ErrorT& ec) {
            return ec and
                (ec == boost::asio::error::eof or
                 ec.value() == boost::asio::ssl::error::stream_truncated);
        }

        bool is_redirect_code(const status_code_t& code) {
            return
                code == status_code_t(301) or
                code == status_code_t(302) or
                code == status_code_t(303);
        }

        bool is_redirect_exhausted(const response_t& response) {
            return
                response.redirect_count().value() >=
                response.request().redirect_count().value();
        }


    } /* anonymous namespace */


    /************************************************************
     * conn_impl_t implementation.
    ************************************************************/


    class conn_impl_t : public std::enable_shared_from_this<conn_impl_t> {
    public:
        /*
          For creating a new connection you need aservice instance
          which responsible for async handling asio jobs and a
          request, which is needed for an instantiation of response object.
         */
        conn_impl_t(service_t& service,
                    const request_t& request);
        
        /*
          This constructor is used for reuse created early connections.
          For example, if you enable keep alive and current connection was
          closed unexpectedly. This allow you to use connection settings for
          new connection.
         */
        conn_impl_t(service_t& service,
                    const request_t& request,
                    const connection_t& connection);
        
        conn_impl_t(const conn_impl_t& conn_impl) = delete;
        conn_impl_t& operator=(const conn_impl_t& conn_impl) = delete;
        ~conn_impl_t();

    public:
        /*
          This function starts an asynchronous connection.
          This connection will ends up in a background process.
        */
        void start();

        /*
          This function stops current and starts a new asynchronous connection.
          This connection will ends up in a background process.
        */
        void restart();
        
        /*
          Function which gives us an object for the future response.
          This response can be obtained when the current connection
          is done (good response or an error on any step, does not matter).
        */
        future_t<response_t> get() const;

        /*
          This function say us that the current connection is expired.
          This means the current connection ends up + waited dispose
          timeout and removed and no longer live. You must fetch response
          before this timeout expired.
        */
        bool is_expired() const;

    private:
        /*
          This functions starts resolving process.
          This process try to understand ip address of the
          destination domain name.
         */
        void resolve();

        /*
          This function starts when resolving process ends up
          and give us an endpoint of destination.
          The process may ends up with an error.
         */
        void on_resolve(const ec_t& ec,
                        const resolver_iterator_t& endpoint);

        /*
          This function need for start a connection process
          to the destination address.
         */
        void connect(const resolver_t::iterator& endpoint);

        /*
          This function starts when connection complete.
          The process may ends up with an error.
         */
        void on_connect(const ec_t& ec,
                        const resolver_iterator_t& endpoint);

        /*
          This function starts if SSL enabled after connect and do
          handshake process of the ssl protocol using OpenSSL library.
         */
        void handshake();

        /*
          This function starts when ssl connection complete.
          The process may ends up with an error.
         */
        void on_handshake(const ec_t& ec);

        /*
          This function try to write HTTP request data (such as
          method, uri, params, headers, cookies, post data body)
          to the opened socket connection after connect (or
          handshake if SSL is enabled).
         */
        void write();

        /*
          This function starts when writing process is done.
          The process may ends up with an error.
         */
        void on_write(const ec_t& ec, const std::size_t&);

        /*
          This function starts after writing process and trying to
          read status of the incoming HTTP response from remote
          server.
         */
        void read_status();

        /*
          This function starts when read status process is done.
          The process may ends up with an error.
         */
        void on_read_status(const ec_t& ec, const std::size_t&);

        /*
          This function starts after read status process and trying to
          read HTTP headers of the remote server response body.
         */
        void read_headers();

        /*
          This function starts when headers reading is done.
          The process may ends up with an error.
         */
        void on_read_headers(const ec_t& ec, const std::size_t&);

        /*
          This function is responsible for reading content of the remote
          server response. Content can be readed in several ways.
          It's can be content length enabled in headers. So we are reading
          precise length of the body.
          Or it can be chunk encoding response. It is a response which remote
          server send to us part by part of fixed size.
          In other way we must use fallback which read body data until it is an end.
         */
        void read_content();

        /*
          This fuction read body of fixed size according to content length in headers.
         */
        void read_content_length();

        /*
          This function starts when reading body is done.
          The process may ends up with an error.
         */
        void on_read_content_length(const ec_t& ec,
                                    const std::size_t length);

        /*
          This function read header of chunk encoding protocol.
          This header contains length of the next data after it.
         */
        void read_chunk_header();

        /*
          This function reads chunk data of the chunk encoding protocol.
          If remote server has another data for us then header-data process
          will repeat.
         */
        void read_chunk_data();
        
        /*
          This function starts when reading chunk header is done.
          The process may ends up with an error.
         */
        void on_read_chunk_header(const ec_t& ec,
                                  const std::size_t);

        /*
          This function starts when reading chunk data is done.
          The process may ends up with an error.
         */
        void on_read_chunk_data(const ec_t& ec,
                                const std::size_t);

        /*
          If it is not exists content length or chunk data we try to read
          remote server response until EOF in the socket.
         */
        void read_until_eof();

        /*
          This function starts when reading some data portion from socket is done.
          The process may ends up with an error.
         */
        void on_read_until_eof(const ec_t& ec, const std::size_t length);

        /*
          This function always setup timeout of connection.
          This timeout is set in request data.
         */
        void setup_timeout();

        /*
          This function starts when time is out and set state accordingly.
         */
        void on_timeout(const ec_t& ec);

        /*
          This functions setup timeout for final response (with an error or not).
          When this timeout is expired response state will be expired and this
          connection will be desctructed because nobody is needed for it.
         */
        void setup_dispose_timer();

        /*
          No one obtain this response so it is can be destructed.
         */
        void on_dispose_timer(const ec_t& ec);

        /*
          This function called when HTTP response code say us the url was moved.
          If redirect function is set up mechanism will do redirects.
          This redirects can be configured by redirects count in request settings.
         */
        void perform_redirect();

        /*
          Functions for working with states of connection mechanism.
         */
        void set_error(const error_code_t& new_state, const string_t& msg);
        void set_error(const error_code_t& new_state, const ec_t& ec);
        void set_success();
        void set_timeout();
        void set_dispose();
        void set_state(const error_code_t& state_);
        bool in_final_state() const;

        /*
          This function called when we want to canceland close all operations of
          all connection objects.
         */
        void end();

        /*
          This function returns true if this connection reuse previous connection.
          This behaviour can be done when keep-alive is enabled and we want to
          reconnect to server with existing settings of inital connection.
         */
        bool is_reused() const;

        /*
          Set up all neccessary parameters and callbacks for http parser.
         */
        void prepare_parser();

        /*
          Start parser which will consume some data read from socket and do parsing
          of http response or part of http response.
          Can be called several times due to obtain remote server data from socket.
         */
        bool execute_parser();

    public:
        service_t& service;
        strand_t strand;
        stream_t stream;
        resolver_t resolver;
        timer__t timeout_timer;
        timer__t dispose_timer;
        promise_t<response_t> promise;
        future_t<response_t> future;
        response_t response;
        bool m_is_reused;
        error_code_t state;

        streambuf_t request_buf;
        streambuf_t response_buf;

        parser_t* parser {nullptr};

        string_t header_field;
        size_t content_length {0};
        raw_t raw;
        headers_t headers;
    };

    conn_impl_t::conn_impl_t(service_t& service_, const request_t& request_)
        : service(service_),
          strand(service.get_service()),
          stream(service.get_service(), request_),
          resolver(service.get_service()),
          timeout_timer(service.get_service()),
          dispose_timer(service.get_service()),
          promise(),
          future{promise.get_future()},
          response(request_),
          m_is_reused(false),
          state{error_code_t::INIT},
          request_buf{},
          response_buf{},
          parser{new parser_t(parser_t::parser_type_t::RESPONSE)},
          header_field{},
          content_length{},
          raw{},
          headers{}
    {

    }

    conn_impl_t::conn_impl_t(service_t& service_,
                             const request_t& request_,
                             const connection_t& connection)
        : service(service_),
          strand(service.get_service()),
          stream(std::move(connection.pimpl->stream)),
          resolver(service.get_service()),
          timeout_timer(service.get_service()),
          dispose_timer(service.get_service()),
          promise(),
          future{promise.get_future()},
          response(request_),
          m_is_reused(true),
          state{error_code_t::INIT},
          request_buf{},
          response_buf{},
          parser{new parser_t(parser_t::parser_type_t::RESPONSE)},
          header_field{},
          content_length{},
          raw{},
          headers{}
    {
        response.redirects(connection.get().get().redirects());
    }

    conn_impl_t::~conn_impl_t()
    {
        if (parser) {
            delete parser;
            parser = nullptr;
        }
    }


    /************************************************************
     * conn_impl_t main functionality.
    ************************************************************/

    bool conn_impl_t::execute_parser() {
        const auto data = boost::asio::buffer_cast<const char*>(response_buf.data());
        const auto nparsed = parser->execute(data, response_buf.size());
        response_buf.consume(nparsed);
        parser->unpause();

        return nparsed > 0;
    }

    void conn_impl_t::prepare_parser() {
        raw = ""_raw;
        header_field = "";
        content_length = 0;
        headers = ""_headers;

        const auto status_fn = [this](const char* at,
                                      const size_t length,
                                      const unsigned short major,
                                      const unsigned short minor,
                                      const unsigned int code)
        {
            response.http_major(http_major_t{major});
            response.http_minor(http_minor_t{minor});
            response.status_code(status_code_t{code});
            response.status_message(status_message_t{string_t(at, length)});
            parser->pause();
        };

        parser->bind_cb(status_fn);

        const auto header_field_fn = [this](const char* at, const size_t length)
        {
            header_field.reserve(length);
            header_field.assign(at, length);
        };

        parser->bind_cb(parser_t::HEADER_FIELD, header_field_fn);

        const auto header_value_fn = [this](const char* at, const size_t length)
        {
            string_t header_value(at, length);
            if (tolower(header_field) == "set-cookie") {
                auto cookie = cookie_t::from_string(header_value);
                cookie.origin_domain(response.request().uri().domain().value());
                cookie.origin_path(response.request().uri().path().value());
                response.cookies().add(std::move(cookie));
            }
            headers.insert(header_field, std::move(header_value));
            header_field.clear();
        };

        parser->bind_cb(parser_t::HEADER_VALUE, header_value_fn);

        const auto headers_complete_fn = [this](const ssize_t content_len)
        {
            content_length = content_len;
            response.headers(std::move(headers));
            parser->pause();
        };

        parser->bind_cb(parser_t::HEADERS_COMPLETE, headers_complete_fn);

        const auto body_fn = [this](const char* at, const size_t length)
        {
            if (response.request().body_callback())
                response.request().body_callback()(at, length, error_t{});
            else
                raw.value().append(at, length);
            parser->pause();
        };

        parser->bind_cb(parser_t::BODY, body_fn);

        const auto chunk_header_fn = [this](const size_t length)
        {
            content_length = length;
            parser->pause();
        };

        parser->bind_cb(parser_t::CHUNK_HEADER, chunk_header_fn);
    }

    /*
      Function which gives us an object for the future response.
      This response can be obtained when the current connection
      is done (good response or an error on any step, does not matter).
    */
    future_t<response_t> conn_impl_t::get() const {
        return future;
    }

    void conn_impl_t::start() {
        prepare_parser();

        if (is_reused()) {
            if (stream.is_open())
                write();
            else
                restart();
        }
        else {
            resolve();
        }

        setup_timeout();
    }

    void conn_impl_t::restart() {
        stream.cancel();
        stream = stream_t(service.get_service(), response.request());
        if (parser) {
            delete parser;
            parser = nullptr;
        }
        parser = new parser_t(parser_t::parser_type_t::RESPONSE);
        m_is_reused = false;
        start();
    }

    void conn_impl_t::setup_timeout() {
        timeout_timer.expires_from_now(
            seconds_t(response.request().timeout().value()));
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec) {
            on_timeout(ec);
        };
        timeout_timer.async_wait(strand.wrap(callback));
    }

    void conn_impl_t::on_timeout(const ec_t& ec) {
        if (not ec)
            set_timeout();
    }

    void conn_impl_t::setup_dispose_timer() {
        dispose_timer.expires_from_now(
            seconds_t(response.request().store_timeout().value()));
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec) {
            on_dispose_timer(ec);
        };
        dispose_timer.async_wait(strand.wrap(callback));
    }

    void conn_impl_t::on_dispose_timer(const ec_t& ec) {
        if (not ec)
            set_dispose();
    }

    void conn_impl_t::resolve() {
        const resolver_t::query query {
            response.request().uri().domain().value(),
            response.request().uri().port().value()
        };
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec,
                                           const resolver_t::iterator& endpoint) {
            on_resolve(ec, endpoint);
        };
        set_state(error_code_t::RESOLVE);
        resolver.async_resolve(query, callback);
    }

    void conn_impl_t::on_resolve(const ec_t& ec,
                                 const resolver_t::iterator& endpoint) {
        if (ec) {
            set_error(error_code_t::RESOLVE_ERROR, ec);
            return;
        }

        connect(endpoint);
    }

    void conn_impl_t::connect(const resolver_t::iterator& endpoint) {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec,
                                           const resolver_t::iterator& endpoint_) {
            on_connect(ec, endpoint_);
        };
        set_state(error_code_t::CONNECT);
        stream.async_connect(endpoint, strand.wrap(callback));
    }

    void conn_impl_t::on_connect(const ec_t& ec,
                                 const resolver_t::iterator&) {
        if (ec) {
            set_error(error_code_t::CONNECT_ERROR, ec);
            return;
        }

        if (response.request().keep_alive())
            stream.set_option(boost::asio::socket_base::keep_alive { true });
        handshake();
    }

    void conn_impl_t::handshake() {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec) {
            on_handshake(ec);
        };
        set_state(error_code_t::HANDSHAKE);
        stream.async_handshake(strand.wrap(callback));
    }

    void conn_impl_t::on_handshake(const ec_t& ec) {
        if (ec) {
            set_error(error_code_t::HANDSHAKE_ERROR, ec);
            return;
        }

        write();
    }

    void conn_impl_t::write() {
        std::ostream request_stream(&request_buf);
        request_stream << response.request().make_request();
        request_stream.flush();

        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t length) {
            on_write(ec, length);
        };
        set_state(error_code_t::WRITE);
        stream.async_write(request_buf, strand.wrap(callback));
    }

    void conn_impl_t::on_write(const ec_t& ec, const std::size_t&) {
        if (ec) {
            if (is_socket_closed(ec) and is_reused() and not in_final_state()) {
                restart();
            }
            else {
                set_error(error_code_t::WRITE_ERROR, ec);
            }
            return;
        }

        read_status();
    }

    void conn_impl_t::read_status() {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t& length) {
            on_read_status(ec, length);
        };
        set_state(error_code_t::READ_STATUS);
        stream.async_read_until(response_buf, "\r\n", strand.wrap(callback));
    }

    void conn_impl_t::on_read_status(const ec_t& ec, const std::size_t&) {
        if (ec) {
            if (is_socket_closed(ec) and is_reused() and not in_final_state()) {
                restart();
            }
            else {
                set_error(error_code_t::READ_STATUS_ERROR, ec);
            }
            return;
        }

        if (not execute_parser()) {
            set_error(error_code_t::READ_STATUS_DATA_ERROR, "bad status data");
            return;
        }

        read_headers();
    }

    void conn_impl_t::read_headers() {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t& length) {
            on_read_headers(ec, length);
        };
        set_state(error_code_t::READ_HEADERS);
        stream.async_read_until(response_buf, "\r\n\r\n", strand.wrap(callback));
    }

    void conn_impl_t::on_read_headers(const ec_t& ec, const std::size_t&) {
        if (ec and not is_eof(ec)) {
            set_error(error_code_t::READ_HEADERS_ERROR, ec);
            return;
        }

        if (response_buf.size() == 0)
            set_error(error_code_t::READ_HEADERS_ERROR, "no headers");

        if (not execute_parser()) {
            set_error(error_code_t::READ_HEADERS_ERROR, "bad headers data");
            return;
        }

        read_content();
    }

    void conn_impl_t::read_content() {
        if (response.headers().count("Content-Length"))
            read_content_length();
        else if (response.headers().contains("Transfer-Encoding", "chunked"))
            read_chunk_header();
        else
            read_until_eof();
    }

    void conn_impl_t::read_content_length() {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t length) {
            on_read_content_length(ec, length);
        };
        set_state(error_code_t::READ_CONTENT_LENGTH);
        const size_t n = response_buf.size() > content_length
            ? 0
            : content_length - response_buf.size();
        stream.async_read(response_buf,
                          boost::asio::transfer_at_least(n),
                          strand.wrap(callback));
    }

    void conn_impl_t::on_read_content_length(const ec_t& ec, const std::size_t) {
        if ((ec and not is_eof(ec)) or
            (ec and is_eof(ec) and response_buf.size() < content_length))
        {
            set_error(error_code_t::READ_CONTENT_LENGTH_ERROR, ec);
        }
        else {
            raw.value().reserve(content_length);
            if (not execute_parser()) {
                set_error(error_code_t::READ_CONTENT_LENGTH_ERROR, "bad content length");
                return;
            }
            set_success();
        }
    }

    void conn_impl_t::read_chunk_header() {
        set_state(error_code_t::READ_CHUNK_HEADER);

        const auto crlf = peek_crlf(response_buf);
        if (crlf) {
            on_read_chunk_header(ec_t(), 0);
            return;
        }

        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t length) {
            on_read_chunk_header(ec, length);
        };

        stream.async_read_until(response_buf, "\r\n", strand.wrap(callback));
    }

    void conn_impl_t::on_read_chunk_header(const ec_t& ec, const std::size_t) {
        if (ec) {
            if (not is_eof(ec))
                set_error(error_code_t::READ_CHUNK_HEADER_ERROR, ec);
            else
                set_success();
            return;
        }

        if (not execute_parser()) {
            set_error(error_code_t::READ_CHUNK_HEADER_ERROR,"bad chunk header");
            return;
        }

        if (content_length > 0) {
            if (response_buf.size() > content_length) {
                set_state(error_code_t::READ_CHUNK_DATA);
                on_read_chunk_data(ec_t(), content_length);
            }
            else {
                read_chunk_data();
            }
        }
        else {
            set_success();
        }
    }

    void conn_impl_t::read_chunk_data() {
        set_state(error_code_t::READ_CHUNK_DATA);

        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec,
                                           const std::size_t length) {
            on_read_chunk_data(ec, length);
        };

        stream.async_read(response_buf,
                          boost::asio::transfer_at_least(content_length - response_buf.size()),
                          strand.wrap(callback));
    }

    void conn_impl_t::on_read_chunk_data(const ec_t& ec, const std::size_t) {
        if (ec and not is_eof(ec)) {
            set_error(error_code_t::READ_CHUNK_DATA_ERROR, ec);
            return;
        }
        else if (ec and is_eof(ec) and response_buf.size() < content_length) {
            set_error(error_code_t::READ_CHUNK_DATA_ERROR, ec);
            return;
        }

        raw.value().reserve(response.raw().value().size() + content_length);
        if (not execute_parser()) {
            set_error(error_code_t::READ_CHUNK_DATA_ERROR, "chunk data error");
            return;
        }

        read_chunk_header();
    }

    void conn_impl_t::read_until_eof() {
        const auto self = shared_from_this();
        const auto callback = [this, self](const ec_t& ec, const std::size_t length) {
            on_read_until_eof(ec, length);
        };
        set_state(error_code_t::READ_UNTIL_EOF);
        stream.async_read(response_buf,
                          boost::asio::transfer_at_least(1),
                          strand.wrap(callback));
    }

    void conn_impl_t::on_read_until_eof(const ec_t& ec, const std::size_t) {
        if (ec) {
            if (not is_eof(ec)) {
                set_error(error_code_t::READ_UNTIL_EOF_ERROR, ec);
            }
            else {
                if (not execute_parser()) {
                    set_error(error_code_t::READ_UNTIL_EOF_ERROR, "until eof error");
                    return;
                }
                set_success();
            }
        }
        else {
            read_until_eof();
        }
    }


    /************************************************************
     * conn_impl_t utils.
     ************************************************************/


    bool conn_impl_t::is_expired() const {
        return state == error_code_t::EXPIRED;
    }

    bool conn_impl_t::is_reused() const {
        return m_is_reused;
    }

    void conn_impl_t::end() {
        resolver.cancel();
        timeout_timer.cancel();
        if (response.request().final_callback())
            response.request().final_callback()(response);
        setup_dispose_timer();

        if (response.request().keep_alive()) {
            if (response.headers().contains("Connection", "close")) {
                stream.cancel();
                stream.close();
            }
        }
        else {
            stream.cancel();
        }

        response.raw(std::move(raw));

        if (response.request().body_callback())
            response.request().body_callback()(nullptr, 0, response.error());

        if (response.error() and response.request().throw_on_error())
            promise.set_exception(std::make_exception_ptr(response.error()));
        else
            promise.set_value(response);
    }

    void conn_impl_t::perform_redirect() {
        if (is_redirect_exhausted(response)) {
            set_error(error_code_t::REDIRECT_EXHAUSTED, "redirect exhausted");
            return;
        }

        if (not response.headers().count("Location")) {
            set_error(error_code_t::REDIRECT_ERROR, "no Location.");
            return;
        }

        auto redirects = std::move(response.redirects());

        if (redirects.get().empty()) {
            redirects.add(response);
        }

        auto redirect_count = std::move(response.redirect_count());
        auto request = std::move(response.request());

        redirect_count.value()++;
        request.uri(uri_t::from_string(response.headers().at("Location")));
        request.prepare();

        response = response_t{std::move(request)};
        response.redirect_count(std::move(redirect_count));
        response.redirects(std::move(redirects));

        redirects = response.redirects();
        redirects.add(response);
        response.redirects(std::move(redirects));

        stream = stream_t(service.get_service(), response.request());

        if (request_buf.size() > 0) {
            request_buf.consume(request_buf.size());
        }

        if (response_buf.size() > 0) {
            response_buf.consume(response_buf.size());
        }

        if (parser) {
            delete parser;
            parser = nullptr;
        }
        parser = new parser_t(parser_t::parser_type_t::RESPONSE);
        prepare_parser();

        resolve();
    }

    void conn_impl_t::set_error(const error_code_t& new_state, const string_t& msg) {
        if (in_final_state())
            return;

        set_state(new_state);
        response.error(error_t(new_state, msg));
        end();
    }

    void conn_impl_t::set_error(const error_code_t& new_state, const ec_t& ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        set_error(new_state, ec.message());
    }

    void conn_impl_t::set_success() {
        if (is_redirect_code(response.status_code())) {
            if (response.request().redirect()) {
                perform_redirect();
                return;
            }
        }
        else {
            if (not in_final_state()) {
                set_state(error_code_t::SUCCESS);
                response.error(error_t(state, "success"));
                end();
            }
        }
    }

    void conn_impl_t::set_timeout() {
        if (in_final_state()) {
            if (not response.request().keep_alive())
                stream.close();
            return;
        }

        set_state(error_code_t::TIMEOUT);
        response.error(error_t(state, "timeout"));
        end();
    }

    void conn_impl_t::set_dispose() {
        set_state(error_code_t::EXPIRED);
    }

    void conn_impl_t::set_state(const error_code_t& state_) {
        if (not in_final_state() or state == error_code_t::EXPIRED)
            state = state_;
    }

    bool conn_impl_t::in_final_state() const {
        switch (state) {
        case error_code_t::RESOLVE_ERROR:
        case error_code_t::CONNECT_ERROR:
        case error_code_t::HANDSHAKE_ERROR:
        case error_code_t::WRITE_ERROR:
        case error_code_t::READ_STATUS_ERROR:
        case error_code_t::READ_STATUS_DATA_ERROR:
        case error_code_t::READ_HEADERS_ERROR:
        case error_code_t::READ_CONTENT_LENGTH_ERROR:
        case error_code_t::READ_CHUNK_HEADER_ERROR:
        case error_code_t::READ_CHUNK_DATA_ERROR:
        case error_code_t::READ_UNTIL_EOF_ERROR:
        case error_code_t::REDIRECT_EXHAUSTED:
        case error_code_t::REDIRECT_ERROR:
        case error_code_t::TIMEOUT:
        case error_code_t::EXPIRED:
        case error_code_t::SUCCESS:
            return true;

        case error_code_t::WRITE:
        case error_code_t::READ_STATUS:
        case error_code_t::READ_HEADERS:
        case error_code_t::READ_CONTENT_LENGTH:
        case error_code_t::READ_CHUNK_HEADER:
        case error_code_t::READ_CHUNK_DATA:
        case error_code_t::READ_UNTIL_EOF:
        case error_code_t::INIT:
        case error_code_t::RESOLVE:
        case error_code_t::CONNECT:
        case error_code_t::HANDSHAKE:
            return false;
        }

        return false;
    }


    /************************************************************
     * connection_t implementation.
     ************************************************************/


    connection_t::connection_t(service_t& service, const request_t& request)
        : pimpl(std::make_shared<conn_impl_t>(service, request))
    {

    }

    connection_t::connection_t(service_t& service,
                               const request_t& request,
                               const connection_t& connection)
        : pimpl(std::make_shared<conn_impl_t>(service, request, connection))
    {

    }

    connection_t::~connection_t()
    {

    }

    connection_t::connection_t(connection_t&& connection)
        : pimpl {std::move(connection.pimpl)}
    {
        connection.pimpl = nullptr;
    }

    connection_t& connection_t::operator=(const connection_t& connection) {
        if (this != &connection) {
            pimpl = connection.pimpl;
        }

        return *this;
    }

    connection_t& connection_t::operator=(connection_t&& connection) {
        if (this != &connection) {
            pimpl = connection.pimpl;
            connection.pimpl = nullptr;
        }

        return *this;
    }


    /************************************************************
     * Other functions.
     ************************************************************/


    future_t<response_t> connection_t::get() const {
        return pimpl->get();
    }

    void connection_t::start() {
        pimpl->start();
    }

    bool connection_t::is_expired() const {
        return pimpl->is_expired();
    }


} /* namespace crequests */
