#pragma once
// Exactly one submitted frame may be in flight. The producer can prepare the
// next frame concurrently, but cannot get two frames ahead. No frame is dropped.
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
namespace touhou::vitacore {
class FrameWorker {
public:
    using Tasks=std::vector<std::function<void()>>;
    ~FrameWorker(){stop();}
    bool start(std::function<bool()> initialize,std::function<void()> shutdown){
        thread=std::thread([this,initialize,shutdown]{
            const bool ok=initialize();
            {std::lock_guard<std::mutex> l(mutex);ready=true;initialized=ok;}done.notify_all();
            if(ok)for(;;){
                Tasks tasks;
                {std::unique_lock<std::mutex> l(mutex);wake.wait(l,[this]{return stopping||busy;});
                 if(!busy&&stopping)break;tasks.swap(packet);}
                for(auto& task:tasks)task();
                // Release snapshots before declaring the packet complete.
                tasks.clear();
                {std::lock_guard<std::mutex> l(mutex);busy=false;++completed;}done.notify_all();
            }
            shutdown();
        });
        std::unique_lock<std::mutex> l(mutex);done.wait(l,[this]{return ready;});return initialized;
    }
    void submit(Tasks tasks){
        std::unique_lock<std::mutex> l(mutex);done.wait(l,[this]{return !busy;});
        packet=std::move(tasks);busy=true;++submitted;l.unlock();wake.notify_one();
    }
    void drain(){std::unique_lock<std::mutex> l(mutex);done.wait(l,[this]{return !busy;});}
    void stop(){
        if(!thread.joinable())return;drain();
        {std::lock_guard<std::mutex> l(mutex);stopping=true;}wake.notify_one();thread.join();
    }
    unsigned finished(){std::lock_guard<std::mutex> l(mutex);return completed;}
private:
    std::mutex mutex;std::condition_variable wake,done;std::thread thread;
    Tasks packet;bool ready=false,initialized=false,busy=false,stopping=false;
    unsigned submitted=0,completed=0;
};
}
