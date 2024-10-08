
/**
* @file z_pool.h
* @the header file of C++ threadPool.
* @author dqsjqian Mr.Zhang
* @mail dqsjqian@163.com
* @date Sep 01 2018
*/
 
#pragma once
 
#include <string>
#include <vector>
#include <list>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
using namespace std;
#define MAX_THREAD_NUM 2048
//线程池,可以提交变参函数或lambda表达式的匿名函数执行,可以获取执行返回值
//不支持类成员函数, 支持类静态成员函数或全局函数,operator()函数等
class threadpool
{
private:
	using Task = std::function<void()>;
	// 线程池
	std::vector<std::thread> pool;
	// 是否关闭某线程
	std::vector<bool> stoped;
	// 任务队列
	std::queue<Task> tasks;
	// 同步
	std::mutex tasks_lock, pool_mutex;
	// 条件阻塞
	std::condition_variable task_cond_var;
	//空闲线程数量
	std::atomic<unsigned int> idle_thread_num;
	//工作线程数量
	std::atomic<unsigned int> work_thread_num;
	//是否让线程自生自灭
	bool freedom_pool;
	//线程池是否暂停工作了
	bool pool_stoped;
private:
	void _initPool_()
	{
		unsigned int index = pool.size();
		stoped.emplace_back(false); idle_thread_num++;//pool容器即将push一个，空闲线程数++
		pool.emplace_back([this, index] {//相对于容器的push_back、insert操作，更加高级
			//此处的循环体不会阻塞整体，因为该处是单个线程的lambda函数，运行在单独的线程中
			while (true)
			{
				std::function<void()> task;// 用于获取一个待执行的 task
				{// unique_lock 相比 lock_guard 的好处是：可以随时 unlock() 和 lock()
					std::unique_lock<std::mutex> lock(this->tasks_lock);
					this->task_cond_var.wait(lock, [this, index]
					{
						return this->stoped[index] || !this->tasks.empty();
					}); // wait 直到有 task
					if (this->stoped[index]) { idle_thread_num--; return; }
					task = std::move(this->tasks.front()); // 取一个 task
					this->tasks.pop();
				}//出了该作用域，lock自动解锁
 
				{   //此处idle_thread_num的--和++操作并不会影响成功启动的线程总数量，
					//因为构造的时候，所有的子线程全在上面wait处等待，并不会执行到此处来
					idle_thread_num--, work_thread_num++;
					task();//耗时任务，执行完成后，可用线程数++
					idle_thread_num++, work_thread_num--;
				}
			}}
		);
	}
 
	unsigned int _checkSize_(unsigned int size)
	{
		size = size < 1 ? 1 : size;
		size = size > MAX_THREAD_NUM ? MAX_THREAD_NUM : size;
		return size;
	}
 
public:
	inline threadpool() { work_thread_num = 0; freedom_pool = false; pool_stoped = false; }
 
	inline ~threadpool()
	{
		for (auto &a : stoped)a = true;
		task_cond_var.notify_all(); // 唤醒所有线程执行
		for (auto &th : pool) {
			if (freedom_pool)th.detach(); // 让线程“自生自灭”
			else if (th.joinable())
				th.join(); // 等待任务结束， 前提：线程一定会执行完
		}
	}
public:
	// 提交一个任务到线程池
	// 返回一个任务的future，调用.get()可以获取返回值（如果有的话），且会阻塞并等待任务执行完
	// 以下两种方法可以实现调用类成员函数：
	// 1、使用bind： .commitTask(std::bind(&ZCQ::helloWorld, &zcq));
	// 2、使用mem_fn： .commitTask(std::mem_fn(&ZCQ::helloWorld), &zcq);
	template<typename FUNC, typename... Args>
	auto commitTask(FUNC&& func, Args&&... args)->std::future<decltype(func(args...))>
	{
		if (hasStopedPool())throw std::runtime_error("the threadPool is stopped.");
		// typename std::result_of<F(Args...)>::type, 函数 func 的返回值类型
		using ReturnType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<ReturnType()> >(
			std::bind(std::forward<FUNC>(func), std::forward<Args>(args)...));
		std::future<ReturnType> future = task->get_future();
		{// 添加任务到队列
			std::lock_guard<std::mutex> lock(tasks_lock);//对当前块的语句加锁
			tasks.emplace([task]() {(*task)(); });// emplace相当于容器的push操作，不会发生临时拷贝，更加高级
			//tasks.push_back(std::move(task)); //跟上面那句写法一模一样
		}
		task_cond_var.notify_one(); // 唤醒一个线程去取获取任务
		return future;
	}
 
	//size：线程池大小;   freedom_threads：是否让线程池"自生自灭"
	void begin(unsigned int size, bool freedom_threads = false)
	{
		pool_stoped = false;
		freedom_pool = freedom_threads;
		size = _checkSize_(size);
		for (unsigned int s = 0; s < size; s++)_initPool_();
	}
 
	//重设线程池大小
	void resize(unsigned int sz)
	{
		pool_stoped = false;
		sz = _checkSize_(sz);
		//活着的线程数
		size_t as = idle_thread_num + work_thread_num;
		//目前线程池总大小
		size_t ps = pool.size(), rs = 0;
		if (sz > as)for (unsigned int s = as; s < sz; s++)_initPool_();
		if (sz < as)for (auto &s : stoped) { if (!s) { s = true, task_cond_var.notify_all(); rs++; }if (rs == as - sz)break; }
	}
 
	// 空闲线程数量
	unsigned int idleNum() { return idle_thread_num; }
	// 工作线程数量
	unsigned int workNum() { return work_thread_num; }
	// 暂时关闭任务提交
	void stopTask() { pool_stoped = true; }
	// 重启任务提交
	void restartTask() { pool_stoped = false; }
	// 强制关闭线程池，后续可用begin或resize重新开启
	void close() { for (auto &a : stoped)a = true; task_cond_var.notify_all(); pool_stoped = true; }
	// 是否停止了线程池的工作
	bool hasStopedPool() { return pool_stoped; }
	// 阻塞且等待所有任务执行完成
	void waitAllTaskRunOver() { while (work_thread_num) {}; }
};
————————————————

                            版权声明：本文为博主原创文章，遵循 CC 4.0 BY-SA 版权协议，转载请附上原文出处链接和本声明。
                        
原文链接：https://blog.csdn.net/dqsjqian/article/details/82316472
