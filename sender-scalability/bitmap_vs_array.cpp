#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>

// 位图实现
class BitMap {
private:
    std::vector<uint64_t> data;
    size_t size_bits;

public:
    BitMap(size_t size) : size_bits(size) {
        if (size == 0) {
            throw std::invalid_argument("数据大小不能为0");
        }
        data.resize((size + 63) / 64, 0);
    }

    void set(size_t index, bool value) {
        if (index >= size_bits) {
            throw std::out_of_range("位图索引越界: " + std::to_string(index));
        }
        size_t bucket = index / 64;
        size_t pos = index % 64;
        if (value) {
            data[bucket] |= (1ULL << pos);
        } else {
            data[bucket] &= ~(1ULL << pos);
        }
    }

    bool get(size_t index) const {
        if (index >= size_bits) {
            throw std::out_of_range("位图索引越界: " + std::to_string(index));
        }
        size_t bucket = index / 64;
        size_t pos = index % 64;
        return (data[bucket] & (1ULL << pos)) != 0;
    }

    size_t size() const { return size_bits; }
};

// 使用uint32_t的数组实现
class Array {
private:
    std::vector<uint32_t> data;

public:
    Array(size_t size) : data(size, 0) {
        if (size == 0) {
            throw std::invalid_argument("数据大小不能为0");
        }
    }

    // 设置为非0表示true，0表示false
    void set(size_t index, bool value) {
        if (index >= data.size()) {
            throw std::out_of_range("数组索引越界: " + std::to_string(index));
        }
        data[index] = value ? 1 : 0;  // 直接写入32位值，无需读-改-写
    }

    bool get(size_t index) const {
        if (index >= data.size()) {
            throw std::out_of_range("数组索引越界: " + std::to_string(index));
        }
        return data[index] != 0;
    }

    size_t size() const { return data.size(); }
};

// 测试位图性能
double test_bitmap(size_t data_size, size_t num_write_threads, size_t iterations) {
    try {
        BitMap bitmap(data_size);
        
        // 写入线程函数 - 无范围限制，可访问所有索引
        auto write_func = [&](size_t thread_id) {
            try {
                std::mt19937 rng(thread_id);
                std::uniform_int_distribution<size_t> dist(0, data_size - 1);
                
                for (size_t i = 0; i < iterations; ++i) {
                    size_t index = dist(rng);
                    bitmap.set(index, i % 2 == 0); // 交替设置true/false
                }
            } catch (const std::exception& e) {
                std::cerr << "写入线程 " << thread_id << " 错误: " << e.what() << std::endl;
                std::terminate();
            }
        };
        
        // 读取函数
        auto read_func = [&]() {
            try {
                std::mt19937 rng(num_write_threads);
                std::uniform_int_distribution<size_t> dist(0, data_size - 1);
                
                for (size_t i = 0; i < iterations; ++i) {
                    size_t index = dist(rng);
                    volatile bool value = bitmap.get(index); // volatile防止优化
                }
            } catch (const std::exception& e) {
                std::cerr << "读取线程错误: " << e.what() << std::endl;
                std::terminate();
            }
        };
        
        // 计时开始
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // 创建写入线程
        std::vector<std::thread> write_threads;
        for (size_t i = 0; i < num_write_threads; ++i) {
            write_threads.emplace_back(write_func, i);
        }
        
        // 单线程读取
        std::thread read_thread(read_func);
        
        // 等待所有线程完成
        for (auto& t : write_threads) {
            t.join();
        }
        read_thread.join();
        
        // 计时结束
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        
        return elapsed.count();
    } catch (const std::exception& e) {
        std::cerr << "位图测试错误: " << e.what() << std::endl;
        return -1;
    }
}

// 测试uint32_t数组性能
double test_array(size_t data_size, size_t num_write_threads, size_t iterations) {
    try {
        Array array(data_size);
        
        // 写入线程函数 - 无范围限制，可访问所有索引
        auto write_func = [&](size_t thread_id) {
            try {
                std::mt19937 rng(thread_id + data_size);
                std::uniform_int_distribution<size_t> dist(0, data_size - 1);
                
                for (size_t i = 0; i < iterations; ++i) {
                    size_t index = dist(rng);
                    array.set(index, i % 2 == 0); // 交替设置true/false
                }
            } catch (const std::exception& e) {
                std::cerr << "写入线程 " << thread_id << " 错误: " << e.what() << std::endl;
                std::terminate();
            }
        };
        
        // 读取函数
        auto read_func = [&]() {
            try {
                std::mt19937 rng(num_write_threads + data_size);
                std::uniform_int_distribution<size_t> dist(0, data_size - 1);
                size_t index = 0;
                for (size_t i = 0; i < iterations; ++i) {
                    volatile bool value = array.get(index); // volatile防止优化
                    index += 8;
                    if(index >= data_size){
                        index = index%8+1;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "读取线程错误: " << e.what() << std::endl;
                std::terminate();
            }
        };
        
        // 计时开始
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // 创建写入线程
        std::vector<std::thread> write_threads;
        for (size_t i = 0; i < num_write_threads; ++i) {
            write_threads.emplace_back(write_func, i);
        }
        
        // 单线程读取
        std::thread read_thread(read_func);
        
        // 等待所有线程完成
        for (auto& t : write_threads) {
            t.join();
        }
        read_thread.join();
        
        // 计时结束
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        
        return elapsed.count();
    } catch (const std::exception& e) {
        std::cerr << "数组测试错误: " << e.what() << std::endl;
        return -1;
    }
}

int main() {
    // 测试参数
    const size_t data_size = 128;           // 数据大小
    const size_t num_write_threads = 16;  // 写入线程数量
    const size_t iterations = 1000000;   // 每个线程的操作次数
    const int test_rounds = 5;            // 测试轮次，取平均值
    
    // 参数检查
    if (data_size == 0) {
        std::cerr << "错误: 数据大小不能为0" << std::endl;
        return 1;
    }
    
    if (num_write_threads == 0) {
        std::cerr << "错误: 线程数量不能为0" << std::endl;
        return 1;
    }
    
    std::cout << "测试配置: " << std::endl;
    std::cout << "数据大小: " << data_size << std::endl;
    std::cout << "写入线程数: " << num_write_threads << std::endl;
    std::cout << "每个线程操作次数: " << iterations << std::endl;
    std::cout << "测试轮次: " << test_rounds << std::endl;
    std::cout << "数组元素类型: uint32_t (32位)" << std::endl;
    std::cout << "注意: 线程可以写入相同的元素，可能存在写冲突" << std::endl << std::endl;
    
    // 测试位图
    double bitmap_total = 0;
    for (int i = 0; i < test_rounds; ++i) {
        double time = test_bitmap(data_size, num_write_threads, iterations);
        if (time < 0) {
            return 1;
        }
        bitmap_total += time;
        std::cout << "位图测试轮次 " << i+1 << ": " << time << " 秒" << std::endl;
    }
    double bitmap_avg = bitmap_total / test_rounds;
    
    // 测试数组
    double array_total = 0;
    for (int i = 0; i < test_rounds; ++i) {
        double time = test_array(data_size, num_write_threads, iterations);
        if (time < 0) {
            return 1;
        }
        array_total += time;
        std::cout << "数组测试轮次 " << i+1 << ": " << time << " 秒" << std::endl;
    }
    double array_avg = array_total / test_rounds;
    
    // 输出结果
    std::cout << std::endl << "测试结果: " << std::endl;
    std::cout << "位图平均时间: " << bitmap_avg << " 秒" << std::endl;
    std::cout << "数组平均时间: " << array_avg << " 秒" << std::endl;
    
    if (bitmap_avg < array_avg) {
        std::cout << "位图比数组快 " << (array_avg / bitmap_avg) << " 倍" << std::endl;
    } else {
        std::cout << "数组比位图快 " << (bitmap_avg / array_avg) << " 倍" << std::endl;
    }
    
    return 0;
}
