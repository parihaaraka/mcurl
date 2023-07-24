#include <iostream>
#include <thread>
#include "mcurl.h"

using namespace std;

void signal_cb(struct ev_loop *loop, ev_signal *w, int)
{
    cout << "caught signal " << w->signum << endl;
    ev_break(loop);
}

int main(int argc, char *argv[])
{
    mcurl sender;

    size_t batch_size = 4;
    size_t counter;
    if (argc > 1)
    {
        auto tmp = strtol(argv[1], nullptr, 10);
        if (tmp > 0)
            batch_size = static_cast<size_t>(tmp);
    }

    auto enqueue_batch = [&](std::function<void(size_t &counter)> fin, std::string url = "https://postman-echo.com/post")
    {
        counter = batch_size;
        for (size_t i = 1; i <= batch_size; ++i)
        {
            mcurl::http_request r;
            r.url = url;
            r.body = "request=" + to_string(i);
            sender.enqueue(r, [&counter, i, fin](mcurl::http_request &, mcurl::response &resp){
                cout << "~ req " << i << ": status " << resp.status;
                if (!resp.error.empty())
                    cout << ", error: " << resp.error;
                cout << endl << endl;
                //cout << "------------------ req " << i << endl
                //     << " status: " << resp.status << endl
                //     << " error:  " << resp.error << endl
                //     << " body:   " << resp.body << endl
                //     << " headers:" << endl;
                //for (auto &h: resp.headers)
                //{
                //    cout << "   " << h.first << ": " << h.second;
                //}
                //cout << endl << endl;

                fin(counter);
                --counter;
            });
        }

    };

    struct ev_loop *loop = EV_DEFAULT;
    ev_signal term_signal_watcher;
    ev_signal_init(&term_signal_watcher, &signal_cb, SIGTERM);
    ev_signal_start(loop, &term_signal_watcher);
    ev_unref(loop);

    ev_signal int_signal_watcher;
    ev_signal_init(&int_signal_watcher, &signal_cb, SIGINT);
    ev_signal_start(loop, &int_signal_watcher);
    ev_unref(loop);

    //---------------------------------------------------------------------------------

    // explicit loop break
    enqueue_batch([](size_t &counter){
        if (counter == 1)
            ev_break(EV_DEFAULT);
    });
    sender.start(loop);
    ev_run(loop, 0);
    cout << "explicit loop break after all jobs" << endl << endl;

    // stop() should stop all watchers when active jobs done => natural loop break
    enqueue_batch([&sender, batch_size](size_t &counter){
        // init stop after first successful request
        if (counter == batch_size)
            sender.stop(
                [&counter](){
                    cout << "finished, active tasks left: " << counter << endl;
                });
    });
    sender.start(loop);
    ev_run(loop, 0);
    cout << "implicit loop break after active tasks done" << endl << endl;

    enqueue_batch([&sender, batch_size](size_t &counter){
        if (counter == batch_size / 2)
            sender.terminate(
                [&counter](){
                    cout << "terminated, active tasks left: " << counter << endl;
                });
    });
    sender.start(loop);
    ev_run(loop, 0);
    cout << "implicit loop break after termination" << endl << endl;

    //---------------------------------------------------------------------------------

    enqueue_batch([&sender, batch_size](size_t &counter){
        // init stop after first successful request
        if (counter == batch_size - 1)
        {
            std::this_thread::sleep_for(30s); // freeze mcurl
            sender.stop(
                [&counter](){
                    cout << "finished, active tasks left: " << counter << endl;
                });
        }
    });
    sender.start(loop);
    ev_run(loop, 0);
    cout << "implicit loop break after timeout and active tasks done" << endl << endl;

    enqueue_batch([&sender, batch_size](size_t &counter){
        if (counter == batch_size / 2)
        {
            std::this_thread::sleep_for(30s); // freeze mcurl
            sender.terminate(
                [&counter](){
                    cout << "terminated, active tasks left: " << counter << endl;
                });
        }
    });
    sender.start(loop);
    ev_run(loop, 0);
    cout << "implicit loop break after timeout and termination" << endl << endl;

    //---------------------------------------------------------------------------------

    return 0;
}
