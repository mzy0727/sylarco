

#ifndef MYCOROUTINE_FIBER_H
#define MYCOROUTINE_FIBER_H

#include <ucontext.h>
#include <memory>
#include <functional>
#include "thread.h"

namespace sylar{
    class Fiber : public std::enable_shared_from_this<Fiber> {
    public:
        typedef std::shared_ptr<Fiber> ptr;

        /**
        * @brief 协程状态
        * @details 在sylar基础上进⾏了状态简化，只定义三态转换关系，也就是协程要么正在运⾏(RUNNING)，
        * 要么准备运⾏(READY)，要么运⾏结束(TERM)。不区分协程的初始状态，初始即READY。不区分协程是异常
        结束还是正常结束，
        * 只要结束就是TERM状态。也不区别HOLD状态，协程只要未结束也⾮运⾏态，那就是READY状态。
        */
        enum State {
            // 就绪态，刚创建或者yield之后的状态
            READY,
            // 运行态，resume之后的状态
            RUNNING,
            // 结束态，协程的回调函数执行完之后为TERM状态
            TERM
        };

    private:
        /**
        * @brief 构造函数
        * @attention ⽆参构造函数只⽤于创建线程的第⼀个协程，也就是线程主函数对应的协程，
        * 这个协程只能由GetThis()⽅法调⽤，所以定义成私有⽅法
        */
        Fiber();

    public:
        /**
        * @brief 构造函数，⽤于创建⽤户协程
        * @param[in] cb 协程⼊⼝函数
        * @param[in] stack_size 栈⼤⼩
        * @param[in] run_in_scheduler 本协程是否参与调度器调度，默认为true
        */
        Fiber(std::function<void()> cb, size_t stack_size = 0, bool run_in_scheduler = true);

        /**
         * @brief 析构函数
         */
        ~Fiber();

        /**
         * @brief 重置协程状态和入口函数，复用栈空间，不重新创建栈
         * @param cb
         */
        void reset(std::function<void()> cb);

        /**
        * @brief 将当前协程切到到执⾏状态
        * @details 当前协程和正在运⾏的协程进⾏交换，前者状态变为RUNNING，后者状态变为READY
        */
        void resume();

        /**
        * @brief 当前协程让出执⾏权
        * @details 当前协程与上次resume时退到后台的协程进⾏交换，前者状态变为READY，后者状态变为RUNNING
        */
        void yield();

        /**
         * @brief 获取协程ID
         * @return 协程ID
         */
        uint64_t getId() const { return m_id; }

        /**
         * @brief 获取协程状态
         * @return 协程状态
         */
        State getState() const { return m_state; }

    public:
        /**
         * @brief 设置当前正在运行的协程，即设置线程局部变量t_fiber的值
         * @param f 协程
         */
        static void SetThis(Fiber *f);

        /**
        * @brief 返回当前线程正在执⾏的协程
        * @details 如果当前线程还未创建协程，则创建线程的第⼀个协程，且该协程为当前线程的主协程，其他协程都通过这个协程来调度，也就是说，其他协程
        * 结束时,都要切回到主协程，由主协程重新选择新的协程进⾏resume
        * @attention 线程如果要创建协程，那么应该⾸先执⾏⼀下Fiber::GetThis()操作，以初始化主函数协程
        */
        static Fiber::ptr GetThis();

        /**
        * @brief 获取总协程数
        */
        static uint64_t TotalFibers();

        /**
         * @brief 协程入口函数
         */
        static void MainFunc();

        /**
         * @brief 获取当前协程的id
         * @return 协程id
         */
        static uint64_t GetFiberId();

    private:
        // 协程id
        uint64_t m_id = 0;
        // 协程栈大小
        uint32_t m_stack_size = 0;
        // 协程状态
        State m_state = READY;
        // 协程上下文
        ucontext_t m_ctx;
        // 协议栈地址
        void *m_stack = nullptr;
        // 协程入口函数
        std::function<void()> m_cb;
        // 本协程是否参与调度器调度
        bool m_runInScheduler;
    };

} // namespace sylar

#endif //MYCOROUTINE_FIBER_H

