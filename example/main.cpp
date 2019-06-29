#include <iostream>
#include "mcurl.h"

using namespace std;

void signal_cb(ev::sig &w, int)
{
    cout << "caught signal " << w.signum << endl;
    w.loop.break_loop();
}

int main(int argc, char *argv[])
{
    MCurl sender;
    sender.onSuccess([](
                     shared_ptr<MCurl::UserData> &,
                     string &,
                     vector<string> &,
                     string &response,
                     string &response_header,
                     int reply_code)
    {
        cout << "response " << reply_code << endl
             << response_header << endl
             << response << endl;
    });

    sender.onFailue([](shared_ptr<MCurl::UserData> &,
                    string &,
                    vector<string>&,
                    const char *error,
                    long status)
    {
        cout << "error " << error << endl
             << "status " << status << endl;
    });

    ev::default_loop loop;
    ev::sig term_signal_watcher;
    term_signal_watcher.set(loop.raw_loop);
    term_signal_watcher.set<&signal_cb>();
    term_signal_watcher.start(SIGTERM);
    term_signal_watcher.loop.unref();

    ev::sig int_signal_watcher;
    int_signal_watcher.set(loop.raw_loop);
    int_signal_watcher.set<&signal_cb>();
    int_signal_watcher.start(SIGINT);
    int_signal_watcher.loop.unref();

    size_t reqCount = 100;
    if (argc > 1)
    {
        auto tmp = strtol(argv[1], nullptr, 10);
        if (tmp > 0)
            reqCount = static_cast<size_t>(tmp);
    }

    for (int i = 0; i < reqCount; ++i)
        sender.enqueue({nullptr, "https://httpbin.org/post", {}, "request=" + to_string(i)});

    sender.start(true, loop.raw_loop);
    loop.run(0);
    cout << "mcurl loop stopped" << endl;
    return 0;
}
