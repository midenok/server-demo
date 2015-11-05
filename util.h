#ifndef __cd_util_h
#define __cd_util_h

#include <stdexcept>
#include <cerrno>
#include <system_error>
#include <sstream>
#include <iostream>
#include <libgen.h>

#define Errno(...) \
    ErrnoEx(make_what_arg(__FILE__, __LINE__, ##__VA_ARGS__))

template<class OStream, typename ... Any>
void stream_all(OStream& out, Any ... args)
{
    int dummy[sizeof...(Any)] = { (out << args, 0)... };
}

template <typename ...Any>
std::string
make_what_arg(const char* file, int line, Any ...args)
{
    std::ostringstream what_arg;
    what_arg << "!" << basename(const_cast<char*>(file)) << ":" << line << "! ";
    stream_all(what_arg, args...);
    return what_arg.str();
}

class ErrnoEx : public std::system_error
{
public:
    ErrnoEx(std::string what_arg)
        : std::system_error(errno, std::system_category(), what_arg) {}
};

template<class OStream, class Object, typename ... Any>
void debug_message(OStream& out, char q1, const char* file, int line, char q2, Object *obj, Any ... args)
{
    std::ostringstream s;
    s << q1 << basename(const_cast<char*>(file)) << ":" << line << q2 << " " << obj << ": ";
    stream_all(s, args...);
    s << std::endl;
    out << s.str() << std::flush;
}

#ifdef NDEBUG
#define debug(...)
#define cdebug(...)
#define error(...) stream_all(std::cerr, __VA_ARGS__)
#define cerror(func, ...) stream_all(std::cerr, __VA_ARGS__)
#else // !NDEBUG
#define debug(...) \
if (ENABLED_OPT(VERBOSE)) \
    debug_message(std::cout, '{', __FILE__, __LINE__, '}', this, __VA_ARGS__)
#define cdebug(...) \
if (ENABLED_OPT(VERBOSE)) \
    debug_message(std::cout, '{', __FILE__, __LINE__, '}', __VA_ARGS__)
#define error(...)  debug_message(std::cerr, '#', __FILE__, __LINE__, '#', this, __VA_ARGS__)
#define cerror(...)  debug_message(std::cerr, '#', __FILE__, __LINE__, '#', __VA_ARGS__)
#endif // NDEBUG

#endif //__cd_util_h
