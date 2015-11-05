#ifndef __cd_threads_h
#define __cd_trheads_h

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>

using std::vector;
using std::deque;

class Task
{
public:
    virtual void execute() = 0;
};

class ThreadManager
{
public:
    virtual void release_thread(size_t managed_id) = 0;
};

const int MAX_TASK_SIZE = 96;

class TaskHolder
{
    /* data_ MUST be first member in TaskHolder!
       We will access Task (in data_) by TaskHolder*.
       Yes, this is a dirty hack... */
    char data_[MAX_TASK_SIZE];
    size_t size_;

public:
    TaskHolder() : size_{0} {}

    template <class T>
    TaskHolder (T &&y)
    {
        assign(y);
    }

    template <class T>
    TaskHolder& operator= (T &&y)
    {
        assign(y);
    }

    Task *
    operator->()
    {
        return (Task *) data_;
    }
    operator Task*()
    {
        return (Task *) data_;
    }

private:
    template <class T>
    void assign (T &y)
    {
        static_assert (std::is_base_of<Task, T>::value, "T is not a descendant of Task!");
        static_assert (sizeof(T) <= sizeof(data_), "TaskHolder capacity is not enough!");
        new (data_) T(std::move(y));
        size_ = sizeof(T);
    }
    void assign (TaskHolder &&h);
};


class Thread
{
private:
    ThreadManager &manager_;
    size_t managed_id_;    // thread identifier inside ThreadManager
    std::thread thread_;
    std::mutex sleep_mx_;
    std::condition_variable sleep_;

    bool continue_ = false;
    TaskHolder tasks_[2];
    TaskHolder* task_in_ = &tasks_[0];
    TaskHolder* task_out_ = &tasks_[1];

    Thread(const Thread & copy) = delete;

public:
    void loop();

    Thread(ThreadManager &manager, size_t managed_id);
    virtual ~Thread() {}

    Thread* start();

    std::thread* operator-> () { return &thread_; }

    template <class AnyTask>
    AnyTask* assign_task(AnyTask &&task)
    {
        std::lock_guard<std::mutex> lk(sleep_mx_);
        /* TODO: avoid copying (moving actually) by returning space for placement new to task creator.
           Task creator by placement new will construct object already in thread space. */
        *task_in_ = std::move(task);
        continue_ = true;

        // notify() here is also guarded to prevent waiting thread
        // missing notify on kernel preemption (see SO:15072479)
        sleep_.notify_one();
        return (AnyTask *)task_in_;
    }

};

class ThreadPool : public ThreadManager
{
private:
    typedef vector<std::unique_ptr<Thread> > thr_vec;
    thr_vec threads;
    vector<Thread*> free_threads;
    deque<TaskHolder> task_queue;
    std::mutex free_threads_mx_;
    std::mutex queue_mx_;

    virtual void release_thread(size_t managed_id);
public:
    void spawn_threads(int thread_count);

    template <class AnyTask>
    AnyTask *
    add_task(AnyTask &task)
    {
        std::unique_lock<std::mutex> lock(free_threads_mx_);
        if (!free_threads.empty()) {
            Thread *thread;
            {
                thread = free_threads.back();
                free_threads.pop_back();
            }
            return thread->assign_task(std::move(task));
        } else {
            lock.unlock();
            std::lock_guard<std::mutex> lock(queue_mx_);
            task_queue.push_back(task);
            return static_cast<AnyTask *>(&(*task_queue.back()));
        }
    }
    virtual ~ThreadPool()
    {
        for (auto &thread: threads) {
            (*thread)->detach();
        }
    }
};

#endif //__cd_threads_h
