#ifndef __cd_pool_h
#define __cd_pool_h

#include <cassert>
#include <cstddef>
#include <vector>

using std::vector;

template <class Object>
class Pool
{
public:
    struct Chunk
    {
        char data[sizeof(Object)];
    };

private:
    vector<Chunk> pool;
    vector<size_t> freelist;

public:
    Pool(size_t capacity)
    {
        pool.resize(capacity);
        freelist.reserve(capacity);
        for (size_t id = 0; id < capacity; ++id)
            freelist.push_back(id);
    }

    static size_t
    memsize(size_t capacity = 1)
    {
        return (sizeof(Chunk) + sizeof(size_t)) * capacity;
    }

    Chunk*
    get(size_t &id) throw (std::bad_alloc)
    {
        if (freelist.size() == 0)
            throw std::bad_alloc();

        id = freelist.back();
        freelist.pop_back();
        return &pool[id];
    }

    void
    release(size_t id)
    {
        assert(id < pool.size());
        freelist.push_back(id);
    }
};

#define INIT_POOL(Object) \
template<> \
thread_local size_t OnPool<Object>::id_ = 0; \
template<> \
thread_local Pool<Object>* OnPool<Object>::pool_ = nullptr;

template <class Object>
class OnPool
{
    static thread_local size_t id_;
    static thread_local Pool<Object> *pool_;

    size_t id;
    Pool<Object> &pool;

public:
    OnPool() :
        id{id_},
        pool{*pool_} {}

    virtual ~OnPool()
    {
        pool.release(id);
    }

    void * operator new(size_t count, Pool<Object> &pool) throw(std::bad_alloc)
    {
        pool_ = &pool;
        return pool.get(id_);
    }
    void operator delete (void * addr)
    {
    }
};

#endif // __cd_pool_h
