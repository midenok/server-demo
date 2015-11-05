#include <algorithm>
#include <cstring>
#include <iostream>
#include <cassert>
#include "threads.h"

void
TaskHolder::assign (TaskHolder &&h)
{
    // TODO: Check it is called actually
    if (h.size_) {
        memcpy(data_, h.data_, h.size_);
        h.size_ = 0;
    }

}

Thread::Thread(ThreadManager &manager, size_t managed_id) :
    manager_{manager},
    managed_id_{managed_id}
{
}

Thread*
Thread::start()
{
    thread_ = std::thread(&Thread::loop, this);
    return this;
}

void
Thread::loop()
{
    Task * task;
    try {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(sleep_mx_);
                sleep_.wait(lock, [this] { return continue_; });
                std::swap(task_in_, task_out_);
                continue_ = false;
            }
            (*task_out_)->execute();
            manager_.release_thread(managed_id_);
        }
    } catch (int err) {
        std::cerr << "Exiting thread with code " << err << "...\n";
    } catch (std::exception& ex) {
        std::cerr << ex.what() << std::endl;
    }
}

void
ThreadPool::spawn_threads(int thread_count)
{
    for (int i = 0; i < thread_count; ++i) {
        Thread *t = new Thread(*this, i);
        t->start();
        threads.push_back(thr_vec::value_type(t));
    }
    free_threads.reserve(threads.size());
    for (auto &t: threads)
        free_threads.push_back(t.get());
}


void
ThreadPool::release_thread(size_t managed_id)
{
    std::lock_guard<std::mutex> lock(queue_mx_);
    if (task_queue.empty()) {
        std::lock_guard<std::mutex> lock2(free_threads_mx_);
        free_threads.push_back(threads[managed_id].get());
    } else {
        threads[managed_id]->assign_task(std::move(task_queue.front()));
        task_queue.pop_front();
    }
}
