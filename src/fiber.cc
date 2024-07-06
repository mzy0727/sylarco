
#include <atomic>
#include <utility>
#include "fiber.h"
#include "log.h"
#include "config.h"
#include "macro.h"
#include "scheduler.h"

namespace sylar{
    static Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    // 全局静态变量，用于统计当前的协程数
    static std::atomic<uint64_t> s_fiber_count{0};
    // 全局静态变量，用于生成协程id
    static std::atomic<uint64_t> s_fiber_id{0};

    // 线程局部变量，当前线程正在运行的协程
    static thread_local  Fiber *t_fiber = nullptr;
    // 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程中运行，智能指针形式
    static thread_local Fiber::ptr t_thread_fiber = nullptr;

    // 协程栈大小，可以通过配置文件获取，默认128K
    static ConfigVar<uint32_t>::ptr g_fiber_stack_size =
            Config::Lookup<uint32_t>("fiber.stack_size", 128 * 1024, "fiber stack size");

    class MallocStackAllocator {
    public:
        static void *Alloc(size_t size) {return malloc(size); }
        static void Dealloc(void *vp, size_t size) {return free(vp); }
    };

    // 定义类型别名
    using StackAllocator = MallocStackAllocator;

    /**
    * @brief 构造函数
    * @attention ⽆参构造函数只⽤于创建线程的第⼀个协程，也就是线程主函数对应的协程，
    * 这个协程只能由GetThis()⽅法调⽤，所以定义成私有⽅法
    */
    Fiber::Fiber() {
        // 设置当前运行的协程
        SetThis(this);
        m_state = RUNNING;

        // 获取协程上下文并保存到m_ctx中
        if (getcontext(&m_ctx)) {
            // 获取上下文失败
            SYLAR_ASSERT2(false, "getcontext");
        }
        // 协程数加1
        ++s_fiber_count;
        m_id = s_fiber_id++; // 协程id从0开始，用完加1

        SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber() main id = " << m_id;
    }

    /**
    * @brief 设置当前运行的协程
    */
    void Fiber::SetThis(Fiber *f) {
        t_fiber = f;
    }

    Fiber::Fiber(std::function<void()> cb, size_t stack_size, bool run_in_scheduler)
            : m_id(s_fiber_id++)
            , m_cb(std::move(cb))
            , m_runInScheduler(run_in_scheduler) {
        ++s_fiber_count;
        m_stack_size = stack_size ? stack_size : g_fiber_stack_size->getValue();
        m_stack = StackAllocator::Alloc(m_stack_size);

        if (getcontext(&m_ctx)) {
            // 获取上下文失败
            SYLAR_ASSERT2(false, "getcontext");
        }

        m_ctx.uc_link = nullptr;
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stack_size;

        // 修改由getcontext获取到的上下⽂指针m_ctx，将其与⼀个函数func进⾏绑定，后面的参数就是func需要的
        makecontext(&m_ctx, &Fiber::MainFunc, 0);
        SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber() id = " << m_id;
    }


    /**
    * 获取当前协程，同时充当初始化当前线程主协程的作用，这个函数在使用协程之前要调用一下
    */
    Fiber::ptr Fiber::GetThis() {
        if (t_fiber) {
            return t_fiber->shared_from_this();
        }

        Fiber::ptr main_fiber(new Fiber);
        SYLAR_ASSERT(t_fiber == main_fiber.get());
        t_thread_fiber = main_fiber;
        return t_fiber->shared_from_this();
    }

    /**
    * @brief 析构函数，线程的主协程析构时需要特殊处理，因为主协程没有分配栈和cb
    */
    Fiber::~Fiber() {
        SYLAR_LOG_DEBUG(g_logger) << "Fiber::~Fiber() id = " << m_id;
        --s_fiber_count;
        if (m_stack) {
            // 有栈，说明是子协程，需要确保子协程一定是结束状态
            SYLAR_ASSERT(m_state == TERM);
            StackAllocator::Dealloc(m_stack, m_stack_size);
            SYLAR_LOG_DEBUG(g_logger) << "dealloc stack, id = " << m_id;
        } else {
            // 没有栈，说明是线程的主协程
            SYLAR_ASSERT(!m_cb); // 主协程是没有cb的
            SYLAR_ASSERT(m_state == RUNNING) // 主协程一定是RUNNING状态

            Fiber * cur = t_fiber; // 当前协程就是自己
            if (cur == this) {
                SetThis(nullptr);
            }
        }

    }

    /**
    * @brief 在非对称协程中，执行resume时的执行环境一定是位于线程的主协程中。
    */
    void Fiber::resume() {
        SYLAR_ASSERT(m_state != TERM && m_state != RUNNING);
        // 设置运行的协程为当前协程
        SetThis(this);
        m_state = RUNNING;

        // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
        if (m_runInScheduler) {
            if (swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx)) {
                SYLAR_ASSERT2(false, "swapcontext");
            }
        } else {
            // 恢复m_ctx的上下文，同时将当前上下文保存到t_thread_fiber的m_ctx中
            if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
                SYLAR_ASSERT2(false, "swapcontext");
            }
        }

    }

    /**
    * @brief 在非对称协程中，执行yield时的执行环境一定是位于线程的子协程中。
    */
    void Fiber::yield() {

        // 协程运行完之后会自动yield一次，回到主协程，此时状态已为结束状态
        SYLAR_ASSERT(m_state == RUNNING || m_state == TERM);
        SetThis(t_thread_fiber.get());
        // 不为结束状态才转变为ready等待调度
        if (m_state != TERM) {
            m_state = READY;
        }
        // 保存当前的上下文到m_ctx中，并切换当前上下文为t_thread_fiber->m_ctx
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            SYLAR_ASSERT2(false, "swapcontext");
        }

    }

    /**
    * @brief 协程入口函数
    * 这里没有处理协程函数出现异常的情况，同样是为了简化状态管理，并且个人认为协程的异常不应该由框架处理，应该由开发者自行处理
    */
    void Fiber::MainFunc() {
        Fiber::ptr cur = GetThis(); // GetThis()的shared_from_this（）方法让引用计数加1
        SYLAR_ASSERT(cur);

        cur->m_cb(); // 真正执行协程的入口函数
        cur->m_cb = nullptr;
        cur->m_state = TERM;

        auto raw_ptr = cur.get();
        cur.reset(); // 手动让t_fiber的引用计数减1
        raw_ptr->yield(); // 协程结束时自动yield，回到主协程。
    }

    /**
    * 这⾥为了简化状态管理，强制只有TERM状态的协程才可以重置，但其实刚创建好但没执⾏过的协程也应该允许重置的
    */
    void Fiber::reset(std::function<void()> cb) {
        // 保证协程栈存在且状态为TERM
        SYLAR_ASSERT(m_stack);
        SYLAR_ASSERT(m_state == TERM);

        m_cb = cb;
        if (getcontext(&m_ctx)) {
            // 获取上下文失败
            SYLAR_ASSERT2(false, "getcontext");
        }
        m_ctx.uc_link = nullptr; // 函数运行结束后恢复的上下文
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stack_size;
        // 修改由getcontext获取到的上下⽂指针m_ctx将其与⼀个函数func进⾏绑定，后面的参数就是func需要的
        makecontext(&m_ctx, &Fiber::MainFunc, 0);
        m_state = READY;


    }

    uint64_t Fiber::GetFiberId() {
        if (t_fiber) {
            return t_fiber->getId();
        }
        return 0;
    }

    uint64_t Fiber::TotalFibers() {
        return s_fiber_count;
    }
}

