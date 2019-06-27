#ifndef MCURL_H
#define MCURL_H

#include <curl/curl.h>
#include <queue>
#include <ev++.h>
#include <map>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <atomic>
#include <fstream>
#include "b64/encode.h"

/*
 * работа с curl multi interface подсмотрена здесь: https://curl.haxx.se/libcurl/c/evhiperfifo.html
 * дока по libev: http://pod.tst.eu/http://cvs.schmorp.de/libev/ev.pod
 *
 * ресурс для тестирования GET (картинки): http://dummyimage.com/100x100&text=test1
 * ресурс для тестирования любых запросов: https://httpbin.org
 *
 * TODO:
 * облагородить работу с сертификатами;
 * сделать поддержку cppcms в качестве альтернативы libev.
 */


class MCurl
{
public:
    MCurl(struct ev_loop *loop = EV_DEFAULT);
    ~MCurl();

    // базовый класс для пользовательских данных
    class UserData
    {
    public:
        UserData() = default;
        virtual ~UserData() = default;
    };

    class Job
    {
        friend class MCurl;
    public:
        struct ContentPart
        {
            // для SMTP всегда: Content-Transfer-Encoding: base64

            // https://developer.mozilla.org/ru/docs/Web/HTTP/%D0%97%D0%B0%D0%B3%D0%BE%D0%BB%D0%BE%D0%B2%D0%BA%D0%B8/Content-Disposition
            enum class Disposition { Inline, FormData, Attachment };

            // вложения из файлов для POST-запроса не реализованы / TODO ?
            enum class SourceType  { Buffer, File };

            SourceType source_type = SourceType::File;
            Disposition disposition = Disposition::Attachment;
            std::string source;         ///< содержимое вложения или поля формы
            std::string name;           ///< поле name заголовка Content-Disposition
            std::string filename;       ///< поле filename заголовка Content-Disposition
            std::string content_type = "application/octet-stream";   ///< mime-type, encoding* (тело заголовка Content-Type)

            static ContentPart file(std::string file_path, std::string destination_file_name = std::string());
            static ContentPart file_from_buffer(std::string buffer, std::string destination_file_name = std::string());
            static ContentPart form_data(std::string name, std::string buffer);
        };

        Job(const Job&) = delete;
        Job& operator = (const Job&) = delete;
        Job(Job &&) = default;

        Job() = default;
        Job(std::shared_ptr<UserData> udata, const std::string &url, const std::vector<std::string> &header, const char* body, size_t body_size, const std::string &_cert = "", const bool _verify_peer = true, const std::string &ca = "")
            : user_data(udata), url(url), request_header(header), request(body, body_size), cert(_cert), ca(ca), verify_peer(_verify_peer), _b64encoder(base64::CRLF)
        {}
        Job(std::shared_ptr<UserData> udata, const std::string &url, const std::vector<std::string> &header = {}, const std::string &_request = "", const std::string &_cert = "", const bool _verify_peer = true, const std::string &ca = "")
            : user_data(udata), url(url), request_header(header), request(_request), cert(_cert), ca(ca), verify_peer(_verify_peer), _b64encoder(base64::CRLF)
        {}
        Job(std::shared_ptr<UserData> udata, const std::string &url, const std::vector<std::string> &header, const std::vector<ContentPart> &_request_parts, const std::string &_cert = "", const bool _verify_peer = true, const std::string &ca = "")
            : user_data(udata), url(url), request_header(header), request_parts(_request_parts), cert(_cert), ca(ca), verify_peer(_verify_peer), _b64encoder(base64::CRLF)
        {}

        ~Job()
        {
            // задание для curl'а недоступно пользователю после помещения его в очередь методом enqueue(Job &&),
            // не получится снаружи менять поля задания и косвенно воздействовать на внутренние переменные вроде
            // curl_header, поэтому всё дотерпит до деструктора Job
            if (curl_header)
                curl_slist_free_all(curl_header);
            if (curl_recipients)
                curl_slist_free_all(curl_recipients);
            if (formpost)
                curl_formfree(formpost);

        }

        std::shared_ptr<UserData> user_data;
        std::string url;      ///< url c обязательным указанием протокола (http[s]://, smtp://)
        std::string user;     ///< имя пользователя для аутенфикации на SMTP-сервере
        std::string password; ///< пароль для аутенфикации на SMTP-сервере
        std::string sender;   ///< адрес отправителя письма в угловых скобках
        // поддерживаются только готовые для заголовка smtp адреса
        // без подписи (имя владельца) и в угловых скобках
        // (не проверяется правильность, не кодируются возможные подписи / TODO ?)
        std::vector<std::string> recipients; ///< адреса получателей письма в угловых скобках
        std::string subject;  ///< тема письма
        std::vector<std::string> request_header;
        std::string request;  ///< тело POST-запроса или текстовая часть письма
        std::vector<ContentPart> request_parts;
        std::string response_header;
        std::string response;
        std::string cert;
        std::string ca;
        std::string proxy;  ///< url прокси-сервера, например: socks5://51.15.45.8:1080
        bool verify_peer = true;
    private:
        enum class SmtpStage { None, Header, Body, PartHeader, PartBody, Footer };
        char error[CURL_ERROR_SIZE] = {0};
        curl_httppost *formpost = nullptr;
        curl_slist *curl_header = nullptr;
        curl_slist *curl_recipients = nullptr;
        std::string _boundary;

        SmtpStage _stage = SmtpStage::None;
        // размер переданной в curl части буфера, обрабатываемого на текущем шаге (SMTP)
        size_t _bytes_done = 0;
        // индекс обрабатываемого вложения
        long _partnum;
        // буфер данных, отправляемых в curl (SMTP)
        // (перезаполняется по мере передачи различных частей тела сообщения)
        std::string _data;
        // поток для считывания файлов вложений (gcc < 5 не умеет перемещать поток, поэтому указатель)
        std::unique_ptr<std::ifstream> _in_stream;
        base64::encoder _b64encoder = base64::CRLF;
    };

    // потокобезопасные
    void enqueue(Job &&);
    void setMaxSimultaneousTransfers(size_t num) { _max_simultanous_transfers = num; }

    void start(bool terminate_on_finish = false, struct ev_loop *loop = nullptr);

    // потокобезопасная остановка
    void stop();        // прерывание приема новых заданий (текущие выполнятся до конца)
    void terminate();   // прерывание работы

    MCurl& onComplete(const std::function<void(std::shared_ptr<UserData>&)> &callback);
    MCurl& onFailue(const std::function<void(std::shared_ptr<UserData>&, std::string &request, std::vector<std::string> &request_header, const char *error, long status)> &callback);
    MCurl& onSuccess(const std::function<void(std::shared_ptr<UserData>&, std::string &request, std::vector<std::string> &request_header, std::string &response, std::string &response_header, long status)> &callback);

    size_t queueSize() const { std::lock_guard<std::mutex> lk(_locker); return _in_queue.size(); }
    bool isActive() const { return _new_job_watcher.is_active(); }
    size_t runningTasksCount() const { return _on_the_go.size(); }

    static std::string timestamp();
    // кодирование текста согласно https://tools.ietf.org/html/rfc1522
    // (тема письма, имена владельцев e-mail и т.п. должны быть закодированы при наличии не-ASCII символов)
    static std::string encode1522(const std::string &value, bool wrap = false);

    static std::string generateBoundary(size_t length = 48);

private:

    struct SockInfo
    {
        curl_socket_t sock;
        CURL *easy;
        ev::io ev;
    };

    // цикл обработки сообщений, в котором работает текущий экземпляр класса
    struct ev_loop *_loop;

    // очередь задач
    std::queue<Job> _in_queue;
    // мьютекс для защиты очереди задач
    mutable std::mutex _locker;
    // выполняющиеся задания
    // TODO перенести CURL* в Job ?
    std::map<CURL*, Job> _on_the_go;

    CURLM *_multi;
    ev::timer _timeout_timer;
    ev::async _new_job_watcher;

    std::atomic_char _mode_wanted;
    ev::async _stop_signal_watcher;

    // man: "After each single curl_easy_perform operation, libcurl will keep the connection alive and open.
    // A subsequent request using the same easy handle to the same host might just be able to use the already
    // open connection! This reduces network impact a lot.
    // ... Each easy handle will attempt to keep the last few connections (default:5)
    // alive for a while in case they are to be used again.
    std::deque<CURL*> _free_easy_handles;

    // лимит количества одновременных задач
    std::atomic_size_t _max_simultanous_transfers;

    // флаг завершения работы mcurl после выполнения всех заданий из входной очереди
    bool _terminate_on_finish;

    std::function<void(std::shared_ptr<UserData>&, std::string &request, std::vector<std::string> &request_header, std::string &response, std::string &response_header, long status)> _successCallback;
    std::function<void(std::shared_ptr<UserData>&, std::string &request, std::vector<std::string> &request_header, const char *error, long status)> _failureCallback;
    std::function<void(std::shared_ptr<UserData>&)> _completeCallback;

    // обработчик сигнала послупления задания
    // creates a new easy handle and adds it to the global curl_multi
    void new_job_cb(ev::async &, int) noexcept;

    // обработка сигнала на остановку
    void stop_cb(ev::async &, int) noexcept;

    // поиск выполненных заданий и завершение их жизни
    void check_multi_info() noexcept;

    // здесь libev сообщает, что засек движуху на сокете
    // (дальше дергаем curl через функцию curl_multi_socket_action, которая уже просит нас сменить режим в libev)
    // с++ вариант этого callback'а не прокатил - не получилось его прописать в ev_io (шаблонная магия)
    static void event_cb(struct ev_loop *, ev_io *w, int revents);

    // вызывается нами для libcurl по его запросу через переданный нам интервал времени
    void timer_cb(ev::timer &, int);

    // здесь libcurl просит нас дернуть его либо через нужное время, либо сразу
    static int multi_timer_cb(CURLM *, long timeout_ms, MCurl *owner);

    // проверка кода возврата некоторых функций libcurl
    static void mcode_or_die(const std::string &where, CURLMcode code);

    // с помощью этого callback'а curl_multi_socket_action уведомляет нас о смене режима сокета
    static int socket_cb(CURL *easy_handle, curl_socket_t s, int what, MCurl *owner, SockInfo *si);

    // добавление данных (по мере их поступления) в итоговый ответ
    static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *data);

    // callback для получения curl'ом данных для отправки
    static size_t read_cb(char *buffer, size_t size, size_t nitems, void *user_ptr);

};

#endif // MCURL_H
