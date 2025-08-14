#include <cstring>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <random>
#include <memory>
#include <cstdint>

const int HEARTBEAT_TIMEOUT_MS = 5000;
const int HEARTBEAT_INTERVAL_MS = 1000;
const int CRASH_PROBABILITY = 5;

// 任务状态枚举
enum class TaskStatus : std::uint8_t { PENDING, ASSIGNED, COMPLETED, FAILED };

// 任务类型枚举
enum class TaskType : std::uint8_t { MAP, REDUCE };

// Worker状态枚举
enum class WorkerStatus : std::uint8_t { IDLE, BUSY, FAILED };

// 基础任务结构
struct Task {
    int task_id;
    TaskType type;
    TaskStatus status;
    int worker_id;
    std::string input_data;
    std::string output_data;
    std::chrono::steady_clock::time_point assigned_time;

    Task(int id, TaskType t, const std::string& input)
            : task_id(id)
            , type(t)
            , status(TaskStatus::PENDING)
            , worker_id(-1)
            , input_data(input)
    {
    }
};

// Worker信息结构
struct WorkerInfo {
    int worker_id;
    WorkerStatus status;
    std::chrono::steady_clock::time_point last_heartbeat;
    std::thread worker_thread;

    WorkerInfo(int id)
            : worker_id(id)
            , status(WorkerStatus::IDLE)
            , last_heartbeat(std::chrono::steady_clock::now())
    {
    }
};

// 用户定义的Map和Reduce函数接口
class UserFunctions {
public:
    // Map函数：将输入数据转换为键值对
    virtual std::vector<std::pair<std::string, std::string> >
    map(const std::string& input) = 0;

    // Reduce函数：聚合相同键的所有值
    virtual std::string reduce(const std::string& key,
                               const std::vector<std::string>& values) = 0;

    virtual ~UserFunctions() = default;
};

// 示例：词频统计的实现
class WordCountFunctions : public UserFunctions {
public:
    std::vector<std::pair<std::string, std::string> >
    map(const std::string& input) override
    {
        std::vector<std::pair<std::string, std::string> > result;
        std::string word;
        for (char c : input) {
            if (std::isalnum(c)) {
                word += std::tolower(c);
            } else if (!word.empty()) {
                result.emplace_back(word, "1");
                word.clear();
            }
        }
        if (!word.empty()) {
            result.emplace_back(word, "1");
        }
        return result;
    }

    std::string reduce(const std::string& key,
                       const std::vector<std::string>& values) override
    {
        int count = 0;
        for (const auto& value : values) {
            count += std::stoi(value);
        }
        return std::to_string(count);
    }
};

class MapReduceMaster {
private:
    std::vector<std::unique_ptr<WorkerInfo> > workers;
    std::vector<std::shared_ptr<Task> > map_tasks;
    std::vector<std::shared_ptr<Task> > reduce_tasks;
    std::map<std::string, std::vector<std::string> > intermediate_data;
    std::map<std::string, std::string> final_results;

    std::mutex master_mutex;
    std::condition_variable task_cv;
    std::condition_variable completion_cv;

    std::atomic<bool> running{true};
    std::atomic<int> completed_map_tasks{0};
    std::atomic<int> completed_reduce_tasks{0};

    UserFunctions* user_functions;

public:
    MapReduceMaster(int num_workers, UserFunctions* functions)
            : user_functions(functions)
    {
        // 创建workers
        for (int i = 0; i < num_workers; ++i) {
            workers.push_back(std::make_unique<WorkerInfo>(i));
        }

        // 启动心跳监控线程
        std::thread([this]() { heartbeat_monitor(); }).detach();
    }

    ~MapReduceMaster()
    {
        running = false;
        task_cv.notify_all();

        // 等待所有worker线程结束
        for (auto& worker : workers) {
            if (worker->worker_thread.joinable()) {
                worker->worker_thread.join();
            }
        }
    }

    // 添加Map任务
    void add_map_task(const std::string& input_data)
    {
        std::lock_guard<std::mutex> lock(master_mutex);
        int task_id = map_tasks.size();
        map_tasks.push_back(
                std::make_shared<Task>(task_id, TaskType::MAP, input_data));
        std::cout << "Added Map task " << task_id
                  << " with input: " << input_data.substr(0, 50) << "..."
                  << '\n';
    }

    // 启动MapReduce作业
    void start_job()
    {
        std::cout << "Starting MapReduce job with " << map_tasks.size()
                  << " map tasks" << '\n';

        // 启动worker线程
        for (auto& worker : workers) {
            worker->worker_thread = std::thread(
                    [this, &worker]() { worker_loop(worker.get()); });
        }

        // 等待所有Map任务完成
        std::unique_lock<std::mutex> lock(master_mutex);
        completion_cv.wait(lock, [this]() {
            return completed_map_tasks.load()
                   == static_cast<int>(map_tasks.size());
        });

        std::cout << "All Map tasks completed. Starting Reduce phase..."
                  << '\n';

        // 创建Reduce任务
        create_reduce_tasks();

        // 等待所有Reduce任务完成
        completion_cv.wait(lock, [this]() {
            return completed_reduce_tasks.load()
                   == static_cast<int>(reduce_tasks.size());
        });

        std::cout << "All Reduce tasks completed. Job finished!" << '\n';
        running = false;
        task_cv.notify_all();
    }

    // 获取最终结果
    const std::map<std::string, std::string>& get_results() const
    {
        return final_results;
    }

private:
    // Worker主循环
    void worker_loop(WorkerInfo* worker)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> crash_dist(1, 100);

        while (running) {
            // 模拟worker崩溃（5%概率）
            if (crash_dist(gen) <= CRASH_PROBABILITY) {
                std::cout << "Worker " << worker->worker_id << " crashed!"
                          << '\n';
                worker->status = WorkerStatus::FAILED;
                return;
            }

            auto task = get_next_task(worker);
            if (!task) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 执行任务
            execute_task(worker, task);

            // 发送心跳
            send_heartbeat(worker);

            std::this_thread::sleep_for(
                    std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        }
    }

    // 获取下一个待执行任务
    std::shared_ptr<Task> get_next_task(WorkerInfo* worker)
    {
        std::lock_guard<std::mutex> lock(master_mutex);

        worker->status = WorkerStatus::IDLE;

        // 首先查找Map任务
        for (auto& task : map_tasks) {
            if (task->status == TaskStatus::PENDING) {
                assign_task_to_worker(task, worker);
                return task;
            }
        }

        // 如果所有Map任务完成，查找Reduce任务
        if (completed_map_tasks.load() == static_cast<int>(map_tasks.size())) {
            for (auto& task : reduce_tasks) {
                if (task->status == TaskStatus::PENDING) {
                    assign_task_to_worker(task, worker);
                    return task;
                }
            }
        }

        return nullptr;
    }

    // 分配任务给worker
    void assign_task_to_worker(std::shared_ptr<Task> task, WorkerInfo* worker)
    {
        task->status = TaskStatus::ASSIGNED;
        task->worker_id = worker->worker_id;
        task->assigned_time = std::chrono::steady_clock::now();
        worker->status = WorkerStatus::BUSY;

        std::cout << "Assigned "
                  << (task->type == TaskType::MAP ? "Map" : "Reduce")
                  << " task " << task->task_id << " to worker "
                  << worker->worker_id << '\n';
    }

    // 执行任务
    void execute_task(WorkerInfo* worker, std::shared_ptr<Task> task)
    {
        try {
            if (task->type == TaskType::MAP) {
                execute_map_task(task);
            } else {
                execute_reduce_task(task);
            }

            std::lock_guard<std::mutex> lock(master_mutex);
            task->status = TaskStatus::COMPLETED;

            if (task->type == TaskType::MAP) {
                completed_map_tasks++;
                std::cout << "Map task " << task->task_id
                          << " completed by worker " << worker->worker_id
                          << '\n';
            } else {
                completed_reduce_tasks++;
                std::cout << "Reduce task " << task->task_id
                          << " completed by worker " << worker->worker_id
                          << '\n';
            }

            completion_cv.notify_all();

        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(master_mutex);
            task->status = TaskStatus::FAILED;
            std::cout << "Task " << task->task_id << " failed: " << e.what()
                      << '\n';
        }
    }

    // 执行Map任务
    void execute_map_task(std::shared_ptr<Task> task)
    {
        // 模拟任务执行时间
        std::this_thread::sleep_for(
                std::chrono::milliseconds(500 + rand() % 1000));

        auto key_value_pairs = user_functions->map(task->input_data);

        std::lock_guard<std::mutex> lock(master_mutex);
        for (const auto& pair : key_value_pairs) {
            intermediate_data[pair.first].push_back(pair.second);
        }
    }

    // 执行Reduce任务
    void execute_reduce_task(std::shared_ptr<Task> task)
    {
        // 模拟任务执行时间
        std::this_thread::sleep_for(
                std::chrono::milliseconds(300 + rand() % 500));

        std::string key = task->input_data; // For reduce tasks, input_data
                                            // contains the key
        std::vector<std::string> values;

        {
            std::lock_guard<std::mutex> lock(master_mutex);
            values = intermediate_data[key];
        }

        std::string result = user_functions->reduce(key, values);

        std::lock_guard<std::mutex> lock(master_mutex);
        final_results[key] = result;
    }

    // 创建Reduce任务
    void create_reduce_tasks()
    {
        int task_id = 0;
        for (const auto& pair : intermediate_data) {
            reduce_tasks.push_back(std::make_shared<Task>(
                    task_id++, TaskType::REDUCE, pair.first));
        }
        std::cout << "Created " << reduce_tasks.size() << " Reduce tasks"
                  << '\n';
    }

    // 发送心跳
    void send_heartbeat(WorkerInfo* worker)
    {
        std::lock_guard<std::mutex> lock(master_mutex);
        worker->last_heartbeat = std::chrono::steady_clock::now();
    }

    // 心跳监控
    void heartbeat_monitor()
    {
        while (running) {
            std::this_thread::sleep_for(
                    std::chrono::milliseconds(HEARTBEAT_TIMEOUT_MS));

            std::lock_guard<std::mutex> lock(master_mutex);
            auto now = std::chrono::steady_clock::now();

            for (auto& worker : workers) {
                if (worker->status != WorkerStatus::FAILED) {
                    auto time_since_heartbeat =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - worker->last_heartbeat)
                                    .count();

                    if (time_since_heartbeat > HEARTBEAT_TIMEOUT_MS) {
                        std::cout << "Worker " << worker->worker_id
                                  << " heartbeat timeout. Marking as failed."
                                  << '\n';
                        worker->status = WorkerStatus::FAILED;

                        // 重新分配失败worker的任务
                        reassign_failed_worker_tasks(worker->worker_id);
                    }
                }
            }
        }
    }

    // 重新分配失败worker的任务
    void reassign_failed_worker_tasks(int failed_worker_id)
    {
        std::cout << "Reassigning tasks from failed worker " << failed_worker_id
                  << '\n';

        // 重新分配Map任务
        for (auto& task : map_tasks) {
            if (task->worker_id == failed_worker_id
                && task->status == TaskStatus::ASSIGNED) {
                task->status = TaskStatus::PENDING;
                task->worker_id = -1;
                std::cout << "Reassigned Map task " << task->task_id << '\n';
            }
        }

        // 重新分配Reduce任务
        for (auto& task : reduce_tasks) {
            if (task->worker_id == failed_worker_id
                && task->status == TaskStatus::ASSIGNED) {
                task->status = TaskStatus::PENDING;
                task->worker_id = -1;
                std::cout << "Reassigned Reduce task " << task->task_id << '\n';
            }
        }

        task_cv.notify_all();
    }
};

class Args {
public:
    int num_workers;
    int crash_probability;

    Args(int num_workers, int crash_probability)
            : num_workers(num_workers)
            , crash_probability(crash_probability)
    {
    }
};

Args parse_args(int argc, char* argv[])
{
    int num_workers = 8;
    int crash_probability = 5;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--num-workers") == 0) {
            num_workers = atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--crash-probability") == 0) {
            crash_probability = atoi(argv[i + 1]);
        }
    }

    if (num_workers < 1) {
        std::cerr << "num_workers must be greater than 0" << '\n';
        throw std::invalid_argument("num_workers must be greater than 0");
    }
    if (crash_probability < 0 || crash_probability > 100) {
        std::cerr << "crash_probability must be between 0 and 100" << '\n';
        throw std::invalid_argument(
                "crash_probability must be between 0 and 100");
    }

    return Args(num_workers, crash_probability);
}

// 示例用法
int main(int argc, char* argv[])
{
    try {
        Args args = parse_args(argc, argv);

        std::cout << "=== MapReduce with Fault Tolerance Demo ===" << '\n';
        std::cout << "num_workers: " << args.num_workers << '\n';
        std::cout << "crash_probability: " << args.crash_probability << '\n';

        // 创建用户函数实例
        WordCountFunctions functions;

        // 创建Master（3个worker）
        MapReduceMaster master(args.num_workers, &functions);

        // 添加一些示例数据
        std::vector<std::string> input_data = {"hello world hello",
                                               "world of mapreduce",
                                               "hello mapreduce world",
                                               "fault tolerance test",
                                               "mapreduce with recovery",
                                               "hello fault tolerance",
                                               "world test recovery",
                                               "hi world hello",
                                               "hi world hello"};

        for (const auto& data : input_data) {
            master.add_map_task(data);
        }

        // 启动作业
        master.start_job();

        // 显示结果
        std::cout << "\n=== Final Results ===" << '\n';
        const auto& results = master.get_results();
        for (const auto& pair : results) {
            std::cout << pair.first << ": " << pair.second << '\n';
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}