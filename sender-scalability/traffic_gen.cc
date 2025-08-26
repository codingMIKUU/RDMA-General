#include <vector>
#include <random>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

class CustomRandCdf {
public:
    void setCdf(const std::vector<std::pair<double, double>>& cdf) {
        m_cdf = cdf;
    }
    //根据CDF分布生成一个随机的流量值
    size_t rand() {
        double r = m_dist(m_gen); // 0~1均匀分布
        double lower = 0.0;
        double lower_cdf = 0.0;
        for (size_t i = 0; i < m_cdf.size(); ++i) {
            if (r <= m_cdf[i].second) {
                double upper = m_cdf[i].first;
                double upper_cdf = m_cdf[i].second;
                double interval_cdf = upper_cdf - lower_cdf;
                double interval_r = (r - lower_cdf) / interval_cdf;
                size_t value = static_cast<size_t>(lower + interval_r * (upper - lower) + 0.5); // 四舍五入取整
                if (value < 1) value = 1; // 保证结果至少为1
                return value;
            }
            lower = m_cdf[i].first;
            lower_cdf = m_cdf[i].second;
        }
        return static_cast<size_t>(m_cdf.back().first); // fallback
    }

    //生成有k个元素的流量值的数组，该数组内流量值符合CDF分布。
    std::vector<size_t> sample(size_t k) {
        std::vector<size_t> result;
        result.reserve(k);
        for (size_t i = 0; i < k; ++i) {
            result.push_back(rand());
        }
        return result;
    }

private:
    std::vector<std::pair<double, double>> m_cdf;
    std::random_device m_rd;
    std::mt19937 m_gen{m_rd()};
    std::uniform_real_distribution<double> m_dist{0.0, 1.0};
};

std::vector<std::pair<double, double>> read_cdf_file(const std::string& filename) {
    std::vector<std::pair<double, double>> cdf;
    std::ifstream infile(filename);
    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        double size, cdf_val;
        if (!(iss >> size >> cdf_val)) continue;
        cdf.push_back({size, cdf_val / 100.0}); // 文件是百分比，需转为0~1
    }
    return cdf;
}

int main() {
    std::string filename = "Twitter-cluster12.txt";
    auto cdf = read_cdf_file(filename);

    CustomRandCdf crand;
    crand.setCdf(cdf);
    auto samples = crand.sample(100000);//samples里存放100000个符合cdf分布的流量值，在rdma-general里，可以直接让不同qp的wqe size的值为sample[i]



    
    // 输出不同区间的流量值数量，验证该代码是否有效
   std::vector<size_t> bins(cdf.size(), 0);
    for (auto v : samples) {
        for (size_t i = 0; i < cdf.size(); ++i) {
            double lower = (i == 0) ? 0 : cdf[i-1].first;
            double upper = cdf[i].first;
            if (v > lower && v <= upper) {
                bins[i]++;
                break;
            }
        }
    }
    for (size_t i = 0; i < bins.size(); ++i) {
        double lower = (i == 0) ? 0 : cdf[i-1].first;
        double upper = cdf[i].first;
        std::cout << "(" << lower << "," << upper << "]: " << bins[i] << std::endl;
    }
    // 打印前100个采样结果
    for (size_t i = 0; i < 100; ++i) {
        std::cout << samples[i] << " ";
    }
    std::cout << std::endl;



    std::ofstream outfile("Twitter-cluster12_traffic_size.txt");
    for(auto v : samples){
        outfile << v<<" ";
    }
    return 0;
}