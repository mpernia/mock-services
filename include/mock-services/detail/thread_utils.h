#ifndef MOCK_SERVICES_DETAIL_THREAD_UTILS_H
#define MOCK_SERVICES_DETAIL_THREAD_UTILS_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mock_services {
namespace detail {

class thread_group {
public:
    using stop_flag = std::shared_ptr<std::atomic<bool>>;

    static stop_flag make_stop_flag() {
        return stop_flag(new std::atomic<bool>(false));
    }

    void add(std::thread t, stop_flag flag) {
        std::lock_guard<std::mutex> lock(mtx_);
        threads_.push_back({std::move(t), std::move(flag)});
    }

    void join_finished() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = threads_.begin(); it != threads_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void join_all() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& entry : threads_) {
            if (entry.thread.joinable())
                entry.thread.join();
        }
        threads_.clear();
    }

private:
    struct entry {
        std::thread thread;
        stop_flag done;
    };

    std::vector<entry> threads_;
    std::mutex mtx_;
};

}
}
#endif
