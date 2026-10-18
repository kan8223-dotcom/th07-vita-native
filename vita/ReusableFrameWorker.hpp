#pragma once
#include "FrameWorker.hpp"
namespace th07_pipeline {
// TH07-private variant of the shared worker's identical one-inflight contract.
// Empty command-vector storage is handed back to the producer, not freed at
// every packet. Captures are still destroyed before completion is published.
class ReusableFrameWorker {
public:
    using Tasks=touhou::vitacore::FrameWorker::Tasks;
    ~ReusableFrameWorker(){stop();}
    bool start(std::function<bool()> initialize,std::function<void()> shutdown){
        thread=std::thread([this,initialize,shutdown]{
            const bool ok=initialize();
            {std::lock_guard<std::mutex> l(mutex);ready=true;initialized=ok;}done.notify_all();
            Tasks tasks;
            if(ok)for(;;){
                {std::unique_lock<std::mutex> l(mutex);wake.wait(l,[this]{return stopping||busy;});
                 if(!busy&&stopping)break;tasks.swap(packet);}
                for(auto& task:tasks)task();
                tasks.clear();
                {std::lock_guard<std::mutex> l(mutex);busy=false;++completed;}done.notify_all();
            }
            shutdown();
        });
        std::unique_lock<std::mutex> l(mutex);done.wait(l,[this]{return ready;});return initialized;
    }
    void submit(Tasks& incoming){
        std::unique_lock<std::mutex> l(mutex);done.wait(l,[this]{return !busy;});
        packet.swap(incoming);busy=true;l.unlock();wake.notify_one();
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
    unsigned completed=0;
};
}
