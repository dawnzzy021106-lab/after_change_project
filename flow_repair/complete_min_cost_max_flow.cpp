#include <iostream>
#include <vector>
#include <deque>
#include <queue>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <chrono>
#include <unordered_set>
#include <iomanip>
#include <unordered_map>
#include <numeric>
#include <climits>
#include <fstream>
#include <sstream>
#include <utility>


using namespace std;

// 读取Available_matrix文件的函数
vector<vector<int>> readAvailableMatrix(const string& filename) {
    vector<vector<int>> matrix;
    ifstream file(filename);
    
    if (!file.is_open()) {
        cerr << "Error: Cannot open file " << filename << endl;
        cerr << "Make sure ClusterSR.cpp has been run first to generate Available_matrix" << endl;
        exit(1);
    }
    
    string line;
    bool readingMatrix = false;
    
    while (getline(file, line)) {
        // 去除行首尾的空格
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        
        // 如果遇到"Available = {"，开始读取
        if (line.find("Available = {") != string::npos) {
            readingMatrix = true;
            continue;
        }
        
        // 如果遇到"};"，结束读取
        if (line == "};" || line == "    };" || line == "};") {
            break;
        }
        
        // 如果是空行，跳过
        if (line.empty()) {
            continue;
        }
        
        // 如果正在读取矩阵内容
        if (readingMatrix) {
            // 移除行首尾的大括号和可能的逗号
            if (line.back() == ',') {
                line.pop_back();  // 移除尾部的逗号
            }
            
            // 检查是否是大括号格式
            if (line.front() == '{' && line.back() == '}') {
                // 移除大括号
                line = line.substr(1, line.length() - 2);
                
                vector<int> row;
                stringstream ss(line);
                string token;
                
                // 按逗号分割
                while (getline(ss, token, ',')) {
                    // 去除可能的空格
                    token.erase(remove(token.begin(), token.end(), ' '), token.end());
                    if (!token.empty()) {
                        row.push_back(stoi(token));
                    }
                }
                
                if (!row.empty()) {
                    matrix.push_back(row);
                }
            }
        }
    }
    
    file.close();
    
    // 验证读取结果
    if (matrix.empty()) {
        cerr << "Warning: No data read from Available_matrix" << endl;
        cerr << "File format might be incorrect" << endl;
    } else {
        cout << "Successfully read Available matrix: " 
             << matrix.size() << " rows x " 
             << matrix[0].size() << " columns" << endl;
    }
    
    return matrix;
}

// 存储修复方案
void saveClusterData(const std::string& filename,
                     const std::vector<int>& main_help_clusterID,
                     const std::vector<std::vector<std::pair<int, int>>>& other_help_clusterID_chunkNum_pairs) {
    
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "错误: 无法打开文件 " << filename << " 进行写入" << std::endl;
        return;
    }
    
    // 1. 写入main_help_clusterID数组
    size_t main_size = main_help_clusterID.size();
    file.write(reinterpret_cast<const char*>(&main_size), sizeof(main_size));
    file.write(reinterpret_cast<const char*>(main_help_clusterID.data()), 
               main_size * sizeof(int));
    
    // 2. 写入other_help_clusterID_chunkNum_pairs
    size_t outer_size = other_help_clusterID_chunkNum_pairs.size();
    file.write(reinterpret_cast<const char*>(&outer_size), sizeof(outer_size));
    
    for (const auto& inner_vec : other_help_clusterID_chunkNum_pairs) {
        size_t inner_size = inner_vec.size();
        file.write(reinterpret_cast<const char*>(&inner_size), sizeof(inner_size));
        
        // 写入pair数组
        for (const auto& p : inner_vec) {
            file.write(reinterpret_cast<const char*>(&p.first), sizeof(p.first));
            file.write(reinterpret_cast<const char*>(&p.second), sizeof(p.second));
        }
    }
    
    file.close();
    std::cout << "集群数据已保存到: " << filename << std::endl;
}

class DynamicCostMaxFlow {
private:
    int n;  // 节点数量
    vector<vector<vector<double>>> graph;  // [u][idx][0]=v, [1]=cap, [2]=cost, [3]=rev
    double epsilon;  // 惩罚参数
    vector<vector<double>> original_flow;  // 记录原始图中的流量

public:
    DynamicCostMaxFlow(int n) : n(n), epsilon(1.0) {
        graph.resize(n);
        original_flow.resize(n, vector<double>(n, 0.0));
    }

    // add_edge(节点1, 节点2, 容量, 单位费用)
    void add_edge(int u, int v, double cap, double cost) {
        // 添加正向边
        vector<double> forward = {(double)v, cap, cost, (double)graph[v].size()};
        // 添加反向边
        vector<double> backward = {(double)u, 0.0, -cost, (double)graph[u].size()};
        graph[u].push_back(forward);
        graph[v].push_back(backward);
    }

    // Bellman-Ford算法找到从s到t的最小费用路径
    pair<vector<pair<int, int>>, double> bellman_ford(int s, int t) {
        vector<double> dist(n, DBL_MAX);
        vector<int> parent(n, -1);
        vector<int> parent_edge(n, -1);
        dist[s] = 0.0;

        // 松弛n-1次
        for (int iter = 0; iter < n - 1; ++iter) {
            bool updated = false;
            for (int u = 0; u < n; ++u) {
                if (dist[u] == DBL_MAX) continue;
                for (int idx = 0; idx < graph[u].size(); ++idx) {
                    int v = (int)graph[u][idx][0];
                    double cap = graph[u][idx][1];
                    double cost = graph[u][idx][2];
                    
                    if (cap > 0 && dist[u] + cost < dist[v]) {
                        dist[v] = dist[u] + cost;
                        parent[v] = u;
                        parent_edge[v] = idx;
                        updated = true;
                    }
                }
            }
            if (!updated) break;
        }

        // 如果没有路径到达t
        if (dist[t] == DBL_MAX) {
            return make_pair(vector<pair<int, int>>(), 0.0);
        }

        // 重建路径并找到最小容量
        vector<pair<int, int>> path;
        double min_cap = DBL_MAX;
        int cur = t;
        unordered_set<int> visited;  // 检测循环

        while (cur != s) {
            // 检查是否出现循环
            if (visited.find(cur) != visited.end()) {
                cout << "Warning: Cycle detected at node " << cur << endl;
                return make_pair(vector<pair<int, int>>(), 0.0);
            }
            visited.insert(cur);

            // 检查前驱节点是否有效
            if (parent[cur] == -1) {
                cout << "Error: Node " << cur << " has no parent" << endl;
                return make_pair(vector<pair<int, int>>(), 0.0);
            }

            int u = parent[cur];
            int idx = parent_edge[cur];

            // 检查边是否存在且容量足够
            if (idx < 0 || idx >= graph[u].size()) {
                cout << "Error: Edge index " << idx << " out of range" << endl;
                return make_pair(vector<pair<int, int>>(), 0.0);
            }

            double cap = graph[u][idx][1];
            if (cap <= 0) {
                cout << "Error: Edge " << u << "->" << cur << " has capacity " << cap << endl;
                return make_pair(vector<pair<int, int>>(), 0.0);
            }

            min_cap = min(min_cap, cap);
            path.push_back(make_pair(u, cur));
            cur = u;

            // 防止无限循环的安全检查
            if (path.size() > n) {
                cout << "Error: Path length exceeds node count" << endl;
                return make_pair(vector<pair<int, int>>(), 0.0);
            }
        }

        reverse(path.begin(), path.end());
        return make_pair(path, min_cap);
    }

    // 最小费用最大流算法
    pair<double, double> min_cost_max_flow(int s, int t, int num_r) {
        double total_flow = 0.0;
        double total_cost = 0.0;
        int iteration = 0;

        while (true) {
            iteration++;
            // cout << "\n=== Iteration " << iteration << " ===" << endl;

            // 找到最小费用路径
            auto result = bellman_ford(s, t);
            vector<pair<int, int>> path = result.first;
            double min_cap = result.second;

            if (path.empty()) {
                // cout << "No augmenting path found, algorithm terminated" << endl;
                break;
            }

            // cout << "Found augmenting path: ";
            // for (auto& p : path) {
            //     cout << "(" << p.first << "-" << p.second << ") ";
            // }
            // cout << ", augmenting cap: " << min_cap << endl;

            // 沿路径增广流量
            total_flow += min_cap;
            unordered_set<int> affected_ri;  // 记录本次增广路径中使用的Ri节点

            for (auto& edge : path) {
                int u = edge.first;
                int v = edge.second;

                // 找到对应的边索引
                int idx = -1;
                for (int i = 0; i < graph[u].size(); ++i) {
                    if ((int)graph[u][i][0] == v) {
                        idx = i;
                        break;
                    }
                }

                if (idx == -1) continue;

                // 更新正向边：容量-
                graph[u][idx][1] -= min_cap;
                // 更新反向边：容量+
                int rev_idx = (int)graph[u][idx][3];
                graph[v][rev_idx][1] += min_cap;

                // 累加费用
                double edge_cost = graph[u][idx][2];
                total_cost += min_cap * edge_cost;

                // 记录原始图中的流量
                if (u != v) {  // 避免自环
                    original_flow[u][v] += min_cap;
                }

                // 记录路径中的Ri节点
                if (u == 0 && v >= 1 && v <= num_r) {  // Vs到Ri的边
                    affected_ri.insert(v);
                }
            }

            // 增加路径中Vs->Ri边的费用
            for (int ri : affected_ri) {
                for (auto& edge : graph[0]) {  // Vs-Ri
                    if ((int)edge[0] == ri) {
                        // 更新正向边费用
                        double old_cost = edge[2];
                        edge[2] += epsilon;
                        // 更新反向边费用（保持费用为负）
                        // int rev_idx = (int)edge[3];
                        // graph[ri][rev_idx][2] = -edge[2];
                        // int rev_idx = (int)edge[3];
                        // double old_rev_cost = graph[ri][rev_idx][2];
                        // graph[ri][rev_idx][2] = old_rev_cost - epsilon;
                        // cout << "Increase cost of edge Vs->R" << ri << " : " << old_cost << " -> " << edge[2] << endl;
                        // cout << "Increase cost of edge R" << ri << "->Vs : " << old_rev_cost << " -> " << graph[ri][rev_idx][2] << endl;
                        break;
                    }
                }
            }

            // cout << "Current total flow: " << total_flow << ", Current total cost: " << total_cost << endl;
        }

        return make_pair(total_flow, total_cost);
    }

    // 获取原始流量矩阵
    vector<vector<double>> get_original_flow() {
        return original_flow;
    }

    // 获取图
    vector<vector<vector<double>>> get_graph() {
        return graph;
    }
};



int main() {
    int k = 10;
    int m = 4;

    // 原始Available矩阵：num_v*num_r（条带数*机架数）
    vector<vector<int>> Available = readAvailableMatrix("/home/hadoop/zzy/ec_prototype-master/flow_repair/Available_matrix");

    // 验证矩阵
    if (Available.empty()) {
        cerr << "Error: Failed to read Available matrix" << endl;
        return 1;
    }
    // 确保列数一致
    int cols = Available[0].size();
    for (int i = 1; i < Available.size(); i++) {
        if (Available[i].size() != cols) {
            cerr << "Error: Inconsistent column size at row " << i << endl;
            return 1;
        }
    }

    // cout << "=== First search ===" << endl;
    // cout << "=== Original Available Matrix ===" << endl;
    // for (const auto& row : Available) {
    //     for (int val : row) {
    //         cout << val << " ";
    //     }
    //     cout << endl;
    // }
    
    // 处理Available矩阵，将值=m的项改为0，生成requestor_available矩阵
    vector<vector<int>> requestor_available = Available;
    for (auto& row : requestor_available) {
        for (auto& val : row) {
            if (val == m) {
                val = -1;
            }
        }
    }
    
    // cout << "\n=== Processed requestor_available Matrix (m=" << m << ") ===" << endl;
    // for (const auto& row : requestor_available) {
    //     for (int val : row) {
    //         cout << val << " ";
    //     }
    //     cout << endl;
    // }

    int num_v = requestor_available.size();  // Vj 的数量
    int num_r = requestor_available[0].size();  // Ri 的数量
    int n = 2 + num_v + num_r;  // 源汇节点+Ri+Vj

    // cout << "\n=== First Search Graph Structure ===" << endl;
    DynamicCostMaxFlow mf1(n);
    
    // Vs -> Ri: 容量 = num_v, 费用 = 1
    for (int i = 0; i < num_r; ++i) {
        mf1.add_edge(0, i + 1, num_v, 1);
        // cout << "Vs -> R" << i+1 << ": capacity=" << num_v << ", cost=1" << endl;
    }

    // Ri -> Vj: 容量 = 1, 费用 = -avail + m + 1
    for (int j = 0; j < num_v; ++j) {
        for (int i = 0; i < num_r; ++i) {
            int avail = requestor_available[j][i];
            if (avail >= 0) {
                double cost = -((double)avail) + m + 1;
                mf1.add_edge(i + 1, j + 1 + num_r, 1, cost);
                // cout << "R" << i+1 << " -> V" << j+1 << ": capacity=1, cost=" << fixed << setprecision(3) << cost << endl;
            }
        }
    }
    
    // Vj -> Vt: 容量 = 1, 费用 = 1
    for (int j = 0; j < num_v; ++j) {
        mf1.add_edge(j + 1 + num_r, n - 1, 1, 1);
        // cout << "V" << j+1 << " -> Vt: capacity=1, cost=1" << endl;
    }
    
    // cout << "\n=== Starting First Minimum Cost Maximum Flow Calculation ===" << endl;
    // 计算最小费用最大流
    auto start_time1 = chrono::high_resolution_clock::now();
    auto result1 = mf1.min_cost_max_flow(0, n - 1, num_r);
    auto end_time1 = chrono::high_resolution_clock::now();
    double flow1 = result1.first;
    double cost1 = result1.second;
    double time1 = chrono::duration<double>(end_time1 - start_time1).count();

    // cout << fixed << setprecision(6);
    // cout << "\n=== First Search Results ===" << endl;
    // cout << "Maximum flow: " << flow1 << endl;
    // cout << "Total cost: " << cost1 << endl;
    
    // 输出第一次搜索的流分配情况
    // cout << "\n=== First Search Flow Distribution ===" << endl;
    
    // 重新计算净流量：初始容量 - 剩余容量
    vector<pair<int, int>> used_edges;

    // Vs到Ri的流
    // cout << "Vs to Ri flow:" << endl;
    for (int i = 1; i <= num_r; ++i) {
        // 找到Vs->Ri边的初始容量和当前容量
        int initial_cap = num_v;
        double current_cap = 0;
        auto graph0 = mf1.get_graph()[0];
        for (auto& edge : graph0) {
            if ((int)edge[0] == i) {
                current_cap = edge[1];
                break;
            }
        }
        double flow_amount = initial_cap - current_cap;
        if (flow_amount > 0) {
            // cout << "  Vs -> R" << i << ": " << flow_amount << endl;
        }
    }
    
    // Ri到Vj的流
    // cout << "\nRi to Vj flow:" << endl;
    for (int j = 0; j < num_v; ++j) {
        for (int i = 0; i < num_r; ++i) {
            int vj_node = j + 1 + num_r;
            int ri_node = i + 1;
            
            // 找到Ri->Vj边的初始容量和当前容量
            // !!!!
            int initial_cap = (requestor_available[j][i] >= 0) ? 1 : 0;
            double current_cap = 0;
            auto graph_ri = mf1.get_graph()[ri_node];
            for (auto& edge : graph_ri) {
                if ((int)edge[0] == vj_node) {
                    current_cap = edge[1];
                    break;
                }
            }
            
            double flow_amount = initial_cap - current_cap;
            if (flow_amount > 0) {
                used_edges.push_back(make_pair(ri_node, vj_node));
                // cout << "  R" << ri_node << " -> V" << vj_node-num_r << ": " << flow_amount << endl;
            }
        }
    }
    
    // Vj到Vt的流
    // cout << "\nVj to Vt flow:" << endl;
    for (int j = 0; j < num_v; ++j) {
        int vj_node = j + 1 + num_r;
        int initial_cap = 1;
        double current_cap = 0;
        auto graph_vj = mf1.get_graph()[vj_node];
        for (auto& edge : graph_vj) {
            if ((int)edge[0] == n-1) {
                current_cap = edge[1];
                break;
            }
        }
        double flow_amount = initial_cap - current_cap;
        if (flow_amount > 0) {
            // cout << "  V" << j+1 << " -> Vt: " << flow_amount << endl;
        }
    }
    
    // 统计第一次搜索中每个Ri使用的边数
    // cout << "\n=== Number of edges used by each Ri (net flow) ===" << endl;
    vector<int> ri_edge_count(num_r + 1, 0);
    for (auto& edge : used_edges) {
        ri_edge_count[edge.first]++;
    }
    
    for (int i = 1; i <= num_r; ++i) {
        // cout << "  R" << i << ": " << ri_edge_count[i] << " edges" << endl;
    }
    
    // 计算第一次搜索中边数最大值
    int max_edges1 = 0;
    for (int i = 1; i <= num_r; ++i) {
        if (ri_edge_count[i] > max_edges1) {
            max_edges1 = ri_edge_count[i];
        }
    }
    // cout << "\nMaximum number of edges from Ri to Vj in first search: " << max_edges1 << endl;
    
    // 记录第一次搜索中流量为1的Ri-Vj边（净流量）
    // cout << "\n=== Ri-Vj edges with flow 1 in first search (net flow) ===" << endl;
    // for (auto& edge : used_edges) {
    //     cout << "  R" << edge.first << " -> V" << edge.second - num_r << endl;
    // }
    // ---------------------------ec_prototype-------------------------------
    int main_parition_idx = -1;
    std::vector<int> main_help_clusterID;
    for (auto& edge : used_edges) {
        main_parition_idx = edge.first - 1;
        main_help_clusterID.push_back(main_parition_idx); // main_help_clusterID[2]=0：第3个待修复条带的目的机架为R1
    }
    // cout << "\n=== ec_prototype : main_help_clusterID ===" << endl;
    int temp_stripeID = 0;
    for (auto& clusterID : main_help_clusterID) {
        // cout << " stripe " << temp_stripeID << " 's main_help_clusterID = " << clusterID << endl;
        temp_stripeID++;
    }
    // 写入文件：


    // ---------------------------ec_prototype-------------------------------
    
    // 第二次搜索 - 构建新图
    // cout << "\n\n=== Second Search ===" << endl;
    
    // 计算remain_available向量
    vector<int> remain_available(num_v, 0);
    for (int j = 0; j < num_v; ++j) {
        // 计算每个Vj在第一次搜索中使用的边的容量总和
        int used_count = 0;
        for (auto& edge : used_edges) {
            if (edge.second == (j + num_r + 1)) {
                // 获取对应的矩阵位置
                int vj_index = j;
                int ri_index = edge.first - 1;
                // 累加Available矩阵中对应位置的值
                used_count += Available[vj_index][ri_index];
            }
        }
        remain_available[j] = k - used_count;
        // cout << "V" << j+1 << " remaining needed blocks: " << remain_available[j] << endl;
    }
    
    // 构建第二次搜索的图
    DynamicCostMaxFlow mf2(n);
    
    // cout << "Second search availability matrix:" << endl;
    
    // 创建第二次搜索的可用矩阵（在原始Available基础上减去已使用的边）
    vector<vector<int>> second_available = Available;
    for (auto& edge : used_edges) {
        // 将used_edges中的边在second_available中对应位置设为0
        int vj_index = edge.second - num_r - 1;
        int ri_index = edge.first - 1;
        if (second_available[vj_index][ri_index] > 0) {
            second_available[vj_index][ri_index] = 0;
        }
    }

    // cout << "\n=== Second Search Graph Structure ===" << endl;
    // for (const auto& row : second_available) {
    //     for (int val : row) {
    //         cout << val << " ";
    //     }
    //     cout << endl;
    // }

    // Vs -> Ri: 容量 = second_available矩阵中Ri对应列之和，费用 = 1
    for (int i = 1; i <= num_r; ++i) {
        // 计算该Ri在second_available矩阵中对应列的和
        int ri_column_sum = 0;
        for (int j = 0; j < num_v; ++j) {
            ri_column_sum += second_available[j][i-1];
        }
        if (ri_column_sum > 0) {
            mf2.add_edge(0, i, ri_column_sum, 1);
            // cout << "Vs -> R" << i << ": capacity=" << ri_column_sum << ", cost=1" << endl;
        }
    }
    
    // Ri -> Vj: 容量 = second_available矩阵中Vj对应行Ri对应列位置的值
    for (int j = 0; j < num_v; ++j) {
        for (int i = 1; i <= num_r; ++i) {
            int avail = second_available[j][i-1];
            if (avail > 0) {
                double cost = -((double)avail) + m + 1;
                mf2.add_edge(i, j + 1 + num_r, avail, cost);
                // cout << "R" << i << " -> V" << j+1 << ": capacity=" << avail << ", cost=" << fixed << setprecision(3) << cost << endl;
            }
        }
    }
    
    // Vj -> Vt: 容量 = remain_available[j], 费用 = 1
    for (int j = 0; j < num_v; ++j) {
        if (remain_available[j] > 0) {
            mf2.add_edge(j + 1 + num_r, n-1, remain_available[j], 1);
            // cout << "V" << j+1 << " -> Vt: capacity=" << remain_available[j] << ", cost=1" << endl;
        }
    }
    
    // cout << "\n===== 第二次建图后 mf2 的完整边表 =====" << endl;
    auto graph_mf2 = mf2.get_graph();
    for (int u = 0; u < n; ++u) {
        for (int idx = 0; idx < graph_mf2[u].size(); ++idx) {
            int v = (int)graph_mf2[u][idx][0];
            double cap = graph_mf2[u][idx][1];
            double cost = graph_mf2[u][idx][2];
            int rev_idx = (int)graph_mf2[u][idx][3];
            // cout << "node" << setw(2) << u << " [" << idx << "] -> " << setw(2) << v << "  cap=" << setw(3) << cap << "  cost=" << fixed << setw(6) << setprecision(2) << cost << "  rev=" << rev_idx << endl;
        }
    }
    
    // cout << "\n=== Starting Second Minimum Cost Maximum Flow Calculation ===" << endl;
    // 计算第二次最小费用最大流
    auto start_time2 = chrono::high_resolution_clock::now();
    auto result2 = mf2.min_cost_max_flow(0, n-1, num_r);
    auto end_time2 = chrono::high_resolution_clock::now();
    double flow2 = result2.first;
    double cost2 = result2.second;
    double time2 = chrono::duration<double>(end_time2 - start_time2).count();
    double total_time = time1 + time2;

    // cout << "\n=== Second Search Results ===" << endl;
    // cout << "Maximum flow: " << flow2 << endl;
    // cout << "Total cost: " << cost2 << endl;
    
    // 输出第二次搜索的流分配情况
    // cout << "\n=== Second Search Flow Distribution ===" << endl;
    
    // 计算第二次搜索的净流量
    vector<pair<int, int>> second_used_edges;

    // Vs到Ri的流
    // cout << "Vs to Ri flow:" << endl;
    for (int i = 1; i <= num_r; ++i) {
        // 计算初始容量：second_available矩阵中Ri对应列的和
        int initial_cap = 0;
        for (int j = 0; j < num_v; ++j) {
            initial_cap += second_available[j][i-1];
        }
        double current_cap = 0;
        auto graph0_mf2 = mf2.get_graph()[0];
        for (auto& edge : graph0_mf2) {
            if ((int)edge[0] == i) {
                current_cap = edge[1];
                break;
            }
        }
        double flow_amount = initial_cap - current_cap;
        if (flow_amount > 0) {
            // cout << "  Vs -> R" << i << ": " << flow_amount << endl;
        }
    }
    
    // ---------------------------ec_prototype-------------------------------
    std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs; 

    // ---------------------------ec_prototype-------------------------------

    // Ri到Vj的流
    // cout << "\nRi to Vj flow (net flow):" << endl;
    for (int j = 0; j < num_v; ++j) {
        // ---------------------------ec_prototype-------------------------------
        std::vector<std::pair<int, int>> other_help_clusterID_chunkNum;
        int flag = -1;
        // ---------------------------ec_prototype-------------------------------
        for (int i = 1; i <= num_r; ++i) {
            int vj_node = j + 1 + num_r;
            // 计算初始容量
            int initial_cap = second_available[j][i-1];
            double current_cap = 0;
            auto graph_i_mf2 = mf2.get_graph()[i];
            for (auto& edge : graph_i_mf2) {
                if ((int)edge[0] == vj_node) {
                    current_cap = edge[1];
                    break;
                }
            }
            double flow_amount = initial_cap - current_cap;
            if (flow_amount > 0) {
                second_used_edges.push_back(make_pair(i, vj_node));
                // cout << "  R" << i << " -> V" << j+1 << ": " << flow_amount << endl;
                // ---------------------------ec_prototype-------------------------------
                flag = 1;
                other_help_clusterID_chunkNum.push_back(make_pair(i-1, flow_amount));   // R1中读取3个可用块对应push_back(pair{0,3})
                // ---------------------------ec_prototype-------------------------------
            }
        }
        // ---------------------------ec_prototype-------------------------------
        if (flag == 1) {
            other_help_clusterID_chunkNum_pairs.push_back(other_help_clusterID_chunkNum);    
        } else {
            // 【修改前】
            // other_help_clusterID_chunkNum_pairs.push_back({{-1,-1}}); 
            
            // 【修改后】直接推入一个空数组代表无需额外块，防止下游拿到 -1 去做越界索引
            other_help_clusterID_chunkNum_pairs.push_back(std::vector<std::pair<int, int>>());
        }
        // ---------------------------ec_prototype-------------------------------
    }

    // ---------------------------ec_prototype-------------------------------
    for (size_t i = 0; i < other_help_clusterID_chunkNum_pairs.size(); ++i) {
        // std::cout << "Stripe " << i << " 's {help_cluster, chunk_num} vector: " ;
        
        for (size_t j = 0; j < other_help_clusterID_chunkNum_pairs[i].size(); ++j) {
            const auto& p = other_help_clusterID_chunkNum_pairs[i][j];
            // std::cout << "(" << p.first << ", " << p.second << ") ";
        }
        
        if (other_help_clusterID_chunkNum_pairs[i].empty()) {
            // std::cout << "null";
        }
        // std::cout << std::endl;
    }
    // 写入文件：
    saveClusterData("/home/hadoop/zzy/ec_prototype-master/flow_repair/cluster_data.bin", main_help_clusterID, other_help_clusterID_chunkNum_pairs);

    // ---------------------------ec_prototype-------------------------------
    
    // Vj到Vt的流
    // cout << "\nVj to Vt flow:" << endl;
    for (int j = 0; j < num_v; ++j) {
        int vj_node = j + 1 + num_r;
        int initial_cap = remain_available[j];
        double current_cap = 0;
        auto graph_vj_mf2 = mf2.get_graph()[vj_node];
        for (auto& edge : graph_vj_mf2) {
            if ((int)edge[0] == n-1) {
                current_cap = edge[1];
                break;
            }
        }
        double flow_amount = initial_cap - current_cap;
        if (flow_amount > 0) {
            // cout << "  V" << j+1 << " -> Vt: " << flow_amount << endl;
        }
    }
    
    // 统计第二次搜索中每个Ri使用的边数（基于净流量）
    // cout << "\n=== Number of edges used by each Ri in second search (net flow) ===" << endl;
    vector<int> ri_edge_count2(num_r + 1, 0);
    for (auto& edge : second_used_edges) {
        ri_edge_count2[edge.first]++;
    }
    
    for (int i = 1; i <= num_r; ++i) {
        // cout << "  R" << i << ": " << ri_edge_count2[i] << " edges" << endl;
    }
    
    // 计算第二次搜索中边数最大值
    int max_edges2 = 0;
    for (int i = 1; i <= num_r; ++i) {
        if (ri_edge_count2[i] > max_edges2) {
            max_edges2 = ri_edge_count2[i];
        }
    }
    // cout << "\nMaximum number of edges from Ri to Vj in second search: " << max_edges2 << endl;
    
    // 综合结果
    cout << "\n\n=== Combined Results ===" << endl;
    cout << "First search maximum flow: " << flow1 << ", Second search maximum flow: " << flow2 << endl;
    cout << "First search total cost: " << cost1 << ", Second search total cost: " << cost2 << endl;
    cout << "First search max edges: " << max_edges1 << ", Second search max edges: " << max_edges2 << endl;
    cout << "First search time: " << time1 << " seconds, Second search time: " << time2 << " seconds" << endl;
    cout << "Total computation time: " << total_time << " seconds" << endl;
    
    // cout << "\n\n=== Final Statistics ===" << endl;
    
    // 统计每个Ri到任意Vj的流的数量（OUT向量）
    vector<int> OUT(num_r, 0);
    auto original_flow2 = mf2.get_original_flow();
    for (int i = 1; i <= num_r; ++i) {
        int edge_count = 0;
        for (int j = 1 + num_r; j < 1 + num_r + num_v; ++j) {
            if (original_flow2[i][j] > 0) {
                edge_count++;
            }
        }
        OUT[i-1] = edge_count;
        // cout << "OUT vector - R" << i << ": " << edge_count << endl;
    }
    
    // 统计每个Vj的数量，按照第一次搜索中流量为1的Ri-Vj边确定存放位置
    vector<int> IN(num_r, 0);
    // 建立Vj到Ri的映射（基于第一次搜索）
    unordered_map<int, int> vj_to_ri_map;
    for (auto& edge : used_edges) {
        int vj_index = edge.second - num_r - 1;
        vj_to_ri_map[vj_index] = edge.first - 1;
    }
    
    // 统计第二次搜索中每个Vj的流入流量数量
    for (int j = 0; j < num_v; ++j) {
        int vj_node = j + 1 + num_r;
        int edge_count = 0;
        for (int i = 1; i <= num_r; ++i) {
            if (original_flow2[i][vj_node] > 0) {
                edge_count++;
            }
        }
        
        // 根据第一次搜索的映射确定在IN向量中的位置
        if (vj_to_ri_map.find(j) != vj_to_ri_map.end()) {
            int ri_index = vj_to_ri_map[j];
            IN[ri_index] += edge_count;
            // cout << "IN vector - V" << j+1 << " (corresponds to R" << ri_index+1 << "): " << edge_count << endl;
        } else {
            // cout << "Note: V" << j+1 << " has no corresponding Ri in first search" << endl;
        }
    }
    
    cout << "\nIN vector: ";
    for (int val : IN) cout << val << " ";
    cout << endl;
    
    cout << "\nOUT vector: ";
    for (int val : OUT) cout << val << " ";
    cout << endl;
    
    int max_IN = *max_element(IN.begin(), IN.end());
    int max_OUT = *max_element(OUT.begin(), OUT.end());
    int max_value = max(max_IN, max_OUT);
    int sum_IN = accumulate(IN.begin(), IN.end(), 0);
    
    cout << "Maximum value: " << max_value << endl;
    cout << "Sum: " << sum_IN << endl;
    printf("\nTotal computation time: %.6f seconds\n", total_time);

    return 0;
}