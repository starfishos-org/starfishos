#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sstream>

// 简单的 HTML 表格生成函数
std::string generate_table(int num_of_rows, int num_of_cols) {
    std::stringstream html;
    
    html << "<table xmlns=\"http://www.w3.org/1999/xhtml\" "
         << "xmlns:tal=\"http://xml.zope.org/namespaces/tal\">\n";
    
    // 创建数据
    std::map<std::string, int> data;
    for (int i = 0; i < num_of_cols; ++i) {
        data[std::to_string(i)] = i;
    }
    
    // 生成表格行
    for (int row = 0; row < num_of_rows; ++row) {
        html << "<tr>\n";
        
        // 生成表格列
        for (const auto& pair : data) {
            int c = pair.second;
            int d = c + 1;
            
            html << "  <td>\n"
                 << "    <span class=\"column-" << d << "\">"
                 << d << "</span>\n"
                 << "  </td>\n";
        }
        
        html << "</tr>\n";
    }
    
    html << "</table>";
    return html.str();
}

// 模拟 Lambda 处理函数
std::string lambda_handler(const std::string& event_json) {
    // 简单解析 JSON（实际应用中应使用 JSON 库）
    size_t rows_pos = event_json.find("\"num_of_rows\":");
    size_t cols_pos = event_json.find("\"num_of_cols\":");
    
    if (rows_pos == std::string::npos || cols_pos == std::string::npos) {
        return "{\"error\": \"Invalid input\"}";
    }
    
    // 提取行数和列数
    int num_of_rows = std::stoi(event_json.substr(rows_pos + 14));
    int num_of_cols = std::stoi(event_json.substr(cols_pos + 14));
    
    // 计时开始
    auto start = std::chrono::high_resolution_clock::now();
    
    // 生成表格
    std::string data = generate_table(num_of_rows, num_of_cols);
    
    // 计时结束
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> latency = end - start;
    
    // 构建结果 JSON
    std::stringstream result;
    result << "{\"latency\": " << latency.count() 
           << ", \"data\": \"" << data << "\"}";
    
    return result.str();
}

int main() {
    // 示例输入
    std::string event = "{\"num_of_rows\": 10, \"num_of_cols\": 5}";
    
    // 调用处理函数
    std::string result = lambda_handler(event);
    
    // 输出结果
    std::cout << result << std::endl;
    
    return 0;
}
