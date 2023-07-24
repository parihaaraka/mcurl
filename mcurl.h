#ifndef MCURL_H
#define MCURL_H

#include <curl/curl.h>
#include <queue>
#include <ev.h>
#include <map>
#include <variant>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <atomic>
#include <fstream>
#include "b64/encode.h"

/*
 * curl multi interface code snippets: https://curl.haxx.se/libcurl/c/evhiperfifo.html
 *
 * GET testing (pictures): http://dummyimage.com/100x100&text=test1
 * another one: https://postman-echo.com
 *
 * TODO:
 * improve certificates usage
 *
 * Reasons for abandoning the idea of thread_local transport without a wrapper object:
 * 1) difficulty in understanding by the user leading to unexpected multi-interface
 * instances in different threads;
 * 2) curl_global_cleanup() call inconvenience;
 * 3) possible confusion with passing correct ev_loop* when starting mcurl.
 */

struct ci_comparator
{
    bool operator()(const std::string &a, const std::string &b) const;
};

struct sock_info;

class mcurl
{
public:
    mcurl(const mcurl&) = delete;
    mcurl& operator=(const mcurl&) = delete;
    mcurl();
    ~mcurl();

    struct content_part
    {
        // SMTP has always Content-Transfer-Encoding: base64

        // https://developer.mozilla.org/ru/docs/Web/HTTP/Headers/Content-Disposition
        enum class disposition_t { Inline, FormData, Attachment };

        // file attachments for POST-requests are not implemented (TODO?)
        enum class source_type_t  { Buffer, File };

        static content_part file(std::string file_path, std::string destination_file_name = std::string());
        static content_part file_from_buffer(std::string buffer, std::string destination_file_name = std::string());
        static content_part form_data(std::string buffer, std::string name);

        source_type_t source_type = source_type_t::File;
        disposition_t disposition = disposition_t::Attachment;
        std::string source;         ///< содержимое вложения или поля формы
        std::string name;           ///< поле name заголовка Content-Disposition
        std::string filename;       ///< поле filename заголовка Content-Disposition
        std::string content_type = "application/octet-stream";   ///< mime-type, encoding* (тело заголовка Content-Type)
    };

private:
    struct request
    {
        request(){}           ///< compiler bug workaround (https://stackoverflow.com/questions/53408962/try-to-understand-compiler-error-message-default-member-initializer-required-be)
        std::string url;      ///< url c обязательным указанием протокола (http[s]://, smtp://)
        std::string user;     ///< имя пользователя для аутенфикации на сервере
        std::string password; ///< пароль для аутенфикации на сервере
        std::vector<std::string> headers;
        std::string cert;
        std::string ca;
        std::string proxy;    ///< url прокси-сервера, например: socks5://51.15.45.8:1080
        bool verify_peer = true;
    };

public:
    struct http_request : public request
    {
        std::string method;   ///< HTTP method (GET by default)
        std::variant<std::string, std::vector<content_part>> body;
    };

    struct smtp_request : public request
    {
        std::string body;
        std::vector<content_part> attachments;
        std::string sender;   ///< адрес отправителя письма в угловых скобках
        // поддерживаются только готовые для заголовка smtp адреса
        // без подписи (имя владельца) и в угловых скобках
        // (не проверяется правильность, не кодируются возможные подписи / TODO ?)
        std::vector<std::string> recipients; ///< адреса получателей письма в угловых скобках
        std::string subject;  ///< тема письма
    };

    struct response
    {
        long status = 0; ///< HTTP, FTP, SMTP or LDAP (OpenLDAP only) response code
        std::map<std::string, std::string, ci_comparator> headers{ci_comparator()};
        std::string body;
        std::string error = std::string(CURL_ERROR_SIZE, '\0');
    };

    /// the function must be called on the loop's thread
    void start(struct ev_loop *loop = nullptr);

    // thread-safe functions
    /// on_finish runs in the loop's (mcurl) thread!
    void enqueue(http_request &r, std::function<void(mcurl::http_request &, mcurl::response &)> on_finish);
    /// on_finish runs in the loop's (mcurl) thread!
    void enqueue(smtp_request &r, std::function<void(mcurl::smtp_request &, mcurl::response &)> on_finish);
    void set_max_simultaneous_transfers(size_t num);
    /// Stop processing new requests (they get stuck in internal queue). Active requests will be finished.
    void stop(std::function<void()> on_empty_queue = nullptr) noexcept;
    /// Terminate data transferring, stop processing new requests and clear all active requests.
    void terminate(std::function<void()> on_empty_queue = nullptr) noexcept;
    bool is_active() const;
    size_t queue_size() const;
    size_t running_tasks_count() const;

    static std::string timestamp();
    /// text encoding in accordance with https://tools.ietf.org/html/rfc1522
    /// (mail body, senders etc. with non-ascii characters must be encoded)
    static std::string encode1522(const std::string &value, bool wrap = false);
    static std::string generate_boundary(size_t length = 48);

private:
    using mime_t = std::unique_ptr<curl_mime, decltype(&curl_mime_free)>;
    using slist_t = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

    struct http_state
    {
        http_state();
        mime_t mime;
        slist_t curl_header;
    };

    struct smtp_state
    {
        smtp_state();
        enum class stage_t { None, Header, Body, PartHeader, PartBody, Footer };
        stage_t stage = stage_t::None;
        slist_t curl_header;
        std::string boundary;
        size_t bytes_done = 0;
        long partnum;     // index of attachment being processed
        std::string data; // transport buffer
        std::vector<std::string> generated_headers;
        std::unique_ptr<std::ifstream> in_stream; // attachments reading stream (gcc < 5 unable to move a stream)
        base64::encoder b64encoder = base64::CRLF;
        mcurl::slist_t curl_recipients;
        static size_t read_cb(char *buffer, size_t size, size_t nitems, void *user_ptr);
    };

    using request_t = std::variant<http_request, smtp_request>;
    using state_t = std::variant<http_state, smtp_state>;
    struct job
    {
        using feed_t = std::function<void(CURL *easy, job &r)>;
        using on_finish_t = std::function<void(request_t &r, response &resp)>;

        job(request_t &r, feed_t f1, on_finish_t f2);
        job(const job&) = delete;
        job& operator=(const job&) = delete;
        job(job&& other) = default;
        job& operator=(job&&) = default;

        request_t req;
        state_t state;

        response resp;
        std::string prev_header; // helper to deal with folded headers (https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)

        feed_t feed; ///< callback to feed high-level request into libcurl
        on_finish_t on_finish;
        void call_on_finish();
    };

    static std::atomic<int> instances_counter;
    static std::mutex counter_guard;

    /// mcurl's event loop handle
    struct ev_loop *loop = nullptr;
    CURLM *multi = nullptr;
    // man: "After each single curl_easy_perform operation, libcurl will keep the connection alive and open.
    // A subsequent request using the same easy handle to the same host might just be able to use the already
    // open connection! This reduces network impact a lot.
    // ... Each easy handle will attempt to keep the last few connections (default:5)
    // alive for a while in case they are to be used again.
    std::deque<CURL*> free_easy_handles;

    /// Callback to notify a caller on all requests completed. Cleared after call.
    std::function<void()> on_empty_queue;
    std::mutex on_empty_queue_guard;
    void call_empty_queue_cb();

    std::atomic_size_t max_simultanous_transfers = 10;
    std::queue<job> in_queue{};
    mutable std::mutex queue_guard;
    std::map<CURL*, job> on_the_go;

    std::atomic_bool active = false;
    std::atomic_char cmd = '\0';
    // raw pointers for the sake of convenient direct usage in libev api and to keep mcurl movable
    ev_async *cmd_watcher = nullptr;
    ev_async *new_job_watcher = nullptr;
    ev_timer *timeout_watcher = nullptr;

    friend void check_multi_info(mcurl &c) noexcept;
    friend void cmd_cb(struct ev_loop *, ev_async *, int) noexcept;
    friend void new_job_cb(struct ev_loop *, ev_async *w, int);
    friend void timer_cb(struct ev_loop *, ev_timer *w, int);
    friend int socket_cb(CURL *easy, curl_socket_t s, int what, void *, sock_info *si);
    friend void event_cb(struct ev_loop *, ev_io *w, int revents);
    friend int multi_timer_cb(CURLM *m, long timeout_ms, void *user_ptr);
    template <typename R>
    friend void set_common_curl_options(CURL *easy, mcurl::job &j);
    friend size_t write_headers_cb(void *input, size_t size, size_t nmemb, void *user_ptr);
}; // class mcurl

#endif // MCURL_H
